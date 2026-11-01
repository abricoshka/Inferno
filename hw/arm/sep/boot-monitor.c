/*
 * Apple SEP Boot Monitor.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "exec/cputlb.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "hw/arm/a13.h"
#include "hw/arm/iboot/kernel_patches.h"
#include "hw/arm/iboot/virt_patches.h"
#include "hw/arm/sep/private.h"
#include "system/address-spaces.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/tcg.h"

#define BOOT_MONITOR_REG_SIZE (0x4000)    // ?

#define BOOT_MONITOR_REG_STATUS          (0x04)
#define BOOT_MONITOR_REG_ERROR           (0x0C)
#define BOOT_MONITOR_REG_LOAD_ADDR       (0x20)
#define BOOT_MONITOR_REG_RANDOMNESS_LO   (0x48)
#define BOOT_MONITOR_REG_RANDOMNESS_HI   (0x4C)
#define BOOT_MONITOR_REG_RANDOMNESS_LOCK (0x50)

struct AppleSEPBootMonitorState
{
    SysBusDevice parent_obj;

    AppleSEP*    sep;
    MemoryRegion boot_monitor_mr;
    uint8_t      boot_monitor_regs[BOOT_MONITOR_REG_SIZE];
};

static uint32_t boot_monitor_reg_get(AppleSEPBootMonitorState* s, hwaddr addr)
{
    assert_cmphex(addr + sizeof(uint32_t), <=, BOOT_MONITOR_REG_SIZE);
    return ldl_le_p(&s->boot_monitor_regs[addr]);
}

static void boot_monitor_reg_set(AppleSEPBootMonitorState* s, hwaddr addr, uint32_t value)
{
    assert_cmphex(addr + sizeof(uint32_t), <=, BOOT_MONITOR_REG_SIZE);
    stl_le_p(&s->boot_monitor_regs[addr], value);
}

static hwaddr boot_monitor_reg_get_64(AppleSEPBootMonitorState* s, hwaddr addr)
{
    assert_cmphex(addr + sizeof(uint64_t), <=, BOOT_MONITOR_REG_SIZE);
    return ldq_le_p(&s->boot_monitor_regs[addr]);
}

static bool boot_monitor_randomness_locked(AppleSEPBootMonitorState* s)
{ return (boot_monitor_reg_get(s, BOOT_MONITOR_REG_RANDOMNESS_LOCK) & BIT(0)) != 0; }

/*
 *   0x08: maybe some command0?
 *         0x2: something about PKA; 0x3: ?; 0x4: during resume;
 *         0x10: during first/cold boot; 0x11: ?; 0x12: ?
 *   0x10: maybe some command1?
 *   0x20/0x24: load address  (0x0000000340000000)
 *   0x28/0x2C: end address   (0x0000000340010000, middle of kernel __text)
 *   0x30/0x34: unknown1 addr (0x000000034004C000 == base of SEPD)
 *   0x38/0x3C: unknown2 addr (0x00000003403D0000 == SEPOS+0xC000, middle of
 *                             __text; 0x0000000340464000 == SEPOS+0x1C000,
 *                             middle of __cstring)
 *   0x40/0x44: unknown0 addr (0x0000000000000000)
 */
static void boot_monitor_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPBootMonitorState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        case BOOT_MONITOR_REG_STATUS:
            // Some status flag; bit0 always reads back clear.
            data &= ~BIT(0);
            break;
        case BOOT_MONITOR_REG_RANDOMNESS_LO:
        case BOOT_MONITOR_REG_RANDOMNESS_HI:
        case BOOT_MONITOR_REG_RANDOMNESS_LOCK:
            if (boot_monitor_randomness_locked(s)) {
                DPRINTF("SEP Boot Monitor: Locked write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr,
                        data);
                return;
            }
            break;
        default:
            DPRINTF("SEP Boot Monitor: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }

    boot_monitor_reg_set(s, addr, (uint32_t)data);
}

static uint64_t boot_monitor_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPBootMonitorState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        case BOOT_MONITOR_REG_STATUS:    // Some status flag, bit0, maybe "is active".
            break;
        case BOOT_MONITOR_REG_ERROR:
            // Must return 0x0. Other possible values: 0x1/0x2/0x3, maybe 0x4.
            // Maybe error codes?
            return 0;
        default: DPRINTF("SEP Boot Monitor: Unknown read at 0x" HWADDR_FMT_plx "\n", addr); break;
    }

    return boot_monitor_reg_get(s, addr);
}

static const MemoryRegionOps boot_monitor_reg_ops = {
    .write                 = boot_monitor_reg_write,
    .read                  = boot_monitor_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

// fully resetting the CPU doesn't work, observed list of registers from real hardware
static void apple_sep_cpu_moni_reset_regs(CPUState* cpu, hwaddr load_addr, hwaddr pwr_dn_save)
{
    ARMCPU*        arm_cpu                   = container_of(cpu, ARMCPU, parent_obj);
    AppleA13State* acpu                      = container_of(arm_cpu, AppleA13State, parent_obj);
    CPUARMState*   env                       = &arm_cpu->env;
    acpu->A13_CPREG_VAR_NAME(ARM64_REG_HID5) = 0x0;
    acpu->A13_CPREG_VAR_NAME(S3_4_c15_c0_5)  = 0x0;
    // clearing ttbr0_el1 is absolutely required for sepfw 26
    // not clearing everything will lead to some tcg/tlb sigsegv
    // not sure whether to only clear _el[1] or the entries of every el
    // but let's clear all. at least "ttbr1_el[1]" seems also to be required.
    memset(env->regs, 0, sizeof(env->regs));
    memset(env->xregs, 0, sizeof(env->xregs));
    memset(env->cp15.ttbr0_el, 0, sizeof(env->cp15.ttbr0_el));
    memset(env->cp15.ttbr1_el, 0, sizeof(env->cp15.ttbr1_el));
    memset(env->cp15.mair_el, 0, sizeof(env->cp15.mair_el));
    memset(env->elr_el, 0, sizeof(env->elr_el));
    // should sp_el be clear/zero before and after?
    memset(env->sp_el, 0, sizeof(env->sp_el));
    memset(env->cp15.esr_el, 0, sizeof(env->cp15.esr_el));
    memset(env->banked_spsr, 0, sizeof(env->banked_spsr));
    memset(env->cp15.tcr_el, 0, sizeof(env->cp15.tcr_el));
    memset(env->cp15.sctlr_el, 0, sizeof(env->cp15.sctlr_el));
    memset(&env->keys, 0, sizeof(env->keys));

    acpu->A13_CPREG_VAR_NAME(SYS_ACC_PWR_DN_SAVE) = pwr_dn_save;
    env->cp15.rvbar                               = load_addr;
    env->cp15.vbar_el[1]                          = load_addr;
    cpu_set_pc(cpu, load_addr);
}

static hwaddr apple_sep_boot_monitor_load_addr(AppleSEPBootMonitorState* s)
{ return boot_monitor_reg_get_64(s, BOOT_MONITOR_REG_LOAD_ADDR); }

// How far after the load address the sepOS images go.
#define SEPOS_SEARCH_SIZE (16 * MiB)

static void apple_sep_boot_monitor_patch_range(AddressSpace* as, const char* name, hwaddr addr, uint64_t size, vaddr va,
                                               bool kernel)
{
    g_autofree uint8_t*        buf   = g_malloc(size);
    g_autofree CKPatcherRange* range = NULL;

    address_space_read(as, addr, MEMTXATTRS_UNSPECIFIED, buf, size);
    range = ck_patcher_range_from_ptr(name, buf, size);
    if (kernel) { ck_patch_sep_timebase(range); }
    ck_patch_virt_unmanaged(range, va);
    address_space_write(as, addr, MEMTXATTRS_UNSPECIFIED, buf, size);
}

// Patches the executable segments of the Mach-O at `base` if it's linked at kernel addresses; returns whether it is.
static bool apple_sep_boot_monitor_patch_macho(AddressSpace* as, hwaddr base, const MachoHeader64* hdr)
{
    g_autofree uint8_t* cmds   = g_malloc(hdr->size_of_cmds);
    uint32_t            off    = 0;
    bool                kernel = false;

    if (address_space_read(as, base + sizeof(*hdr), MEMTXATTRS_UNSPECIFIED, cmds, hdr->size_of_cmds) != MEMTX_OK) {
        return false;
    }
    for (uint32_t i = 0; i < hdr->n_cmds && off + sizeof(MachoLoadCommand) <= hdr->size_of_cmds; i++) {
        MachoSegmentCommand64* seg = (MachoSegmentCommand64*)(cmds + off);
        uint64_t               skip;

        off += seg->cmd_size;
        if (seg->cmd != LC_SEGMENT_64 || off > hdr->size_of_cmds || !(seg->initprot & VM_PROT_EXECUTE)
            || !(seg->vmaddr & BIT_ULL(63)))
        {
            continue;
        }
        // The segment that starts the image holds its header too.
        skip = seg->fileoff == 0 ? ROUND_UP(sizeof(*hdr) + hdr->size_of_cmds, 4) : 0;
        if (skip < seg->filesize) {
            apple_sep_boot_monitor_patch_range(as, "sepOS kernel", base + seg->fileoff + skip, seg->filesize - skip,
                                               seg->vmaddr + skip, true);
            kernel = true;
        }
    }
    return kernel;
}

// Patches the sepOS kernel the SEP ROM has just loaded at `load_addr`: the code before the first Mach-O (all of the
// kernel since sepOS 27) and the Mach-Os linked at kernel addresses. The apps don't need it.
static void apple_sep_boot_monitor_patch_virt(CPUState* cpu, hwaddr load_addr)
{
    AddressSpace* as           = cpu_get_address_space(cpu, ARMASIdx_NS);
    hwaddr        first        = 0;
    bool          kernel_macho = false;
    MachoHeader64 hdr;

    for (hwaddr off = 0; off < SEPOS_SEARCH_SIZE; off += 4 * KiB) {
        if (address_space_read(as, load_addr + off, MEMTXATTRS_UNSPECIFIED, &hdr, sizeof(hdr)) != MEMTX_OK
            || hdr.magic != MACH_MAGIC_64 || hdr.file_type != MH_EXECUTE)
        {
            continue;
        }
        if (first == 0) { first = off; }
        kernel_macho |= apple_sep_boot_monitor_patch_macho(as, load_addr + off, &hdr);
    }

    if (first == 0) {
        warn_report("SEP Boot Monitor: no sepOS at 0x" HWADDR_FMT_plx " to patch", load_addr);
        return;
    }
    apple_sep_boot_monitor_patch_range(as, "sepOS boot", load_addr, first, load_addr, !kernel_macho);
}

static void apple_sep_boot_monitor_jump_work(CPUState* cpu, run_on_cpu_data data)
{
    AppleA13State* acpu      = APPLE_A13(cpu);
    hwaddr         load_addr = data.target_ptr;
    hwaddr         pwr_dn_save;

    assert(bql_locked());

    cpu_synchronize_state(cpu);

    pwr_dn_save = acpu->A13_CPREG_VAR_NAME(SYS_ACC_PWR_DN_SAVE);
    if (hwaccel_enabled()) { apple_sep_boot_monitor_patch_virt(cpu, load_addr); }

    DPRINTF("%s: entering image at 0x" HWADDR_FMT_plx "\n", __func__, load_addr);

    apple_sep_cpu_moni_reset_regs(cpu, load_addr, pwr_dn_save);

#ifdef CONFIG_TCG
    if (tcg_enabled()) {
        arm_rebuild_hflags(&acpu->parent_obj.env);
        tlb_flush(cpu);
    }
#endif

    // using qemu_irq_raise ARM_CPU_IRQ here will cause a7iop atomic sigsegv
}

#ifdef SEP_DISABLE_ASLR
static void disable_aslr_SYS_ACC_PWR_DN_SAVE(AppleSEP* s)
{
    DPRINTF("SEP_BOOT_MONITOR_JUMP: Disable ASLR SYS_ACC_PWR_DN_SAVE\n");
    AppleA13State* acpu        = APPLE_A13(s->cpu);
    hwaddr         pwr_dn_save = acpu->A13_CPREG_VAR_NAME(SYS_ACC_PWR_DN_SAVE);
    AddressSpace*  nsas        = cpu_get_address_space(CPU(acpu), ARMASIdx_NS);
    address_space_set(nsas, pwr_dn_save + 0x80, 0, 0x40, MEMTXATTRS_UNSPECIFIED);
}
#endif

void apple_sep_boot_monitor_jump(AppleSEPBootMonitorState* s)
{
    AppleSEP* sep = s->sep;
    hwaddr    load_addr;

    assert(bql_locked());

    if (!sep->modern) { return; }

    load_addr = apple_sep_boot_monitor_load_addr(s);
    if (load_addr == 0) {
        DPRINTF("%s: no load address programmed\n", __func__);
        return;
    }

    if (!QEMU_IS_ALIGNED(load_addr, 4)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: unaligned load address 0x" HWADDR_FMT_plx "\n", __func__, load_addr);
        return;
    }

#ifdef SEP_DISABLE_ASLR
    disable_aslr_SYS_ACC_PWR_DN_SAVE(sep);
#endif

    async_run_on_cpu(CPU(sep->cpu), apple_sep_boot_monitor_jump_work, RUN_ON_CPU_TARGET_PTR(load_addr));
}

static void apple_sep_boot_monitor_reset_enter(Object* obj, ResetType type)
{
    AppleSEPBootMonitorState* s = APPLE_SEP_BOOT_MONITOR(obj);

    memset(s->boot_monitor_regs, 0, sizeof(s->boot_monitor_regs));
}

static void apple_sep_boot_monitor_realize(DeviceState* dev, Error** errp)
{
    AppleSEPBootMonitorState* s   = APPLE_SEP_BOOT_MONITOR(dev);
    SysBusDevice*             sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->boot_monitor_mr, OBJECT(s), &boot_monitor_reg_ops, s, "regs", BOOT_MONITOR_REG_SIZE);
    sysbus_init_mmio(sbd, &s->boot_monitor_mr);
}

static void apple_sep_boot_monitor_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_boot_monitor_reset_enter;
    dc->realize      = apple_sep_boot_monitor_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPBootMonitorState, apple_sep_boot_monitor, APPLE_SEP_BOOT_MONITOR,
                                     SYS_BUS_DEVICE)

AppleSEPBootMonitorState* apple_sep_boot_monitor_create(AppleSEP* sep)
{
    AppleSEPBootMonitorState* s = APPLE_SEP_BOOT_MONITOR(qdev_new(TYPE_APPLE_SEP_BOOT_MONITOR));
    s->sep                      = sep;
    return s;
}
