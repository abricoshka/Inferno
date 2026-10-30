/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2017
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "accel/accel-ops.h"
#include "accel/accel-private.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/whpx.h"
#include "system/whpx_int.h"
#include "hw/boards.h"

bool                 whpx_allowed;
WHPXDispatch         whp;
WHV_PARTITION_HANDLE whpx_partition;

void assert_whpx_ok_impl(HRESULT hr, const char* file, unsigned int line, const char* exp)
{
    if (SUCCEEDED(hr)) { return; }

    error_report("Error: %s = 0x%08lx (at %s:%u)", exp, (unsigned long)hr, file, line);
    abort();
}

static bool whpx_load(void)
{
    HMODULE lib = LoadLibraryA("WinHvPlatform.dll");

    if (lib == NULL) {
        error_report("WHPX: WinHvPlatform.dll is missing; enable the Windows Hypervisor Platform feature");
        return false;
    }

#define WHPX_LOAD_FUNCTION(type, name, args)                                      \
    whp.name = (type(WINAPI*) args)(void*)GetProcAddress(lib, #name);             \
    if (whp.name == NULL) {                                                       \
        error_report("WHPX: WinHvPlatform.dll has no " #name "; update Windows"); \
        return false;                                                             \
    }
    WHPX_FUNCTIONS(WHPX_LOAD_FUNCTION)
#undef WHPX_LOAD_FUNCTION

    return true;
}

static void whpx_set_phys_mem(MemoryRegionSection* section, bool add)
{
    MemoryRegion*           area     = section->mr;
    hwaddr                  gpa      = section->offset_within_address_space;
    uint64_t                size     = int128_get64(section->size);
    bool                    writable = !area->readonly && !area->rom_device;
    WHV_MAP_GPA_RANGE_FLAGS flags;

    if (!memory_region_is_ram(area)) {
        // MMIO, which exits.
        if (writable) { return; }
        // A ROM device in MMIO mode.
        if (!memory_region_is_romd(area)) { add = false; }
    }

    // Partial pages are never mapped, so they're emulated.
    if (!QEMU_IS_ALIGNED(gpa | size, qemu_real_host_page_size())) { return; }

    if (!add) {
        // It may never have been mapped.
        whp.WHvUnmapGpaRange(whpx_partition, gpa, size);
        return;
    }

    flags = WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute | (writable ? WHvMapGpaRangeFlagWrite : 0);
    assert_whpx_ok(whp.WHvMapGpaRange(
        whpx_partition, (uint8_t*)memory_region_get_ram_ptr(area) + section->offset_within_region, gpa, size, flags));
}

static void whpx_region_add(MemoryListener* listener, MemoryRegionSection* section)
{
    accel_private_system_region_add(section);
    whpx_set_phys_mem(section, true);
}

static void whpx_region_del(MemoryListener* listener, MemoryRegionSection* section)
{ whpx_set_phys_mem(section, false); }

// No dirty tracking, so all of it may have changed.
static void whpx_log_sync(MemoryListener* listener, MemoryRegionSection* section)
{
    if (memory_region_is_ram(section->mr)) {
        memory_region_set_dirty(section->mr, section->offset_within_region, int128_get64(section->size));
    }
}

static MemoryListener whpx_memory_listener = {
    .name       = "whpx",
    .priority   = MEMORY_LISTENER_PRIORITY_ACCEL,
    .region_add = whpx_region_add,
    .region_del = whpx_region_del,
    .log_sync   = whpx_log_sync,
};

static void whpx_private_map(void* host, hwaddr ipa, uint64_t size, bool writable)
{
    assert_whpx_ok(whp.WHvMapGpaRange(whpx_partition, host, ipa, size,
                                      WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute
                                          | (writable ? WHvMapGpaRangeFlagWrite : 0)));
}

static void whpx_private_unmap(hwaddr ipa, uint64_t size)
{ assert_whpx_ok(whp.WHvUnmapGpaRange(whpx_partition, ipa, size)); }

static const AccelPrivateOps whpx_private_ops = {
    .map   = whpx_private_map,
    .unmap = whpx_private_unmap,
};

static int whpx_accel_init(AccelState* as, MachineState* ms)
{
    WHV_CAPABILITY         cap  = {0};
    WHV_PARTITION_PROPERTY prop = {0};
    int                    ret;

    if (!whpx_load()) { return -ENOSYS; }

    if (FAILED(whp.WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), NULL))
        || !cap.HypervisorPresent)
    {
        error_report("WHPX: the hypervisor isn't running");
        return -ENODEV;
    }

    assert_whpx_ok(whp.WHvCreatePartition(&whpx_partition));
    prop.ProcessorCount = ms->smp.max_cpus;
    assert_whpx_ok(
        whp.WHvSetPartitionProperty(whpx_partition, WHvPartitionPropertyCodeProcessorCount, &prop, sizeof(prop)));
    ret = whpx_arch_configure(ms);
    if (ret != 0) { return ret; }
    assert_whpx_ok(whp.WHvSetupPartition(whpx_partition));

    accel_private_init(&whpx_private_ops);
    memory_listener_register(&whpx_memory_listener, &address_space_memory);
    return whpx_arch_init(ms);
}

static void whpx_accel_class_init(ObjectClass* oc, const void* data)
{
    AccelClass* ac = ACCEL_CLASS(oc);

    ac->name         = "WHPX";
    ac->init_machine = whpx_accel_init;
    ac->allowed      = &whpx_allowed;
}

static const TypeInfo whpx_accel_type = {
    .name       = ACCEL_CLASS_NAME("whpx"),
    .parent     = TYPE_ACCEL,
    .class_init = whpx_accel_class_init,
};

DEFINE_TYPE(whpx_accel_type)
