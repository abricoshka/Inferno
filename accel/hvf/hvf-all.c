/*
 * QEMU Hypervisor.framework support
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "accel/accel-ops.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/hvf.h"
#include "system/hvf_int.h"
#include "hw/core/cpu.h"
#include "hw/boards.h"
#include "trace.h"

bool hvf_allowed;

struct mac_slot
{
    int      present;
    uint64_t size;
    uint64_t gpa_start;
    uint64_t gva;
};

struct mac_slot mac_slots[HVF_MAX_SLOTS];

const char* hvf_return_string(hv_return_t ret)
{
    switch (ret) {
        case HV_SUCCESS     : return "HV_SUCCESS";
        case HV_ERROR       : return "HV_ERROR";
        case HV_BUSY        : return "HV_BUSY";
        case HV_BAD_ARGUMENT: return "HV_BAD_ARGUMENT";
        case HV_NO_RESOURCES: return "HV_NO_RESOURCES";
        case HV_NO_DEVICE   : return "HV_NO_DEVICE";
        case HV_UNSUPPORTED : return "HV_UNSUPPORTED";
        case HV_DENIED      : return "HV_DENIED";
        default             : return "[unknown hv_return value]";
    }
}

void assert_hvf_ok_impl(hv_return_t ret, const char* file, unsigned int line, const char* exp)
{
    if (ret == HV_SUCCESS) { return; }

    error_report("Error: %s = %s (0x%x, at %s:%u)", exp, hvf_return_string(ret), ret, file, line);

    abort();
}

/*
 * Pages of a vCPU's own address space that system memory leaves empty, mapped on first access. They are dropped when
 * that address space changes there, its IOMMU invalidates them, or system memory maps something there.
 */
typedef struct HVFPrivateSpace HVFPrivateSpace;

typedef struct
{
    IOMMUNotifier      n;
    IOMMUMemoryRegion* mr;
    HVFPrivateSpace*   space;
} HVFPrivateIOMMU;

struct HVFPrivateSpace
{
    AddressSpace*  as;
    MemoryListener listener;
    GPtrArray*     iommus;
};

typedef struct
{
    hwaddr             ipa;
    HVFPrivateSpace*   space;
    IOMMUMemoryRegion* iommu;
    hwaddr             iova;
} HVFPrivatePage;

typedef struct
{
    HVFPrivateSpace*   space;
    IOMMUMemoryRegion* iommu;
    hwaddr             start;
    hwaddr             last;
    bool               by_iova;
} HVFPrivateRange;

static QemuMutex   hvf_private_lock;
static GPtrArray*  hvf_private_spaces;
static GHashTable* hvf_private_pages;

static gboolean hvf_private_page_drop(gpointer key, gpointer value, gpointer opaque)
{
    HVFPrivatePage*        page  = value;
    const HVFPrivateRange* range = opaque;
    hwaddr                 addr  = range->by_iova ? page->iova : page->ipa;

    if ((range->space != NULL && page->space != range->space) || (range->iommu != NULL && page->iommu != range->iommu)
        || addr + qemu_real_host_page_size() - 1 < range->start || addr > range->last)
    {
        return false;
    }

    assert_hvf_ok(hv_vm_unmap(page->ipa, qemu_real_host_page_size()));
    g_free(page);
    return true;
}

static void hvf_private_drop(HVFPrivateRange range)
{
    QEMU_LOCK_GUARD(&hvf_private_lock);

    g_hash_table_foreach_remove(hvf_private_pages, hvf_private_page_drop, &range);
}

static void hvf_private_drop_section(HVFPrivateSpace* space, MemoryRegionSection* section)
{
    hvf_private_drop((HVFPrivateRange){
        .space = space,
        .start = section->offset_within_address_space,
        .last  = section->offset_within_address_space + int128_get64(section->size) - 1,
    });
}

static void hvf_private_iommu_notify(IOMMUNotifier* n, IOMMUTLBEntry* entry)
{
    HVFPrivateIOMMU* iommu = container_of(n, HVFPrivateIOMMU, n);

    hvf_private_drop((HVFPrivateRange){
        .space   = iommu->space,
        .iommu   = iommu->mr,
        .start   = entry->iova,
        .last    = entry->iova + MIN(entry->addr_mask, HWADDR_MAX - entry->iova),
        .by_iova = true,
    });
}

static void hvf_private_region_del(MemoryListener* listener, MemoryRegionSection* section)
{ hvf_private_drop_section(container_of(listener, HVFPrivateSpace, listener), section); }

static HVFPrivateSpace* hvf_private_space(AddressSpace* as)
{
    HVFPrivateSpace* space;

    for (guint i = 0; i < hvf_private_spaces->len; i++) {
        space = g_ptr_array_index(hvf_private_spaces, i);
        if (space->as == as) { return space; }
    }

    space           = g_new0(HVFPrivateSpace, 1);
    space->as       = as;
    space->iommus   = g_ptr_array_new();
    space->listener = (MemoryListener){
        .name       = "hvf-private",
        .region_del = hvf_private_region_del,
        .priority   = MEMORY_LISTENER_PRIORITY_ACCEL,
    };
    memory_listener_register(&space->listener, as);
    g_ptr_array_add(hvf_private_spaces, space);
    return space;
}

static void hvf_private_watch_iommu(HVFPrivateSpace* space, IOMMUMemoryRegion* mr, int iommu_idx)
{
    HVFPrivateIOMMU* iommu;

    for (guint i = 0; i < space->iommus->len; i++) {
        iommu = g_ptr_array_index(space->iommus, i);
        if (iommu->mr == mr) { return; }
    }

    iommu        = g_new0(HVFPrivateIOMMU, 1);
    iommu->mr    = mr;
    iommu->space = space;
    iommu_notifier_init(&iommu->n, hvf_private_iommu_notify, IOMMU_NOTIFIER_UNMAP, 0, HWADDR_MAX, iommu_idx);
    memory_region_register_iommu_notifier(MEMORY_REGION(mr), &iommu->n, &error_fatal);
    g_ptr_array_add(space->iommus, iommu);
}

bool hvf_map_private_page(AddressSpace* as, hwaddr ipa, bool is_write)
{
    uint64_t            page_size = qemu_real_host_page_size();
    hwaddr              base      = ipa & ~(page_size - 1);
    hv_memory_flags_t   flags     = HV_MEMORY_READ | HV_MEMORY_EXEC;
    IOMMUMemoryRegion*  iommu     = NULL;
    hwaddr              iova      = 0;
    bool                writable  = true;
    MemoryRegionSection section;
    HVFPrivateSpace*    space;
    HVFPrivatePage*     page;

    if (as == &address_space_memory) { return false; }

    WITH_RCU_READ_LOCK_GUARD()
    {
        bool candidate;

        section   = memory_region_find(as->root, base, page_size);
        candidate = section.mr != NULL && (memory_region_is_iommu(section.mr) || memory_region_is_ram(section.mr));
        if (section.mr != NULL) { memory_region_unref(section.mr); }
        if (!candidate) { return false; }
    }

    BQL_LOCK_GUARD();
    QEMU_LOCK_GUARD(&hvf_private_lock);
    RCU_READ_LOCK_GUARD();

    if (g_hash_table_contains(hvf_private_pages, &base)) { return false; }

    section = memory_region_find(get_system_memory(), base, page_size);
    if (section.mr != NULL) {
        memory_region_unref(section.mr);
        return false;
    }

    space   = hvf_private_space(as);
    section = memory_region_find(as->root, base, page_size);
    if (section.mr != NULL && memory_region_is_iommu(section.mr)) {
        int           iommu_idx;
        IOMMUTLBEntry entry;

        iommu     = IOMMU_MEMORY_REGION(section.mr);
        iommu_idx = memory_region_iommu_attrs_to_index(iommu, MEMTXATTRS_UNSPECIFIED);
        iova      = section.offset_within_region;
        hvf_private_watch_iommu(space, iommu, iommu_idx);
        entry = memory_region_get_iommu_class_nocheck(iommu)->translate(iommu, iova, IOMMU_NONE, iommu_idx);
        memory_region_unref(section.mr);
        section.mr = NULL;

        if ((entry.perm & IOMMU_RO) && entry.addr_mask >= page_size - 1) {
            section  = memory_region_find(entry.target_as->root, entry.translated_addr & ~(page_size - 1), page_size);
            writable = entry.perm & IOMMU_WO;
        }
    }
    if (section.mr == NULL) { return false; }

    if (writable && !section.readonly) { flags |= HV_MEMORY_WRITE; }
    if (!memory_region_is_ram(section.mr) || int128_get64(section.size) != page_size
        || (is_write && !(flags & HV_MEMORY_WRITE)))
    {
        memory_region_unref(section.mr);
        return false;
    }

    page        = g_new0(HVFPrivatePage, 1);
    page->ipa   = base;
    page->space = space;
    page->iommu = iommu;
    page->iova  = iova;

    assert_hvf_ok(hv_vm_map((uint8_t*)memory_region_get_ram_ptr(section.mr) + section.offset_within_region, base,
                            page_size, flags));
    memory_region_unref(section.mr);
    g_hash_table_insert(hvf_private_pages, &page->ipa, page);
    return true;
}

static int do_hvf_set_memory(hvf_slot* slot, hv_memory_flags_t flags)
{
    struct mac_slot* macslot;
    hv_return_t      ret;

    macslot = &mac_slots[slot->slot_id];

    if (macslot->present) {
        if (macslot->size != slot->size) {
            macslot->present = 0;
            trace_hvf_vm_unmap(macslot->gpa_start, macslot->size);
            ret = hv_vm_unmap(macslot->gpa_start, macslot->size);
            assert_hvf_ok(ret);
        }
    }

    if (!slot->size) { return 0; }

    macslot->present   = 1;
    macslot->gpa_start = slot->start;
    macslot->size      = slot->size;
    trace_hvf_vm_map(slot->start, slot->size, slot->mem, flags, flags & HV_MEMORY_READ ? 'R' : '-',
                     flags & HV_MEMORY_WRITE ? 'W' : '-', flags & HV_MEMORY_EXEC ? 'X' : '-');
    ret = hv_vm_map(slot->mem, slot->start, slot->size, flags);
    assert_hvf_ok(ret);
    return 0;
}

static void hvf_set_phys_mem(MemoryRegionSection* section, bool add)
{
    hvf_slot*         mem;
    MemoryRegion*     area     = section->mr;
    bool              writable = !area->readonly && !area->rom_device;
    hv_memory_flags_t flags;
    uint64_t          page_size = qemu_real_host_page_size();

    if (!memory_region_is_ram(area)) {
        if (writable) { return; }
        else if (!memory_region_is_romd(area)) {
            /*
             * If the memory device is not in romd_mode, then we actually want
             * to remove the hvf memory slot so all accesses will trap.
             */
            add = false;
        }
    }

    if (!QEMU_IS_ALIGNED(int128_get64(section->size), page_size)
        || !QEMU_IS_ALIGNED(section->offset_within_address_space, page_size))
    {
        /* Not page aligned, so we can not map as RAM */
        add = false;
    }

    mem = hvf_find_overlap_slot(section->offset_within_address_space, int128_get64(section->size));

    if (mem && add) {
        if (mem->size == int128_get64(section->size) && mem->start == section->offset_within_address_space
            && mem->mem == (memory_region_get_ram_ptr(area) + section->offset_within_region))
        {
            return; /* Same region was attempted to register, go away. */
        }
    }

    /* Region needs to be reset. set the size to 0 and remap it. */
    if (mem) {
        mem->size = 0;
        if (do_hvf_set_memory(mem, 0)) {
            error_report("Failed to reset overlapping slot");
            abort();
        }
    }

    if (!add) { return; }

    if (area->readonly || (!memory_region_is_ram(area) && memory_region_is_romd(area))) {
        flags = HV_MEMORY_READ | HV_MEMORY_EXEC;
    }
    else {
        flags = HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC;
    }

    /* Now make a new slot. */
    int x;

    for (x = 0; x < hvf_state->num_slots; ++x) {
        mem = &hvf_state->slots[x];
        if (!mem->size) { break; }
    }

    if (x == hvf_state->num_slots) {
        error_report("No free slots (%d in use); raise HVF_MAX_SLOTS", hvf_state->num_slots);
        abort();
    }

    mem->size   = int128_get64(section->size);
    mem->mem    = memory_region_get_ram_ptr(area) + section->offset_within_region;
    mem->start  = section->offset_within_address_space;
    mem->region = area;

    if (do_hvf_set_memory(mem, flags)) {
        error_report("Error registering new memory slot");
        abort();
    }
}

static void hvf_set_dirty_tracking(MemoryRegionSection* section, bool on)
{
    hvf_slot* slot;

    slot = hvf_find_overlap_slot(section->offset_within_address_space, int128_get64(section->size));

    /* protect region against writes; begin tracking it */
    if (on) {
        slot->flags |= HVF_SLOT_LOG;
        hv_vm_protect((uintptr_t)slot->start, (size_t)slot->size, HV_MEMORY_READ | HV_MEMORY_EXEC);
        /* stop tracking region*/
    }
    else {
        slot->flags &= ~HVF_SLOT_LOG;
        hv_vm_protect((uintptr_t)slot->start, (size_t)slot->size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    }
}

static void hvf_log_start(MemoryListener* listener, MemoryRegionSection* section, int old, int new)
{
    if (old != 0) { return; }

    hvf_set_dirty_tracking(section, 1);
}

static void hvf_log_stop(MemoryListener* listener, MemoryRegionSection* section, int old, int new)
{
    if (new != 0) { return; }

    hvf_set_dirty_tracking(section, 0);
}

static void hvf_log_sync(MemoryListener* listener, MemoryRegionSection* section)
{
    /*
     * sync of dirty pages is handled elsewhere; just make sure we keep
     * tracking the region.
     */
    hvf_set_dirty_tracking(section, 1);
}

static void hvf_region_add(MemoryListener* listener, MemoryRegionSection* section)
{
    hvf_private_drop_section(NULL, section);
    hvf_set_phys_mem(section, true);
}

static void hvf_region_del(MemoryListener* listener, MemoryRegionSection* section) { hvf_set_phys_mem(section, false); }

static MemoryListener hvf_memory_listener = {
    .name       = "hvf",
    .priority   = MEMORY_LISTENER_PRIORITY_ACCEL,
    .region_add = hvf_region_add,
    .region_del = hvf_region_del,
    .log_start  = hvf_log_start,
    .log_stop   = hvf_log_stop,
    .log_sync   = hvf_log_sync,
};

static int hvf_accel_init(AccelState* as, MachineState* ms)
{
    int           x;
    hv_return_t   ret;
    HVFState*     s        = HVF_STATE(as);
    int           pa_range = 36;
    MachineClass* mc       = MACHINE_GET_CLASS(ms);

    if (mc->hvf_get_physical_address_range) {
        pa_range = mc->hvf_get_physical_address_range(ms);
        if (pa_range < 0) { return -EINVAL; }
    }

    ret = hvf_arch_vm_create(ms, (uint32_t)pa_range);
    if (ret == HV_DENIED) {
        error_report("Could not access HVF. Is the executable signed"
                     " with com.apple.security.hypervisor entitlement?");
        exit(1);
    }
    assert_hvf_ok(ret);

    s->num_slots = ARRAY_SIZE(s->slots);
    for (x = 0; x < s->num_slots; ++x) {
        s->slots[x].size    = 0;
        s->slots[x].slot_id = x;
    }

    QTAILQ_INIT(&s->hvf_sw_breakpoints);

    qemu_mutex_init(&hvf_private_lock);
    hvf_private_spaces = g_ptr_array_new();
    hvf_private_pages  = g_hash_table_new(g_int64_hash, g_int64_equal);

    hvf_state = s;
    memory_listener_register(&hvf_memory_listener, &address_space_memory);

    return hvf_arch_init();
}

static int hvf_gdbstub_sstep_flags(AccelState* as) { return SSTEP_ENABLE | SSTEP_NOIRQ; }

static void hvf_accel_class_init(ObjectClass* oc, const void* data)
{
    AccelClass* ac                    = ACCEL_CLASS(oc);
    ac->name                          = "HVF";
    ac->init_machine                  = hvf_accel_init;
    ac->allowed                       = &hvf_allowed;
    ac->gdbstub_supported_sstep_flags = hvf_gdbstub_sstep_flags;
}

static const TypeInfo hvf_accel_type = {
    .name   = TYPE_HVF_ACCEL,
    .parent = TYPE_ACCEL,
    OBJECT_TYPE_INSTANCE(HVFState),
    .class_init = hvf_accel_class_init,
};

static void hvf_type_init(void) { type_register_static(&hvf_accel_type); }

type_init(hvf_type_init);
