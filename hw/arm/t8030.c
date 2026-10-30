/*
 * Apple T8030 SoC (iPhone 11).
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
#include "hw/arm/a13.h"
#include "fs/apfs.h"
#include "hw/arm/boot.h"
#include "hw/arm/dart.h"
#include "hw/arm/dt.h"
#include "hw/arm/kernel_patches.h"
#include "hw/arm/lm-backlight.h"
#include "hw/arm/mem.h"
#include "hw/input/mt-spi.h"
#include "hw/arm/sart.h"
#include "hw/arm/sep/sim.h"
#include "hw/arm/sep/emu.h"
#include "hw/arm/t8030-config.c.inc"
#include "hw/arm/t8030.h"
#include "hw/audio/aop-audio.h"
#include "hw/audio/cs35l27.h"
#include "hw/audio/cs42l77.h"
#include "hw/audio/mca.h"
#include "hw/block/ans.h"
#include "hw/char/apple_uart.h"
#include "hw/display/apple_displaypipe_v4.h"
#include "hw/display/apple_scaler.h"
#include "hw/display/synopsys_mipi_dsim.h"
#include "hw/dma/apple_sio.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/intc/apple_aic.h"
#include "hw/misc/a7iop/core.h"
#include "hw/misc/a7iop/rtkit.h"
#include "hw/misc/aes.h"
#include "hw/misc/aop.h"
#include "hw/misc/baseband.h"
#include "hw/input/buttons.h"
#include "hw/misc/chestnut.h"
#include "hw/misc/fan53740.h"
#include "hw/sensor/mic_tempsensor.h"
#include "hw/misc/roswell.h"
#include "hw/misc/smc.h"
#include "hw/misc/spmi-baseband.h"
#include "hw/misc/spmi-pmu.h"
#include "hw/misc/unimp.h"
#include "hw/nvram/apple_nvram.h"
#include "hw/pci-host/apcie.h"
#include "hw/spmi/apple_spmi.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "hw/usb/apple_typec.h"
#include "hw/usb/usb-uplink.h"
#include "hw/watchdog/apple_wdt.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/block-backend.h"
#include "system/hw_accel.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "system/tcg.h"
#include "arm-powerctl.h"
#include "target/arm/internals.h"
#include "target/arm/apple-virt.h"
#include <pixman.h>
#ifdef CONFIG_PNG
    #include <png.h>
#else
    #error "PNG support is required"
#endif

#define PROP_VISIT_GETTER_SETTER(_type, _name)                                                           \
    static void t8030_get_##_name(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) \
    {                                                                                                    \
        _type##_t value;                                                                                 \
                                                                                                         \
        value = APPLE_T8030(obj)->_name;                                                                 \
        visit_type_##_type(v, name, &value, errp);                                                       \
    }                                                                                                    \
                                                                                                         \
    static void t8030_set_##_name(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) \
    { visit_type_##_type(v, name, &APPLE_T8030(obj)->_name, errp); }

#define PROP_STR_GETTER_SETTER(_name)                                                                       \
    static char* t8030_get_##_name(Object* obj, Error** errp) { return g_strdup(APPLE_T8030(obj)->_name); } \
                                                                                                            \
    static void t8030_set_##_name(Object* obj, const char* value, Error** errp)                             \
    {                                                                                                       \
        AppleT8030MachineState* t8030;                                                                      \
                                                                                                            \
        t8030 = APPLE_T8030(obj);                                                                           \
        g_free(t8030->_name);                                                                               \
        t8030->_name = g_strdup(value);                                                                     \
    }

#define PROP_GETTER_SETTER(_type, _name)                                                                       \
    static void t8030_set_##_name(Object* obj, _type value, Error** errp) { APPLE_T8030(obj)->_name = value; } \
                                                                                                               \
    static _type t8030_get_##_name(Object* obj, Error** errp) { return APPLE_T8030(obj)->_name; }

#define SROM_BASE (0x100000000)
#define SROM_SIZE (512 * KiB)

#define SRAM_BASE (0x19C000000)
#define SRAM_SIZE (0x400000)

#define DRAM_BASE (0x800000000)

#define SEPROM_BASE (0x240000000)
#define SEPROM_SIZE (8 * MiB)

#define PREBOOT_APTICKET    "System/Library/Caches/apticket.der"
#define PREBOOT_KERNELCACHE "System/Library/Caches/com.apple.kernelcaches/kernelcache"
#define PREBOOT_DEVICETREE  "usr/standalone/firmware/devicetree.img4"
#define PREBOOT_TRUSTCACHE  "usr/standalone/firmware/FUD/StaticTrustCache.img4"
#define PREBOOT_ROOT_HASH   "usr/standalone/firmware/root_hash.img4"
#define PREBOOT_SPTM        "usr/standalone/firmware/FUD/Ap,SecurePageTableMonitor.img4"
#define PREBOOT_TXM         "usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4"

#define GPIO_FORCE_DFU (161)

#define DISPLAY_SIZE           (68 * MiB)
#define DISPLAY_DART_PAGE_SIZE (16 * KiB)
#define DISPLAY_FB_IOVA        (0x10000000)

#define PMAP_IO_RANGE_SIZE         (24)
#define PMAP_IO_RANGE_CARVEOUT_SIG (0x43415256)    // 'CARV'

#define DWC2_IRQ (495)

#define NUM_UARTS (9)

#define ANS_SIZE   (0x3D24000)
#define SIO_SIZE   (0x114000)
#define PANIC_SIZE (0x100000)

#define T8030_MCA_MCLK_CFG_BASE     (0x23B0400B4)
#define T8030_MCA_MCLK_CFG_CLUSTERS (6)
#define T8030_MCA_MCLK_CFG_STRIDE   (4)
#define T8030_MCA_MCLK_CFG_SIZE     (T8030_MCA_MCLK_CFG_CLUSTERS * T8030_MCA_MCLK_CFG_STRIDE)

#define AMCC_BASE                               (0x200000000)
#define AMCC_PLANE_COUNT                        (4)
#define AMCC_PLANE_STRIDE                       (0x40000)
#define AMCC_SIZE                               (AMCC_PLANE_COUNT * AMCC_PLANE_STRIDE + 0x80000)
#define AMCC_PLANE_BLK_MCC_CHANNEL_DEC(_p)      (0x4 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_MCC_CHANNEL_DEC_STACKED  BIT(0)
#define AMCC_PLANE_BLK_HASH0_LO(_p)             (0x8 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_HASH0_HI(_p)             (0xC + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_HASH1_LO(_p)             (0x10 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_HASH1_HI(_p)             (0x14 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_NUM_MCU_CHANNEL(_p)      (0x20 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_LOWER_LIMIT(_p)              (0x680 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_UPPER_LIMIT(_p)              (0x684 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_ENABLED(_p)                  (0x688 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_LOCK(_p)                     (0x68C + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_TZ0_BASE(_p)                 (0x6A0 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_TZ0_END(_p)                  (0x6A4 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_TZ0_LOCK(_p)                 (0x6A8 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH0(_p)  (0x1004 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH1(_p)  (0x1008 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH2(_p)  (0x100C + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_BLK_ADDR_MAP_MODE(_p)    (0x1010 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_BLK_ADDR_CFG(_p)         (0x1014 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_BLK_NUM_MCU_CHANNEL(_p)  (0x1020 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_CACHE_CONTROL(_p)            (0x1C00 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_CACHE_STATUS(_p)             (0x1C04 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_NON_PLANE                          (AMCC_PLANE_COUNT * AMCC_PLANE_STRIDE)
#define AMCC_NON_PLANE_GFX                      ((AMCC_PLANE_COUNT + 1) * AMCC_PLANE_STRIDE)
#define AMCC_NON_PLANE_BROADCAST                (AMCC_NON_PLANE + 0xC)
#define AMCC_NON_PLANE_TAG_RAM_BANK_HASH0       (AMCC_NON_PLANE + 0x80)
#define AMCC_NON_PLANE_TAG_RAM_BANK_HASH1       (AMCC_NON_PLANE + 0x84)
#define AMCC_NON_PLANE_ADDR_HASH0_LO            (AMCC_NON_PLANE + 0x180)
#define AMCC_NON_PLANE_ADDR_HASH0_HI            (AMCC_NON_PLANE + 0x184)
#define AMCC_NON_PLANE_ADDR_HASH1_LO            (AMCC_NON_PLANE + 0x188)
#define AMCC_NON_PLANE_ADDR_HASH1_HI            (AMCC_NON_PLANE + 0x18C)
#define AMCC_NON_PLANE_ADDR_NUM_MCU_CHANNEL     (AMCC_NON_PLANE + 0x198)
#define AMCC_NON_PLANE_GFX_TAG_RAM_BANK_HASH0   (AMCC_NON_PLANE_GFX + 0x80)
#define AMCC_NON_PLANE_GFX_TAG_RAM_BANK_HASH1   (AMCC_NON_PLANE_GFX + 0x84)
#define AMCC_NON_PLANE_GFX_ADDR_HASH0_LO        (AMCC_NON_PLANE_GFX + 0x100)
#define AMCC_NON_PLANE_GFX_ADDR_HASH0_HI        (AMCC_NON_PLANE_GFX + 0x104)
#define AMCC_NON_PLANE_GFX_ADDR_HASH1_LO        (AMCC_NON_PLANE_GFX + 0x108)
#define AMCC_NON_PLANE_GFX_ADDR_HASH1_HI        (AMCC_NON_PLANE_GFX + 0x10C)
#define AMCC_NON_PLANE_GFX_ADDR_NUM_MCU_CHANNEL (AMCC_NON_PLANE_GFX + 0x118)
#define AMCC_RREG32(_machine, _off)             ldl_le_p(&_machine->amcc_reg[_off])
#define AMCC_WREG32(_machine, _off, _val)       stl_le_p(&_machine->amcc_reg[_off], _val)

#define FUSE_ENABLED  (0xA55AC33C)
#define FUSE_DISABLED (0xA050C030)

static uint32_t t8030_real_cpu_count(AppleT8030MachineState* t8030)
{ return MACHINE(t8030)->smp.cpus - (t8030->sep_rom_filename != NULL); }

static qemu_irq t8030_irq(AppleT8030MachineState* t8030, uint32_t irq)
{ return qdev_get_gpio_in(DEVICE(t8030->aic), irq); }

// iBoot hands off with FP enabled; SPTM uses NEON before enabling it itself.
static void t8030_iboot_handoff_state(CPUState* cpu, run_on_cpu_data data)
{
    CPUARMState* env = cpu_env(cpu);

    cpu_synchronize_state(cpu);
    env->cp15.cpacr_el1 = REG_FIELD_DP64(env->cp15.cpacr_el1, CPACR_EL1, FPEN, 3);
    if (tcg_enabled()) { arm_rebuild_hflags(env); }
}

static void t8030_start_cpus(AppleT8030MachineState* t8030, uint64_t cpu_mask)
{
    uint32_t i;

    for (i = 0; i < t8030_real_cpu_count(t8030); ++i) {
        if ((cpu_mask & BIT_ULL(i)) != 0) {
            apple_a13_set_on(t8030->cpus[i]);
            async_run_on_cpu(CPU(t8030->cpus[i]), t8030_iboot_handoff_state, RUN_ON_CPU_NULL);
        }
    }
}

static void t8030_patch_kernel(MachoHeader64* header, uint32_t build_version)
{
    ck_patch_kernel(header);

    if (hwaccel_enabled()) { ck_patch_virt(header); }
}

static void t8030_set_kernel(AppleT8030MachineState* t8030, MachoHeader64* kernel)
{
    vaddr    kc_end;
    uint32_t build_version = apple_boot_build_version(kernel);

    if (t8030->kernel != NULL) { g_free(apple_boot_get_macho_buffer(t8030->kernel)); }
    t8030->kernel = kernel;

    info_report("%s %u.%u.%u", apple_boot_platform_string(kernel), BUILD_VERSION_MAJOR(build_version),
                BUILD_VERSION_MINOR(build_version), BUILD_VERSION_PATCH(build_version));
    t8030->build_version = build_version;

    switch (BUILD_VERSION_MAJOR(build_version)) {
        case 13: t8030->sio_protocol = 8; break;
        case 14:
        case 15:
        case 16: t8030->sio_protocol = 9; break;
        case 17:
        case 18:
        case 26:
        case 27: t8030->sio_protocol = 10; break;
        default: assert_not_reached();
    }

    apple_rtkit_max_version = BUILD_VERSION_MAJOR(build_version) <= 13 ? 10 : 12;

    if (t8030->securerom_filename == NULL) {
        apple_boot_get_kc_bounds(kernel, NULL, &g_virt_base, &kc_end, NULL, NULL);
        info_report("Kernel virtual low: 0x%016" VADDR_PRIx, g_virt_base);
        info_report("Kernel virtual high: 0x%016" VADDR_PRIx, kc_end);

        g_phys_base = (hwaddr)apple_boot_get_macho_buffer(kernel);

        t8030_patch_kernel(kernel, build_version);
    }
}

static bool t8030_check_panic(AppleT8030MachineState* t8030)
{
    AppleEmbeddedPanicHeader* panic_info;
    bool                      ret;

    if (t8030->panic_size == 0) { return false; }

    panic_info = g_malloc0(t8030->panic_size);

    address_space_rw(&address_space_memory, t8030->panic_base, MEMTXATTRS_UNSPECIFIED, panic_info, t8030->panic_size,
                     false);
    address_space_set(&address_space_memory, t8030->panic_base, 0, t8030->panic_size, MEMTXATTRS_UNSPECIFIED);

    ret = panic_info->magic == EMBEDDED_PANIC_MAGIC;
    g_free(panic_info);
    return ret;
}

static uint64_t get_kaslr_random(void)
{
    uint64_t value = 0;
    qemu_guest_getrandom(&value, sizeof(value), NULL);
    return value;
}

#define L2_GRANULE      ((0x4000) * (0x4000 / 8))
#define L2_GRANULE_MASK (L2_GRANULE - 1)

static void get_kaslr_slides(AppleT8030MachineState* t8030, hwaddr* phys_slide_out, vaddr* virt_slide_out)
{
    static const size_t slide_granular      = (1 << 21);
    static const size_t slide_granular_mask = slide_granular - 1;
    static const size_t slide_virt_max      = 0x100 * (2 * 1024 * 1024);

    hwaddr slide_phys;
    vaddr  slide_virt;
    size_t random_value = get_kaslr_random();

    if (t8030->kaslr_off) {
        *phys_slide_out = 0;
        *virt_slide_out = 0;
        return;
    }

    slide_virt = (random_value & ~slide_granular_mask) % slide_virt_max;
    if (slide_virt == 0) { slide_virt = slide_virt_max; }
    slide_phys = slide_virt & L2_GRANULE_MASK;

    *phys_slide_out = slide_phys;
    *virt_slide_out = slide_virt;
}

// 256 MiB doesn't seem to work properly with sepos 18.5
// SEP's MPIDR influences stack/sp address
// #define T8030_TZ0_SIZE (300 * MiB)
#define T8030_TZ0_SIZE (240 * MiB)    // FIXME: workaround for sepos >= 16

// for booting sepfw 26 without opcode17:
// tz0_addr must be, at least, aligned to 32 MiB
#define T8030_TZ0_ALIGN (32 * MiB)

// SPTM locks the read-only region itself.
static void t8030_lock_regions(AppleT8030MachineState* t8030, hwaddr ro_lower, hwaddr ro_upper, bool lock_ro)
{
    AppleBootInfo* info = &t8030->boot_info;

    for (int i = 0; i < AMCC_PLANE_COUNT; ++i) {
        AMCC_WREG32(t8030, AMCC_PLANE_LOWER_LIMIT(i), (ro_lower - info->dram_base) >> 14);
        AMCC_WREG32(t8030, AMCC_PLANE_UPPER_LIMIT(i), (ro_upper - info->dram_base - 1) >> 14);
        AMCC_WREG32(t8030, AMCC_PLANE_LOCK(i), lock_ro);
        AMCC_WREG32(t8030, AMCC_PLANE_TZ0_BASE(i), (info->tz0_addr - info->dram_base) >> 12);
        AMCC_WREG32(t8030, AMCC_PLANE_TZ0_END(i), ((info->tz0_addr + info->tz0_size - 1) - info->dram_base) >> 12);
        AMCC_WREG32(t8030, AMCC_PLANE_TZ0_LOCK(i), 1);
        AMCC_WREG32(t8030, AMCC_PLANE_BLK_MCC_CHANNEL_DEC(i),
                    ((info->dram_size >> 27) - 1) << 1
                        | (info->dram_size < 4 * GiB ? AMCC_PLANE_BLK_MCC_CHANNEL_DEC_STACKED : 0));
        AMCC_WREG32(t8030, AMCC_PLANE_MUI_BLK_ADDR_CFG(i), 0x40201);
        AMCC_WREG32(t8030, AMCC_PLANE_BLK_HASH0_LO(i), 0xad4b4c00);
        AMCC_WREG32(t8030, AMCC_PLANE_BLK_HASH0_HI(i), 0x4);
        AMCC_WREG32(t8030, AMCC_PLANE_BLK_HASH1_LO(i), 0x52b4b200);
        AMCC_WREG32(t8030, AMCC_PLANE_BLK_HASH1_HI(i), 0x3);
        AMCC_WREG32(t8030, AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH0(i), 0x4992);
        AMCC_WREG32(t8030, AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH1(i), 0x64b4);
        AMCC_WREG32(t8030, AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH2(i), 0xad26);
        AMCC_WREG32(t8030, AMCC_PLANE_MUI_BLK_ADDR_MAP_MODE(i), 0x301);
        AMCC_WREG32(t8030, AMCC_PLANE_BLK_NUM_MCU_CHANNEL(i), 0x0);
        // AMCC_WREG32(t8030, AMCC_PLANE_CACHE_STATUS(i), 0x1);
    }
    // only here for good measure
    AMCC_WREG32(t8030, AMCC_NON_PLANE_TAG_RAM_BANK_HASH0, 0x22a);
    AMCC_WREG32(t8030, AMCC_NON_PLANE_TAG_RAM_BANK_HASH1, 0x45);
    AMCC_WREG32(t8030, AMCC_NON_PLANE_ADDR_NUM_MCU_CHANNEL, 0x0);

    AMCC_WREG32(t8030, AMCC_NON_PLANE_ADDR_HASH0_LO, AMCC_RREG32(t8030, AMCC_PLANE_BLK_HASH0_LO(0)));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_ADDR_HASH0_HI, AMCC_RREG32(t8030, AMCC_PLANE_BLK_HASH0_HI(0)));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_ADDR_HASH1_LO, AMCC_RREG32(t8030, AMCC_PLANE_BLK_HASH1_LO(0)));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_ADDR_HASH1_HI, AMCC_RREG32(t8030, AMCC_PLANE_BLK_HASH1_HI(0)));

    AMCC_WREG32(t8030, AMCC_NON_PLANE_GFX_TAG_RAM_BANK_HASH0, AMCC_RREG32(t8030, AMCC_NON_PLANE_TAG_RAM_BANK_HASH0));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_GFX_TAG_RAM_BANK_HASH1, AMCC_RREG32(t8030, AMCC_NON_PLANE_TAG_RAM_BANK_HASH1));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_GFX_ADDR_HASH0_LO, AMCC_RREG32(t8030, AMCC_PLANE_BLK_HASH0_LO(0)));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_GFX_ADDR_HASH0_HI, AMCC_RREG32(t8030, AMCC_PLANE_BLK_HASH0_HI(0)));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_GFX_ADDR_HASH1_LO, AMCC_RREG32(t8030, AMCC_PLANE_BLK_HASH1_LO(0)));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_GFX_ADDR_HASH1_HI, AMCC_RREG32(t8030, AMCC_PLANE_BLK_HASH1_HI(0)));
    AMCC_WREG32(t8030, AMCC_NON_PLANE_GFX_ADDR_NUM_MCU_CHANNEL,
                AMCC_RREG32(t8030, AMCC_NON_PLANE_ADDR_NUM_MCU_CHANNEL));

    if (t8030->sep_rom_filename != NULL) {
        apple_sep_setup_tz0(APPLE_SEP(object_property_get_link(OBJECT(t8030), "sep", &error_fatal)),
                            MACHINE(t8030)->ram, info->tz0_addr - info->dram_base, info->tz0_size);
    }
}

static void t8030_load_kernelcache(AppleT8030MachineState* t8030, const char* cmdline, CarveoutAllocator* ca)
{
    MachineState*  machine = MACHINE(t8030);
    vaddr          kc_base;
    vaddr          kc_end;
    vaddr          ro_lower;
    vaddr          ro_upper;
    vaddr          apple_dt_va;
    hwaddr         mem_size;
    hwaddr         phys_ptr;
    AppleBootInfo* info = &t8030->boot_info;
    uint8_t*       data;
    gsize          len;

    apple_boot_get_kc_bounds(t8030->kernel, NULL, &kc_base, &kc_end, &ro_lower, &ro_upper);

    get_kaslr_slides(t8030, &g_phys_slide, &g_virt_slide);

    g_phys_base = DRAM_BASE;
    g_virt_base = kc_base + (g_virt_slide - g_phys_slide);

    info->trustcache_addr = vtop_slid(ro_lower) - info->trustcache_size;
    info_report("TrustCache Physical Base: 0x" HWADDR_FMT_plx, info->trustcache_addr);

    address_space_rw(&address_space_memory, info->trustcache_addr, MEMTXATTRS_UNSPECIFIED, t8030->trustcache,
                     info->trustcache_size, true);

    info->kern_entry = apple_boot_load_macho(t8030->kernel, &address_space_memory,
                                             apple_dt_get_node(t8030->device_tree, "/chosen/memory-map"),
                                             g_phys_base + g_phys_slide, g_virt_slide);

    info_report("Kernel Virtual Base: 0x%016" VADDR_PRIx, g_virt_base);
    info_report("Kernel Physical Base: 0x" HWADDR_FMT_plx, g_phys_base);
    info_report("Kernel Virtual Slide: 0x%016" VADDR_PRIx, g_virt_slide);
    info_report("Kernel Physical Slide: 0x" HWADDR_FMT_plx, g_phys_slide);
    info_report("Kernel Entry Point: 0x%016" VADDR_PRIx, info->kern_entry);

    phys_ptr = vtop_static(ROUND_UP_16K(kc_end + g_virt_slide));

    // RAM Disk
    if (apple_dfu_copy_image(t8030->dfu, "rdsk", &data, &len)) {
        info->ramdisk_addr = phys_ptr;
        apple_boot_load_ramdisk_data("DFU ramdisk", data, len, &address_space_memory, info->ramdisk_addr,
                                     &info->ramdisk_size);
        info->ramdisk_size  = ROUND_UP_16K(info->ramdisk_size);
        phys_ptr           += info->ramdisk_size;
    }

    // TZ0
    info->tz0_size = T8030_TZ0_SIZE;
    phys_ptr       = ROUND_UP(phys_ptr, T8030_TZ0_ALIGN);

    info->tz0_addr  = phys_ptr;
    phys_ptr       += info->tz0_size;
    info_report("TZ0 Physical Base: 0x" HWADDR_FMT_plx, info->tz0_addr);

    // SEP Firmware
    info->sep_fw_addr = phys_ptr;
    // should be equal to make the mapping work
    info->sep_fw_size = SEPFW_MAPPING_SIZE;
    info_report("SEPFW Physical Base: 0x" HWADDR_FMT_plx, info->sep_fw_addr);
    info_report("SEPFW Size: 0x" HWADDR_FMT_plx, info->sep_fw_size);
    phys_ptr += info->sep_fw_size;

    // Kernel boot args
    info->kern_boot_args_addr  = phys_ptr;
    info->kern_boot_args_size  = 0x4000;
    phys_ptr                  += info->kern_boot_args_size;

    // Device tree
    info->device_tree_addr  = phys_ptr;
    apple_dt_va             = ptov_static(info->device_tree_addr);
    phys_ptr               += info->device_tree_size;
    info_report("Device Tree Physical Base: 0x" HWADDR_FMT_plx, info->device_tree_addr);
    info_report("Device Tree Virtual Base: 0x%016" VADDR_PRIx, apple_dt_va);
    info_report("Device Tree Size: 0x" HWADDR_FMT_plx, info->device_tree_size);

    info->top_of_kernel_data_pa = ROUND_UP_16K(phys_ptr);
    info_report("Top Of Kernel Data: 0x" HWADDR_FMT_plx, info->top_of_kernel_data_pa);

    apple_boot_finalise_dt(t8030->device_tree, &address_space_memory, info);

    mem_size = carveout_alloc_finalise(ca);
    info_report("Memory Size: 0x" HWADDR_FMT_plx, mem_size);

    apple_boot_setup_bootargs(t8030->build_version, &address_space_memory, info->kern_boot_args_addr, g_virt_base,
                              g_phys_base, mem_size, info->top_of_kernel_data_pa, apple_dt_va, info->device_tree_size,
                              &t8030->video_args, cmdline, machine->ram_size);
    g_virt_base = kc_base;

    t8030_lock_regions(t8030, info->trustcache_addr, vtop_slid(ro_upper), true);
}

// The collection's own segments; `apple_boot_get_segment` looks into `com.apple.kernel` instead.
static MachoSegmentCommand64* t8030_sptm_get_segment(MachoHeader64* header, const char* name)
{
    MachoLoadCommand* cmd = (MachoLoadCommand*)(header + 1);

    for (uint32_t i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        MachoSegmentCommand64* seg = (MachoSegmentCommand64*)cmd;

        if (cmd->cmd == LC_SEGMENT_64 && strncmp(seg->segname, name, sizeof(seg->segname)) == 0) { return seg; }
    }

    return NULL;
}

static hwaddr t8030_sptm_segment_size(MachoHeader64* header, const char* name)
{
    MachoSegmentCommand64* seg = t8030_sptm_get_segment(header, name);
    return seg == NULL ? 0 : ROUND_UP_16K(seg->vmsize);
}

static hwaddr t8030_sptm_load_segment(MachoHeader64* header, const char* name, hwaddr pa)
{
    MachoSegmentCommand64* seg = t8030_sptm_get_segment(header, name);
    vaddr                  base;

    if (seg == NULL || seg->vmsize == 0) { return pa; }

    apple_boot_get_kc_bounds(header, NULL, &base, NULL, NULL, NULL);
    address_space_rw(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                     apple_boot_get_macho_buffer(header) + seg->vmaddr - base, seg->vmsize, true);
    return pa + ROUND_UP_16K(seg->vmsize);
}

static void t8030_sptm_set_region(AppleDTNode* memory_map, const char* name, uint64_t addr, uint64_t size)
{
    AppleDTProp* prop = apple_dt_get_prop(memory_map, name);

    assert_nonnull(prop);
    stq_le_p(prop->data, addr);
    stq_le_p(prop->data + sizeof(uint64_t), size);
}

// iBoot numbers the DARTs SPTM manages.
static void t8030_sptm_assign_dart_ids(AppleDTNode* root)
{
    AppleDTNode* arm_io = apple_dt_get_node(root, "arm-io");
    uint32_t     id     = 0;

    assert_nonnull(arm_io);
    for (GList* l = arm_io->children; l != NULL; l = l->next) {
        AppleDTNode* node   = l->data;
        AppleDTProp* compat = apple_dt_get_prop(node, "compatible");

        if (compat != NULL && strcmp(compat->data, "dart,t8020") == 0) { apple_dt_set_prop_u32(node, "dart-id", id++); }
    }
}

// SPTM checks that the regions from `TXM-ro` to `SPTM-rw` are contiguous and in this order.
static void t8030_load_sptm(AppleT8030MachineState* t8030, const char* cmdline, CarveoutAllocator* ca)
{
    MachineState*  machine    = MACHINE(t8030);
    AppleBootInfo* info       = &t8030->boot_info;
    AppleDTNode*   memory_map = apple_dt_get_node(t8030->device_tree, "/chosen/memory-map");
    MachoHeader64* kc         = t8030->kernel;
    MachoHeader64* txm        = t8030->txm;
    MachoHeader64* sptm       = t8030->sptm;
    vaddr          kc_text;
    vaddr          txm_text;
    vaddr          sptm_text;
    hwaddr         region;
    hwaddr         phys_ptr;
    hwaddr         ro_lower;
    hwaddr         ro_upper;
    hwaddr         sptm_base;
    hwaddr         before_sptm;
    hwaddr         mem_size;
    vaddr          apple_dt_va;
    uint8_t*       data;
    gsize          len;

    apple_boot_get_kc_bounds(kc, &kc_text, NULL, NULL, NULL, NULL);
    apple_boot_get_kc_bounds(txm, &txm_text, NULL, NULL, NULL, NULL);
    apple_boot_get_kc_bounds(sptm, &sptm_text, NULL, NULL, NULL, NULL);

    // SPTM maps itself at `virtBase + (SPTM-ro - physBase)`, and TXM and the kernel collection 1 and 2 strides above.
    // Placing SPTM-ro at the kernel collection's offset in its 64 GiB block keeps that slide page-table aligned.
    // TXM writes a spare page in the `DeviceTree` region.
    info->device_tree_size += 16 * KiB;

    before_sptm = t8030_sptm_segment_size(txm, "__TEXT") + t8030_sptm_segment_size(txm, "__DATA_CONST")
                  + t8030_sptm_segment_size(txm, "__TEXT_EXEC") + t8030_sptm_segment_size(txm, "__TEXT_BOOT_EXEC")
                  + info->trustcache_size + t8030_sptm_segment_size(kc, "__TEXT_EXEC")
                  + t8030_sptm_segment_size(kc, "__TEXT_BOOT_EXEC") + t8030_sptm_segment_size(kc, "__TEXT")
                  + t8030_sptm_segment_size(kc, "__PRELINK_TEXT") + t8030_sptm_segment_size(kc, "__DATA_CONST")
                  + t8030_sptm_segment_size(kc, "__DATA_SPTM") + info->device_tree_size;
    assert_cmphex(extract64(kc_text, 0, 36), >, before_sptm);

    g_phys_base = DRAM_BASE;
    g_virt_base = kc_text & ~MAKE_64BIT_MASK(0, 36);
    phys_ptr    = g_phys_base + extract64(kc_text, 0, 36) - before_sptm;
    ro_lower    = phys_ptr;

    // The memory skipped for that is the physical slide, which XNU frees.
    t8030_sptm_set_region(memory_map, "slide", g_phys_base, phys_ptr - g_phys_base);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(txm, "__TEXT", phys_ptr);
    phys_ptr = t8030_sptm_load_segment(txm, "__DATA_CONST", phys_ptr);
    t8030_sptm_set_region(memory_map, "TXM-ro", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(txm, "__TEXT_EXEC", phys_ptr);
    t8030_sptm_set_region(memory_map, "TXM-rx", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(txm, "__TEXT_BOOT_EXEC", phys_ptr);
    t8030_sptm_set_region(memory_map, "TXM-bx", region, phys_ptr - region);

    info->trustcache_addr = phys_ptr;
    address_space_rw(&address_space_memory, info->trustcache_addr, MEMTXATTRS_UNSPECIFIED, t8030->trustcache,
                     info->trustcache_size, true);
    phys_ptr += info->trustcache_size;

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(kc, "__TEXT_EXEC", phys_ptr);
    t8030_sptm_set_region(memory_map, "BootKC-rx", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(kc, "__TEXT_BOOT_EXEC", phys_ptr);
    t8030_sptm_set_region(memory_map, "BootKC-bx", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(kc, "__TEXT", phys_ptr);
    phys_ptr = t8030_sptm_load_segment(kc, "__PRELINK_TEXT", phys_ptr);
    phys_ptr = t8030_sptm_load_segment(kc, "__DATA_CONST", phys_ptr);
    t8030_sptm_set_region(memory_map, "BootKC-ro", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(kc, "__DATA_SPTM", phys_ptr);
    t8030_sptm_set_region(memory_map, "BootKC-rs", region, phys_ptr - region);

    info->device_tree_addr  = phys_ptr;
    phys_ptr               += info->device_tree_size;

    sptm_base = phys_ptr;
    phys_ptr  = t8030_sptm_load_segment(sptm, "__TEXT", phys_ptr);
    phys_ptr  = t8030_sptm_load_segment(sptm, "__DATA_CONST", phys_ptr);
    phys_ptr  = t8030_sptm_load_segment(sptm, "__LATE_CONST", phys_ptr);
    t8030_sptm_set_region(memory_map, "SPTM-ro", sptm_base, phys_ptr - sptm_base);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(sptm, "__TEXT_EXEC", phys_ptr);
    phys_ptr = t8030_sptm_load_segment(sptm, "__LAST", phys_ptr);
    t8030_sptm_set_region(memory_map, "SPTM-rx", region, phys_ptr - region);
    ro_upper = phys_ptr;

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(sptm, "__DATA", phys_ptr);
    phys_ptr = t8030_sptm_load_segment(sptm, "__BOOTDATA", phys_ptr);
    t8030_sptm_set_region(memory_map, "SPTM-rw", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(sptm, "__LINKEDIT", phys_ptr);
    t8030_sptm_set_region(memory_map, "SPTM-le", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(txm, "__DATA", phys_ptr);
    t8030_sptm_set_region(memory_map, "TXM-rw", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(txm, "__LINKEDIT", phys_ptr);
    t8030_sptm_set_region(memory_map, "TXM-le", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(kc, "__PRELINK_INFO", phys_ptr);
    phys_ptr = t8030_sptm_load_segment(kc, "__DATA", phys_ptr);
    t8030_sptm_set_region(memory_map, "BootKC-rw", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = t8030_sptm_load_segment(kc, "__LINKEDIT", phys_ptr);
    t8030_sptm_set_region(memory_map, "BootKC-le", region, phys_ptr - region);

    // No exclaves on A13.
    t8030_sptm_set_region(memory_map, "CL4-rx", -1, 0);
    t8030_sptm_set_region(memory_map, "CL4-ro", -1, 0);
    t8030_sptm_set_region(memory_map, "CL4-dummypage", phys_ptr, 16 * KiB);
    phys_ptr += 16 * KiB;

    t8030_sptm_set_region(memory_map, "TXM-virt", txm_text, 0);
    t8030_sptm_set_region(memory_map, "TXM-entry", apple_boot_get_entry_point(txm), 0);
    t8030_sptm_set_region(memory_map, "BootKC-virt", kc_text, 0);
    t8030_sptm_set_region(memory_map, "BootKC-entry", apple_boot_get_entry_point(kc), 0);
    t8030_sptm_set_region(memory_map, "SPTM-virt", sptm_text, 0);
    t8030_sptm_set_region(memory_map, "SPTM-entry", apple_boot_get_entry_point(sptm), 0);

    info->kern_entry = sptm_base + apple_boot_get_entry_point(sptm) - sptm_text;
    info_report("SPTM Physical Base: 0x" HWADDR_FMT_plx, sptm_base);
    info_report("SPTM Entry Point: 0x" HWADDR_FMT_plx, info->kern_entry);

    info->kern_boot_args_addr  = phys_ptr;
    info->kern_boot_args_size  = 0x4000;
    phys_ptr                  += info->kern_boot_args_size;

    if (apple_dfu_copy_image(t8030->dfu, "rdsk", &data, &len)) {
        info->ramdisk_addr = phys_ptr;
        apple_boot_load_ramdisk_data("DFU ramdisk", data, len, &address_space_memory, info->ramdisk_addr,
                                     &info->ramdisk_size);
        info->ramdisk_size  = ROUND_UP_16K(info->ramdisk_size);
        phys_ptr           += info->ramdisk_size;
    }

    info->sep_fw_addr  = phys_ptr;
    info->sep_fw_size  = SEPFW_MAPPING_SIZE;
    phys_ptr          += info->sep_fw_size;
    info_report("SEPFW Physical Base: 0x" HWADDR_FMT_plx, info->sep_fw_addr);

    info->top_of_kernel_data_pa = ROUND_UP_16K(phys_ptr);
    info_report("Top Of Kernel Data: 0x" HWADDR_FMT_plx, info->top_of_kernel_data_pa);

    apple_dt_va = ptov_static(info->device_tree_addr);
    apple_boot_finalise_dt(t8030->device_tree, &address_space_memory, info);

    mem_size = carveout_alloc_finalise(ca);
    info_report("Memory Size: 0x" HWADDR_FMT_plx, mem_size);

    apple_boot_setup_bootargs(t8030->build_version, &address_space_memory, info->kern_boot_args_addr, g_virt_base,
                              g_phys_base, mem_size, info->top_of_kernel_data_pa, apple_dt_va, info->device_tree_size,
                              &t8030->video_args, cmdline, machine->ram_size);

    t8030_lock_regions(t8030, ro_lower, ro_upper, false);
}

static void t8030_rtkit_seg_prop_setup(AppleDTNode* iop_nub, hwaddr base, uint32_t size)
{
    apple_dt_set_prop_u64(iop_nub, "region-base", base);
    apple_dt_set_prop_u64(iop_nub, "region-size", size);
}

static void t8030_rtkit_mem_setup(AppleT8030MachineState* t8030, CarveoutAllocator* ca, const char* name,
                                  const char* nub_name, uint32_t size)
{
    AppleDTNode* child;
    AppleDTNode* iop_nub;
    hwaddr       base = carveout_alloc_mem(ca, size);

    if (t8030->device_tree == NULL) { return; }

    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(child);
    child = apple_dt_get_node(child, name);
    assert_nonnull(child);
    iop_nub = apple_dt_get_node(child, nub_name);
    assert_nonnull(iop_nub);

    t8030_rtkit_seg_prop_setup(iop_nub, base, size);
}

static const uint8_t t8030_sep_nonce[] = {
    0xFE, 0xED, 0xFA, 0xCE, 0xFE, 0xED, 0xFA, 0xCE, 0xFE, 0xED,
    0xFA, 0xCE, 0xFE, 0xED, 0xFA, 0xCE, 0xFE, 0xED, 0xFA, 0xCE,
};

static bool t8030_load_preboot_file(const char* path, uint8_t** data, gsize* len, Error** errp)
{
    BlockBackend*       blk       = blk_by_name("root");
    g_autofree uint8_t* active    = NULL;
    g_autofree char*    full_path = NULL;
    gsize               active_len;

    if (blk == NULL) {
        error_setg(errp, "No `root` drive to read `%s` from", path);
        return false;
    }

    if (!apfs_read_file(blk, APFS_VOL_ROLE_PREBOOT, "active", &active, &active_len, NULL)) {
        return apfs_read_file(blk, APFS_VOL_ROLE_SYSTEM, path, data, len, errp);
    }

    full_path = g_strdup_printf("%s/%s", (char*)active, path);
    return apfs_read_file(blk, APFS_VOL_ROLE_PREBOOT, full_path, data, len, errp);
}

/* Mark the DART page table regions as PPL-protected, as `_t8020dart_init` requires. */
static void t8030_protect_dart_tables(AppleT8030MachineState* t8030, const uint64_t (*regions)[2], uint32_t count)
{
    AppleDTNode* defaults;
    AppleDTProp* prop;
    uint8_t*     ranges;
    uint32_t     len;
    uint32_t     i;

    defaults = apple_dt_get_node(t8030->device_tree, "defaults");
    assert_nonnull(defaults);
    prop = apple_dt_get_prop(defaults, "pmap-io-ranges");
    assert_nonnull(prop);

    ranges = g_malloc(prop->len + count * PMAP_IO_RANGE_SIZE);
    len    = 0;
    for (i = 0; i + PMAP_IO_RANGE_SIZE <= prop->len; i += PMAP_IO_RANGE_SIZE) {
        if (ldl_le_p(prop->data + i + 20) != PMAP_IO_RANGE_CARVEOUT_SIG) {
            memcpy(ranges + len, prop->data + i, PMAP_IO_RANGE_SIZE);
            len += PMAP_IO_RANGE_SIZE;
        }
    }

    // One range per region, since iOS 15 claims a range for each region and panics if it is already owned.
    for (i = 0; i < count; i++, len += PMAP_IO_RANGE_SIZE) {
        stq_le_p(ranges + len, regions[i][0]);
        stq_le_p(ranges + len + 8, regions[i][1] - regions[i][0]);
        stl_le_p(ranges + len + 16, 0x4007);    // PPL-protected, like the DART register ranges.
        stl_le_p(ranges + len + 20, PMAP_IO_RANGE_CARVEOUT_SIG);
    }

    apple_dt_set_prop(defaults, "pmap-io-ranges", len, ranges);
    g_free(ranges);
}

/*
 * Map the framebuffer in dart-disp0 and leave the display scanning it out, as
 * iBoot does. XNU adopts the tables through `pt-region-N` and `l2-tt-N`
 * instead of clearing the TTBRs, until IOMFB takes over.
 */
static void t8030_boot_display(AppleT8030MachineState* t8030, CarveoutAllocator* ca)
{
    AppleDARTState* dart;
    AppleDTNode*    node;
    uint32_t        fb_sid;
    uint32_t        sids;
    uint32_t        sid;
    hwaddr          fb_size;
    hwaddr          size;
    hwaddr          tables;
    uint64_t        pt_regions[32][2];
    uint32_t        count;
    uint64_t        l2_tt[2];
    char            name[16];

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-disp0", &error_abort));
    node = apple_dt_get_node(t8030->device_tree, "arm-io/dart-disp0/mapper-disp0");
    assert_nonnull(node);
    fb_sid = apple_dt_get_prop_u32(node, "reg", &error_abort);

    node = apple_dt_get_node(t8030->device_tree, "arm-io/dart-disp0");
    assert_nonnull(node);
    // Allow persistent mappings.
    apple_dt_set_prop_u32(node, "dart-options", apple_dt_get_prop_u32(node, "dart-options", &error_abort) | BIT32(6));

    fb_size = ROUND_UP((hwaddr)t8030->video_args.row_bytes * t8030->video_args.height, DISPLAY_DART_PAGE_SIZE);

    // XNU requires persistent tables for every translated SID once any SID has them.
    sids = apple_dt_get_prop_u32(node, "sids", &error_abort) & ~apple_dt_get_prop_u32(node, "bypass", &error_abort);
    assert_cmphex(sids & BIT32(fb_sid), !=, 0);

    // An L1 table for each SID, plus the L2 table of the framebuffer's.
    size   = (ctpop32(sids) + 1) * DISPLAY_DART_PAGE_SIZE;
    tables = carveout_alloc_mem(ca, size);

    count = 0;
    for (sid = 0; sid < 32; sid++) {
        if ((sids & BIT32(sid)) == 0) { continue; }

        size = sid == fb_sid ? 2 * DISPLAY_DART_PAGE_SIZE : DISPLAY_DART_PAGE_SIZE;
        apple_dart_map_boot_range(dart, sid, tables, DISPLAY_FB_IOVA, t8030->video_args.base_addr,
                                  sid == fb_sid ? fb_size : 0);

        pt_regions[count][0] = tables;
        pt_regions[count][1] = tables + size;
        snprintf(name, sizeof(name), "pt-region-%u", sid);
        apple_dt_set_prop(node, name, sizeof(pt_regions[count]), pt_regions[count]);
        count++;
        l2_tt[0] = tables;
        // iOS 15 reads the u32 after the address as the table's level, iOS 14 requires it to be 0.
        l2_tt[1] = BUILD_VERSION_MAJOR(t8030->build_version) >= 15 ? 2 : 0;
        snprintf(name, sizeof(name), "l2-tt-%u", sid);
        apple_dt_set_prop(node, name, sizeof(l2_tt), l2_tt);

        tables += size;
    }

    t8030_protect_dart_tables(t8030, pt_regions, count);

    adp_v4_set_boot_fb(APPLE_DISPLAY_PIPE_V4(object_property_get_link(OBJECT(t8030), "disp0", &error_abort)),
                       DISPLAY_FB_IOVA);
}

static void t8030_update_device_tree(AppleT8030MachineState* t8030)
{
    AppleDTNode* node;
    GList*       iter;
    GList*       next = NULL;
    uint32_t     i;

    node = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(node);

    apple_dt_set_prop_u32(node, "chip-revision", t8030->chip_revision);

    apple_dt_set_prop(node, "clock-frequencies", sizeof(t8030_clock_frequencies), t8030_clock_frequencies);
    apple_dt_set_prop(node, "clock-frequencies-nclk", sizeof(t8030_clock_frequencies_nclk),
                      t8030_clock_frequencies_nclk);

    apple_dt_set_prop_strn(t8030->device_tree, "platform-name", 32, "t8030");
    apple_dt_set_prop_strn(t8030->device_tree, "model-number", 32, t8030->model_number);
    apple_dt_set_prop_strn(t8030->device_tree, "region-info", 32, t8030->region_info);
    apple_dt_set_prop_strn(t8030->device_tree, "config-number", 64, t8030->config_number);
    apple_dt_set_prop_strn(t8030->device_tree, "serial-number", 32, t8030->serial_number);
    apple_dt_set_prop_strn(t8030->device_tree, "mlb-serial-number", 32, t8030->mlb_serial_number);
    apple_dt_set_prop_strn(t8030->device_tree, "regulatory-model-number", 32, t8030->regulatory_model);

    node = apple_dt_get_node(t8030->device_tree, "chosen");
    apple_dt_set_prop_u32(node, "chip-id", 0x8030);
    apple_dt_set_prop_u32(node, "board-id", t8030->board_id);
    apple_dt_set_prop_u32(node, "certificate-production-status", 1);
    apple_dt_set_prop_u32(node, "certificate-security-mode", 1);
    apple_dt_set_prop_u32(node, "mix-n-match-prevention-status", 1);
    apple_dt_set_prop_u64(node, "unique-chip-id", t8030->ecid);
    // Update the display parameters
    apple_dt_set_prop_u32(node, "display-scale", 2);
    node = apple_dt_get_node(t8030->device_tree, "product");
    apple_dt_set_prop_u64(node, "display-corner-radius", 0x100000027);
    apple_dt_set_prop_u32(node, "oled-display", 1);
    apple_dt_set_prop_str(node, "graphics-featureset-class", "GLES2,0");
    apple_dt_set_prop_str(node, "graphics-featureset-fallbacks", "");
    apple_dt_set_prop_str(node, "artwork-display-gamut", "sRGB");

    node = apple_dt_get_node(t8030->device_tree, "chosen/lock-regs/amcc");
    if (node != NULL) {
        apple_dt_set_prop_u32(node, "aperture-count", 1);
        apple_dt_set_prop_u32(node, "aperture-size", AMCC_PLANE_COUNT * AMCC_PLANE_STRIDE + 0x4000);
        apple_dt_set_prop_u32(node, "plane-count", AMCC_PLANE_COUNT);
        apple_dt_set_prop_u32(node, "plane-stride", AMCC_PLANE_STRIDE);
        apple_dt_set_prop_u64(node, "aperture-phys-addr", AMCC_BASE);
        apple_dt_set_prop_u32(node, "cache-status-reg-offset", AMCC_PLANE_CACHE_CONTROL(0));    // ?? Apple Bug?
        apple_dt_set_prop_u32(node, "cache-status-reg-mask", 0x1F);
        apple_dt_set_prop_u32(node, "cache-status-reg-value", 0);
        apple_dt_set_prop_u32(node, "broadcast-reg-offset", AMCC_NON_PLANE_BROADCAST);
        apple_dt_set_prop_u32(node, "broadcast-reg-value", 1);

        node = apple_dt_get_node(node, "amcc-ctrr-a");
        assert_nonnull(node);

        apple_dt_set_prop_u32(node, "page-size-shift", 14);
        apple_dt_set_prop_u32(node, "lower-limit-reg-offset", AMCC_PLANE_LOWER_LIMIT(0));
        apple_dt_set_prop_u32(node, "lower-limit-reg-mask", 0xFFFFFFFF);
        apple_dt_set_prop_u32(node, "upper-limit-reg-offset", AMCC_PLANE_UPPER_LIMIT(0));
        apple_dt_set_prop_u32(node, "upper-limit-reg-mask", 0xFFFFFFFF);
        apple_dt_set_prop_u32(node, "lock-reg-offset", AMCC_PLANE_LOCK(0));
        apple_dt_set_prop_u32(node, "lock-reg-mask", 1);
        apple_dt_set_prop_u32(node, "lock-reg-value", 1);
        apple_dt_set_prop_u32(node, "enable-reg-offset", AMCC_PLANE_ENABLED(0));
        apple_dt_set_prop_u32(node, "enable-reg-value", 1);
        apple_dt_set_prop_u32(node, "enable-reg-mask", 1);
    }

    node = apple_dt_get_node(t8030->device_tree, "arm-io/pmgr");
    assert_nonnull(node);
    apple_dt_set_prop(node, "voltage-states0", sizeof(t8030_voltage_states0), t8030_voltage_states0);
    apple_dt_set_prop(node, "voltage-states1", sizeof(t8030_voltage_states1), t8030_voltage_states1);
    apple_dt_set_prop(node, "voltage-states2", sizeof(t8030_voltage_states2), t8030_voltage_states2);
    apple_dt_set_prop(node, "voltage-states5", sizeof(t8030_voltage_states5), t8030_voltage_states5);
    apple_dt_set_prop(node, "voltage-states8", sizeof(t8030_voltage_states8), t8030_voltage_states8);
    apple_dt_set_prop(node, "voltage-states9", sizeof(t8030_voltage_states9), t8030_voltage_states9);
    apple_dt_set_prop(node, "voltage-states10", sizeof(t8030_voltage_states10), t8030_voltage_states10);
    apple_dt_set_prop(node, "voltage-states11", sizeof(t8030_voltage_states11), t8030_voltage_states11);
    apple_dt_set_prop(node, "voltage-states1-sram", sizeof(t8030_voltage_states1_sram), t8030_voltage_states1_sram);
    apple_dt_set_prop(node, "voltage-states2-sram", sizeof(t8030_voltage_states2_sram), t8030_voltage_states2_sram);
    apple_dt_set_prop(node, "voltage-states5-sram", sizeof(t8030_voltage_states5_sram), t8030_voltage_states5_sram);
    apple_dt_set_prop(node, "voltage-states8-sram", sizeof(t8030_voltage_states8_sram), t8030_voltage_states8_sram);
    apple_dt_set_prop(node, "voltage-states9-sram", sizeof(t8030_voltage_states9_sram), t8030_voltage_states9_sram);
    apple_dt_set_prop(node, "bridge-settings-0", sizeof(t8030_bridge_settings0), t8030_bridge_settings0);
    apple_dt_set_prop(node, "bridge-settings-1", sizeof(t8030_bridge_settings1), t8030_bridge_settings1);
    apple_dt_set_prop(node, "bridge-settings-2", sizeof(t8030_bridge_settings2), t8030_bridge_settings2);
    apple_dt_set_prop(node, "bridge-settings-3", sizeof(t8030_bridge_settings3), t8030_bridge_settings3);
    apple_dt_set_prop(node, "bridge-settings-4", sizeof(t8030_bridge_settings4), t8030_bridge_settings4);
    apple_dt_set_prop(node, "bridge-settings-5", sizeof(t8030_bridge_settings5), t8030_bridge_settings5);
    apple_dt_set_prop(node, "bridge-settings-6", sizeof(t8030_bridge_settings6), t8030_bridge_settings6);
    apple_dt_set_prop(node, "bridge-settings-7", sizeof(t8030_bridge_settings7), t8030_bridge_settings7);
    apple_dt_set_prop(node, "bridge-settings-8", sizeof(t8030_bridge_settings8), t8030_bridge_settings8);
    apple_dt_set_prop(node, "bridge-settings-12", sizeof(t8030_bridge_settings12), t8030_bridge_settings12);
    apple_dt_set_prop(node, "bridge-settings-13", sizeof(t8030_bridge_settings13), t8030_bridge_settings13);
    apple_dt_set_prop(node, "bridge-settings-14", sizeof(t8030_bridge_settings14), t8030_bridge_settings14);
    apple_dt_set_prop(node, "bridge-settings-15", sizeof(t8030_bridge_settings15), t8030_bridge_settings15);
    apple_dt_set_prop(node, "bridge-settings-16", sizeof(t8030_bridge_settings16), t8030_bridge_settings16);
    apple_dt_set_prop(node, "bridge-settings-17", sizeof(t8030_bridge_settings17), t8030_bridge_settings17);

    node = apple_dt_get_node(t8030->device_tree, "arm-io/mipi-dsim/lcd");
    assert_nonnull(node);
    apple_dt_set_prop_u32(node, "lcd-panel-id", 0xA1C432D1);

    node = apple_dt_get_node(t8030->device_tree, "arm-io/bluetooth");
    assert_nonnull(node);
    apple_dt_set_prop(node, "local-mac-address", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x42, 0x42});

    node = apple_dt_get_node(t8030->device_tree, "arm-io/usb-drd");
    assert_nonnull(node);
    apple_dt_set_prop(node, "device-mac-address", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x22});
    apple_dt_set_prop(node, "host-mac-address", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x22, 0x12});

    node = apple_dt_get_node(t8030->device_tree, "chosen");
    assert_nonnull(node);
    apple_dt_set_prop(node, "mac-address-bluetooth0", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x42, 0x42});
    apple_dt_set_prop(node, "mac-address-ethernet0", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x22});
    apple_dt_set_prop(node, "mac-address-wifi0", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x22, 0x12});

    node = apple_dt_get_node(t8030->device_tree, "arm-io/apcie");
    assert_nonnull(node);
    // TODO: S8000 needs it, and T8030 probably does need it as well.
    apple_dt_set_prop_null(node, "apcie-phy-tunables");
    // do not use no-phy-power-gating for T8030
    //// apple_dt_set_prop_null(node, "no-phy-power-gating");

    node = apple_dt_get_node(t8030->device_tree, "arm-io/sio/iop-sio-nub");
    assert_nonnull(node);
    apple_dt_set_prop_u32(node, "pre-loaded", 1);
    apple_dt_set_prop_u32(node, "running", 1);

    node = apple_dt_get_node(t8030->device_tree, "arm-io/aop/iop-aop-nub");
    assert_nonnull(node);
    apple_dt_del_prop_named(node, "aop-memory-alignment");    // TODO: RTKit FW / Patchbay
    apple_dt_set_prop_u32(node, "pre-loaded", 1);
    apple_dt_set_prop_u32(node, "running", 1);

    node = apple_dt_get_node(t8030->device_tree, "arm-io/smc/iop-smc-nub");
    assert_nonnull(node);
    apple_dt_set_prop_u32(node, "pre-loaded", 1);
    apple_dt_set_prop_u32(node, "running", 1);

    node = apple_dt_get_node(t8030->device_tree, "arm-io/ans/iop-ans-nub");
    assert_nonnull(node);
    apple_dt_set_prop_u32(node, "pre-loaded", 1);
    apple_dt_set_prop_u32(node, "running", 1);

    node = apple_dt_get_node(t8030->device_tree, "arm-io/pmp/iop-pmp-nub");
    assert_nonnull(node);
    apple_dt_set_prop_u32(node, "pre-loaded", 1);
    apple_dt_set_prop_u32(node, "running", 1);

    node = apple_dt_get_node(t8030->device_tree, "cpus");
    assert_nonnull(node);

    for (iter = node->children, i = 0; iter; iter = next, ++i) {
        AppleDTNode* child;

        next  = iter->next;
        child = iter->data;
        if (i >= t8030_real_cpu_count(t8030)) {
            apple_dt_del_node(node, child);    // FIXME: Might not be sufficient
            continue;
        }

        if (t8030->cpus[i]->cpu_id == 0) { apple_dt_set_prop_str(child, "state", "running"); }
        // The host's under a hardware accelerator; the watchdog's clock (`fixed-frequency`) has to match it.
        apple_dt_set_prop_u64(child, "timebase-frequency", ARM_CPU(t8030->cpus[i])->gt_cntfrq_hz);
        apple_dt_set_prop_u64(child, "fixed-frequency", ARM_CPU(t8030->cpus[i])->gt_cntfrq_hz);
        apple_dt_set_prop_u64(child, "peripheral-frequency", 24000000);
        apple_dt_set_prop_u64(child, "memory-frequency", 24000000);
        apple_dt_set_prop_u64(child, "bus-frequency", 24000000);
        apple_dt_set_prop_u64(child, "clock-frequency", 24000000);
        apple_dt_del_prop_named(child, "reg-private");
        apple_dt_del_prop_named(child, "cpu-uttdbg-reg");
        apple_dt_del_prop_named(child, "coresight-reg");
    }

    node = apple_dt_get_node(t8030->device_tree, "arm-io/wdt");
    assert_nonnull(node);

    // FIXME: Why??
    apple_dt_set_prop_u32(node, "no-pmu", 1);

    node = apple_dt_get_node(t8030->device_tree, "arm-io/aic");
    apple_dt_set_prop_u32(node, "#main-cpus", t8030_real_cpu_count(t8030));
    apple_dt_set_prop_u32(node, "#shared-timestamps", 0);

    // h_active, h_back_porch, h_front_porch, h_sync_pulse, v_active, v_back_porch,
    // v_front_porch, v_sync_pulse
    // FIXME: Unhardcode.
    static const uint32_t timing_info[] = {828, 144, 1, 1, 1792, 1, 1, 1};
    node                                = apple_dt_get_node(t8030->device_tree, "arm-io/disp0");
    assert_nonnull(node);
    apple_dt_set_prop_str(node, "display-target", "DisplayTarget5");
    apple_dt_set_prop(node, "display-timing-info", sizeof(timing_info), timing_info);
    apple_dt_set_prop_u32(node, "bics-param-set", 0xD);
    apple_dt_set_prop_u32(node, "dot-pitch", 326);
    apple_dt_del_prop_named(node, "function-brightness_update");    // aop compass

    // FIXME: ???
    static const char* dart_nodes[] = {"dart-aop", "dart-sio"};
    for (i = 0; i < ARRAY_SIZE(dart_nodes); ++i) {
        node = apple_dt_get_node(t8030->device_tree, "arm-io");
        assert_nonnull(node);
        node = apple_dt_get_node(node, dart_nodes[i]);
        assert_nonnull(node);
        apple_dt_set_prop_u32(node, "vm-base", 0x4000);
    }

    // FIXME: ????
    node = apple_dt_get_node(t8030->device_tree, "arm-io/sep");
    assert_nonnull(node);
    apple_dt_set_prop_u32(node, "low-power-enable", 1);
}

static void t8030_set_sptm(AppleT8030MachineState* t8030, const char* filename, uint8_t* data, gsize len)
{
    if (t8030->sptm != NULL) { g_free(apple_boot_get_macho_buffer(t8030->sptm)); }
    t8030->sptm = apple_boot_load_monitor_data(filename, data, len, "sptm", "rspt");
    if (hwaccel_enabled()) { ck_patch_virt_monitor(t8030->sptm); }
}

static void t8030_set_txm(AppleT8030MachineState* t8030, const char* filename, uint8_t* data, gsize len)
{
    if (t8030->txm != NULL) { g_free(apple_boot_get_macho_buffer(t8030->txm)); }
    t8030->txm = apple_boot_load_monitor_data(filename, data, len, "trxm", "rtrx");
    ck_patch_txm(t8030->txm);
    if (hwaccel_enabled()) { ck_patch_virt_monitor(t8030->txm); }
}

/* What iBoot reads from Preboot. Images restored through DFU replace these. */
static void t8030_load_preboot_files(AppleT8030MachineState* t8030)
{
    uint8_t* data;
    gsize    len;

    if (t8030_load_preboot_file(PREBOOT_DEVICETREE, &data, &len, NULL)) {
        apple_dt_destroy(t8030->device_tree);
        t8030->device_tree = apple_boot_load_dt_data(PREBOOT_DEVICETREE, data, len);
    }

    if (t8030_load_preboot_file(PREBOOT_KERNELCACHE, &data, &len, NULL)) {
        t8030_set_kernel(t8030, apple_boot_load_kernel_data(PREBOOT_KERNELCACHE, data, len, NULL));
    }

    if (t8030_load_preboot_file(PREBOOT_TRUSTCACHE, &data, &len, NULL)) {
        g_free(t8030->trustcache);
        t8030->trustcache =
            apple_boot_load_trustcache_data(PREBOOT_TRUSTCACHE, data, len, &t8030->boot_info.trustcache_size);
    }

    if (t8030_load_preboot_file(PREBOOT_SPTM, &data, &len, NULL)) { t8030_set_sptm(t8030, PREBOOT_SPTM, data, len); }

    if (t8030_load_preboot_file(PREBOOT_TXM, &data, &len, NULL)) { t8030_set_txm(t8030, PREBOOT_TXM, data, len); }
}

static void t8030_memory_setup(AppleT8030MachineState* t8030)
{
    AppleDTNode*       carveout_memory_map;
    MachineState*      machine;
    MachoHeader64*     hdr;
    AppleNvram*        nvram;
    AppleBootInfo*     info;
    AppleDTNode*       memory_map;
    bool               have_autoboot;
    bool               auto_boot;
    bool               use_sptm;
    char*              cmdline;
    char*              seprom;
    gsize              fsize;
    uint8_t*           data;
    gsize              len;
    CarveoutAllocator* ca;
    CKPatcherRange*    seprom_range;

    if (t8030->securerom_filename != NULL) {
        if (t8030->securerom == NULL
            && !g_file_get_contents(t8030->securerom_filename, &t8030->securerom, &t8030->securerom_size, NULL))
        {
            error_setg(&error_fatal, "Failed to load SecureROM from `%s`", t8030->securerom_filename);
            return;
        }
    }
    else {
        t8030_load_preboot_files(t8030);
    }

    if (apple_dfu_copy_image(t8030->dfu, "rdtr", &data, &len)) {
        apple_dt_destroy(t8030->device_tree);
        t8030->device_tree = apple_boot_load_dt_data("DFU device tree", data, len);
    }

    if (t8030->device_tree != NULL) { apple_dt_unfinalise(t8030->device_tree); }

    // Only a booting kernel gets the display as iBoot leaves it, see `t8030_boot_display`.
    apple_dart_clear_boot_ranges(APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-disp0", &error_abort)));
    adp_v4_clear_boot_fb(APPLE_DISPLAY_PIPE_V4(object_property_get_link(OBJECT(t8030), "disp0", &error_abort)));

    carveout_memory_map = apple_dt_get_node(t8030->device_tree, "/chosen/carveout-memory-map");

    machine    = MACHINE(t8030);
    info       = &t8030->boot_info;
    memory_map = apple_dt_get_node(t8030->device_tree, "/chosen/memory-map");

    if (t8030_check_panic(t8030)) {
        qemu_system_guest_panicked(NULL);
        return;
    }

    info->dram_base = DRAM_BASE;
    info->dram_size = machine->maxram_size;

    ca = carveout_alloc_new(carveout_memory_map, info->dram_base, info->dram_size, 16 * KiB);

    t8030_rtkit_mem_setup(t8030, ca, "sio", "iop-sio-nub", SIO_SIZE);
    t8030_rtkit_mem_setup(t8030, ca, "ans", "iop-ans-nub", ANS_SIZE);

    if (t8030->sep_rom_filename) {
        if (!g_file_get_contents(t8030->sep_rom_filename, &seprom, &fsize, NULL)) {
            error_setg(&error_fatal, "Could not load data from file '%s'", t8030->sep_rom_filename);
            return;
        }

        seprom_range = ck_patcher_range_from_ptr("seprom", seprom, fsize);
        ck_sep_seprom_patches(seprom_range);
        if (hwaccel_enabled()) { ck_patch_virt_unmanaged(seprom_range, SEPROM_BASE); }

        address_space_rw(&address_space_memory, SEPROM_BASE, MEMTXATTRS_UNSPECIFIED, (uint8_t*)seprom, fsize, true);

        g_free(seprom);
    }

    nvram = APPLE_NVRAM(object_resolve_path_at(OBJECT(t8030), "peripheral/nvram"));
    if (nvram == NULL) {
        error_setg(&error_fatal, "Failed to find NVRAM device");
        return;
    }
    apple_nvram_load(nvram);

    info_report("Boot Mode: %u", t8030->boot_info.boot_mode);
    switch (t8030->boot_info.boot_mode) {
        case kAppleBootModeDFU:
            auto_boot = false;
            env_unset(nvram, "auto-boot");
            t8030->boot_info.boot_mode = kAppleBootModeAuto;
            break;
        case kAppleBootModeExitRecovery:
            auto_boot = true;
            env_set_bool(nvram, "auto-boot", true, 0);
            t8030->boot_info.boot_mode = kAppleBootModeAuto;
            break;
        default: break;
    }

    have_autoboot = env_find(nvram, "auto-boot") != NULL;
    auto_boot     = env_get_bool(nvram, "auto-boot", false);

    if (t8030->boot_info.non_cold_boot && (!t8030->boot_info.had_autoboot && have_autoboot && auto_boot)) {
        fprintf(stdout, "\n--------\nPotentially-completed restore process.\nPlease check the restore status, and "
                        "continue forth with the filesystem patches if success is observed.\n--------\n");
        exit(0);
        return;
    }

    t8030->boot_info.had_autoboot  = have_autoboot;
    t8030->boot_info.non_cold_boot = true;

    info_report("Auto Boot: %s", auto_boot ? "true" : "false");

    if (auto_boot) { cmdline = g_strdup(machine->kernel_cmdline); }
    else {
        cmdline = g_strconcat("-restore rd=md0 nand-enable-reformat=1 ", machine->kernel_cmdline, NULL);
    }

    // FIXME: Our current hardware WKDM implementation is broken. When fixed: only pass the arg under hwaccel.
    // FIXME: NVMe CoastGuard, see `boot.c`.
    cmdline = g_strconcat("-vm_compressor_wk_sw nvme-coastguard-disable=1 agm-trusted=1 agm-genuine=1 agm-authentic=1 ",
                          cmdline, NULL);

    info_report("Boot Args: [%s]", cmdline);

    AppleDTNode* chosen = apple_dt_get_node(t8030->device_tree, "chosen");

    if (env_find(nvram, "security-mode-change-enable") != NULL) {
        uint32_t val = env_get_uint(nvram, "security-mode-change-enable", 0);
        env_unset(nvram, "security-mode-change-enable");
        if (chosen != NULL) { apple_dt_set_prop_u32(chosen, "security-mode-change-enable", val); }
    }

    apple_boot_setup_boot_nonce(nvram, info);

    apple_nvram_save(nvram);

    info->nvram_size = nvram->len;

    if (info->nvram_size > XNU_MAX_NVRAM_SIZE) { info->nvram_size = XNU_MAX_NVRAM_SIZE; }
    if (apple_nvram_serialize(nvram, info->nvram_data, sizeof(info->nvram_data)) < 0) {
        error_report("Failed to read NVRAM");
    }

    t8030->video_args.base_addr = carveout_alloc_mem(ca, DISPLAY_SIZE);

    if (t8030->securerom_filename != NULL) {
        address_space_rw(&address_space_memory, SROM_BASE, MEMTXATTRS_UNSPECIFIED, t8030->securerom,
                         t8030->securerom_size, 1);
        return;
    }

    if (!auto_boot && !apple_dfu_booted(t8030->dfu)) {
        g_autofree uint8_t* nonce     = NULL;
        size_t              nonce_len = 0;

        apple_boot_compute_ap_nonce(QCRYPTO_HASH_ALGO_SHA384, info->boot_nonce_generator, &nonce, &nonce_len);
        apple_dfu_start(t8030->dfu, nonce, nonce_len, t8030_sep_nonce, sizeof(t8030_sep_nonce));
        g_free(cmdline);
        return;
    }

    if (t8030->device_tree == NULL) {
        error_setg(&error_fatal, "No device tree to boot; restore through DFU");
        return;
    }

    // HACK: Use DEV model to restore without FDR errors
    // NOTE: Previously restored back to AP, however,
    // iOS 16+ kernel panics because of the AP ticket,
    // as it names it "n104dev" and then tries to read
    // "n104ap" after restore.
    apple_dt_set_prop(t8030->device_tree, "compatible", 28, "N104DEV\0iPhone12,1\0AppleARM");

    if (!apple_boot_contains_boot_arg(cmdline, "rd=", true)) {
        apple_dt_set_prop_strn(chosen, "root-matching", 256,
                               "<dict><key>IOProviderClass</key><string>IOMedia</"
                               "string><key>IOPropertyMatch</key><dict><key>Partition "
                               "ID</key><integer>1</integer></dict></dict>");
    }

    AppleDTNode* pram = apple_dt_get_node(t8030->device_tree, "pram");
    if (pram != NULL) {
        uint64_t panic_reg[2];
        panic_reg[0] = carveout_alloc_mem(ca, PANIC_SIZE);
        panic_reg[1] = PANIC_SIZE;
        apple_dt_set_prop(pram, "reg", sizeof(panic_reg), &panic_reg);
        apple_dt_set_prop_u64(chosen, "embedded-panic-log-size", panic_reg[1]);
        t8030->panic_base = panic_reg[0];
        t8030->panic_size = panic_reg[1];
    }

    AppleDTNode* vram = apple_dt_get_node(t8030->device_tree, "vram");
    if (vram != NULL) {
        uint64_t vram_reg[2];
        vram_reg[0] = t8030->video_args.base_addr;
        vram_reg[1] = DISPLAY_SIZE;
        apple_dt_set_prop(vram, "reg", sizeof(vram_reg), &vram_reg);
    }
    // adjust framebuffer for verbose display.
    t8030->video_args.base_addr +=
        QEMU_ALIGN_DOWN(DISPLAY_SIZE - ((hwaddr)t8030->disp_height * t8030->disp_width * sizeof(uint32_t)), 16 * KiB);
    t8030->video_args.row_bytes    = t8030->disp_width * sizeof(uint32_t);
    t8030->video_args.width        = t8030->disp_width;
    t8030->video_args.height       = t8030->disp_height;
    t8030->video_args.depth.depth  = sizeof(uint32_t) * 8;
    t8030->video_args.depth.rotate = 1;

    t8030->video_args.display = !apple_boot_contains_boot_arg(machine->kernel_cmdline, "-s", false)
                                && !apple_boot_contains_boot_arg(machine->kernel_cmdline, "-v", false);

    if (t8030->boot_info.ticket_data != NULL) {
        g_free(t8030->boot_info.ticket_data);
        t8030->boot_info.ticket_data   = NULL;
        t8030->boot_info.ticket_length = 0;
    }

    if (!apple_dfu_copy_ticket(t8030->dfu, &info->ticket_data, &info->ticket_length)) {
        t8030_load_preboot_file(PREBOOT_APTICKET, (uint8_t**)&info->ticket_data, &info->ticket_length, NULL);
        if (t8030_load_preboot_file(PREBOOT_ROOT_HASH, &data, &len, NULL)) {
            apple_boot_load_root_hash_data(chosen, PREBOOT_ROOT_HASH, data, len);
        }
    }

    if (apple_dfu_copy_image(t8030->dfu, "rkrn", &data, &len)) {
        t8030_set_kernel(t8030, apple_boot_load_kernel_data("DFU kernelcache", data, len, NULL));
    }

    if (apple_dfu_copy_image(t8030->dfu, "rtsc", &data, &len)) {
        g_free(t8030->trustcache);
        t8030->trustcache = apple_boot_load_trustcache_data("DFU trust cache", data, len, &info->trustcache_size);
    }

    if (apple_dfu_copy_image(t8030->dfu, "rspt", &data, &len)) { t8030_set_sptm(t8030, "DFU SPTM", data, len); }

    if (apple_dfu_copy_image(t8030->dfu, "rtrx", &data, &len)) { t8030_set_txm(t8030, "DFU TXM", data, len); }

    hdr = t8030->kernel;
    if (hdr == NULL) {
        error_setg(&error_fatal, "No kernel to boot; restore through DFU");
        return;
    }

    apple_sio_set_protocol_version(APPLE_SIO(object_resolve_path_component(OBJECT(t8030), "sio")), t8030->sio_protocol);

    t8030_boot_display(t8030, ca);

    if (hwaccel_enabled()) { ck_patch_virt_monitor(hdr); }

    use_sptm = apple_boot_get_segment(hdr, "__DATA_SPTM") != NULL;
    if (use_sptm) {
        if (t8030->sptm == NULL || t8030->txm == NULL) {
            error_setg(&error_fatal, "This kernelcache needs SPTM and TXM");
            return;
        }
        apple_boot_allocate_sptm_records(memory_map);
        t8030_sptm_assign_dart_ids(t8030->device_tree);

        // SPTM types every frame below the top of kernel data, so TZ0 is carved out like on hardware.
        info->tz0_size = T8030_TZ0_SIZE;
        info->tz0_addr = ROUND_UP(carveout_alloc_mem(ca, info->tz0_size + T8030_TZ0_ALIGN), T8030_TZ0_ALIGN);
        info_report("TZ0 Physical Base: 0x" HWADDR_FMT_plx, info->tz0_addr);
    }
    else {
        apple_boot_allocate_segment_records(memory_map, hdr);
    }

    t8030_update_device_tree(t8030);

    apple_boot_populate_dt(t8030->device_tree, info, auto_boot);

    switch (hdr->file_type) {
        case MH_EXECUTE:
        case MH_FILESET:
            if (use_sptm) { t8030_load_sptm(t8030, cmdline, ca); }
            else {
                t8030_load_kernelcache(t8030, cmdline, ca);
            }
            g_free(cmdline);
            break;
        default: error_setg(&error_fatal, "Unsupported kernelcache type: 0x%X\n", hdr->file_type); return;
    }
}

static uint64_t pmgr_unk_e4800            = 0;
static uint32_t pmgr_unk_e4000[0x180 / 4] = {0};

static void pmgr_unk_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    hwaddr base = (hwaddr)opaque;
    hwaddr i;

    switch (base + addr) {
        case 0x23D2E4800:    // ???? 0x240002c00 and 0x2400037a4
            pmgr_unk_e4800 = deposit64(pmgr_unk_e4800, 0, size * 8,
                                       data);    // 0x240002c00 and 0x2400037a4
            break;
        case 0x23D2E4804                : pmgr_unk_e4800 = deposit64(pmgr_unk_e4800, 32, 32, data); break;
        case 0x23D2E4000 ... 0x23D2E417F:    // ???? 0x24000377c
            i                 = ((base + addr) - 0x23D2E4000) / 4;
            pmgr_unk_e4000[i] = extract64(data, 0, 32);    // 0x24000377c
            if (size == 8) { pmgr_unk_e4000[i + 1] = extract64(data, 32, 32); }
            break;
        default: break;
    }

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE unk @ 0x" HWADDR_FMT_plx
                  " base: 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n",
                  base + addr, base, data);
#endif
}

#define REG_MINIPMGR_SECURITY_AES_DISABLE                   (0x0)
#define MINIPMGR_SECURITY_AES_DISABLE_UID                   BIT(0)
#define MINIPMGR_SECURITY_AES_DISABLE_GID_SHIFT             (1)
#define MINIPMGR_SECURITY_AES_DISABLE_GID0                  (1 << MINIPMGR_SECURITY_AES_DISABLE_GID_SHIFT)
#define MINIPMGR_SECURITY_AES_DISABLE_GID1                  (2 << MINIPMGR_SECURITY_AES_DISABLE_GID_SHIFT)
#define MINIPMGR_SECURITY_AES_DISABLE_GID2                  (3 << MINIPMGR_SECURITY_AES_DISABLE_GID_SHIFT)
#define REG_MINIPMGR_SECURITY_MCC_BOOTROM_DIS               (0x10)
#define MINIPMGR_SECURITY_MCC_BOOTROM_DIS                   BIT(0)
#define REG_MINIPMGR_SECURITY_GPIO_STRAPS                   (0x20)
#define MINIPMGR_SECURITY_GPIO_STRAPS_BOARD_ID_MASK         (0x1F)
#define MINIPMGR_SECURITY_GPIO_STRAPS_BOARD_ID(v)           ((v) & MINIPMGR_SECURITY_GPIO_STRAPS_BOARD_ID_MASK)
#define MINIPMGR_SECURITY_GPIO_STRAPS_UNTRUSTED_BOOT_CONFIG BIT(16)
#define MINIPMGR_SECURITY_GPIO_STRAPS_VALID                 BIT(31)
#define REG_MINIPMGR_SECURITY_SET_ONLY                      (0x30)
#define MINIPMGR_SECURITY_SET_ONLY_MIX_AND_MATCH            BIT(31)
#define REG_MINIPMGR_SECURITY_SEP                           (0x34)
#define MINIPMGR_SECURITY_SEP_LOCK_SET                      BIT(8)
#define MINIPMGR_SECURITY_SEP_SECURE_MODE_CLR               BIT(8)
#define MINIPMGR_SECURITY_SEP_FIRST_BOOT                    BIT(24)
#define MINIPMGR_SECURITY_SEP_FIRST_AWAKE_BOOT              BIT(25)
#define MINIPMGR_SECURITY_SEP_ATTACK_CLK_GATE_DISABLE       BIT(31)

static uint64_t pmgr_unk_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleT8030MachineState* t8030 = APPLE_T8030(qdev_get_machine());
    AppleSEP*               sep;
    hwaddr                  base = (hwaddr)opaque;

#if 0
    if ((((base + addr) & 0xffffffffb) != 0x210E20020) &&
        (((base + addr) & 0xffffffffb) != 0x211E20020)) {
        qemu_log_mask(LOG_UNIMP,
                      "PMGR reg READ unk @ 0x" HWADDR_FMT_plx
                      " base: 0x" HWADDR_FMT_plx "\n",
                      base + addr, base);
    }
#endif

    uint32_t security_epoch          = 1;    // On IMG4: Security Epoch ; On IMG3: Minimum
                                             // Epoch, verified on SecureROM s5l8955xsi
    bool     current_prod            = true;
    bool     current_secure_mode     = true;    // T8015 SEPOS Kernel also requires this.
    uint32_t security_domain         = 1;
    bool     raw_prod                = true;
    bool     raw_secure_mode         = true;
    uint32_t sep_bit30_current_value = 0;
    bool     fuses_locked            = true;

    switch (base + addr) {
        case 0x23D280088:    // PMGR_AON
            return 0xFF;
        case 0x23D2BC000:    // Current Production Mode
            return current_prod ? FUSE_ENABLED : FUSE_DISABLED;
        case 0x23D2BC400:    // Raw Production Mode
            return raw_prod ? FUSE_ENABLED : FUSE_DISABLED;
        case 0x23D2BC004:    // Current Secure Mode
            sep = APPLE_SEP(object_property_get_link(OBJECT(t8030), "sep", NULL));

            if (sep != NULL && apple_sep_get_fuse_changer_bit(sep, 1)) {
                current_secure_mode = 0;    // SEP DSEC img4 tag demotion active
            }
            return current_secure_mode ? FUSE_ENABLED : FUSE_DISABLED;
        case 0x23D2BC404:    // Raw Secure Mode
            return raw_secure_mode ? FUSE_ENABLED : FUSE_DISABLED;
        case 0x23D2BC008:    // Current Security Domain Bit 0
        case 0x23D2BC408:    // Raw Security Domain Bit 0
            return (security_domain & BIT32(0)) == 0 ? FUSE_DISABLED : FUSE_ENABLED;
        case 0x23D2BC00C:    // Current Security Domain Bit 1
        case 0x23D2BC40C:    // Raw Security Domain Bit 1
            return (security_domain & BIT32(1)) == 0 ? FUSE_DISABLED : FUSE_ENABLED;
        case 0x23D2BC010:    // Current Board ID and Epoch
            sep = APPLE_SEP(object_property_get_link(OBJECT(t8030), "sep", NULL));

            if (sep != NULL) { sep_bit30_current_value = (uint32_t)apple_sep_get_fuse_changer_bit(sep, 0) << 30; }
            QEMU_FALLTHROUGH;
        case 0x23D2BC410:    // Raw Board ID and Epoch, bit 30 should remain unset
            return ((t8030->board_id >> 5) & 0x7) | ((security_epoch & 0x7f) << 5) | sep_bit30_current_value
                   | ((uint32_t)fuses_locked << 31);
        case 0x23D2BC020:    // Fuses Sealed?
            return current_prod ? FUSE_ENABLED : FUSE_DISABLED;
        // case 0x23D2BC024: // ?
        //     return ...;
        // case 0x23D2BC028: // ?
        //     return ...;
        case 0x23D2BC02C:                    // Unknown
            return (0 << 31) | BIT32(30);    // bit31 causes a panic
        case 0x23D2BC030:                    // Chip Revision
            return ((t8030->chip_revision & 0x7) << 6) | (((t8030->chip_revision & 0x70) >> 4) << 5) | (0 << 1);
        case 0x23D2BC300:    // ECID LOW T8030
            return extract64(t8030->ecid, 0, 32);
        case 0x23D2BC304:    // ECID HIGH T8030
            return extract64(t8030->ecid, 32, 32);
        case 0x23D2BC30C: {
            // T8030 SEP Chip Revision
            //  1 vs. not 1: TRNG/Monitor
            //  0 vs. not 0: Monitor
            //  2 vs. not 2: ARTM
            //  Production SARS doesn't like value (0 << 28) in combination with
            //  kbkdf_index being 0
            uint32_t environment;
            // environment = 0x0; // 0 ; might be the production value
            // environment = 0x2; // 1
            // environment = 0x3; // 1
            environment = 0x8;    // 2 ; might be a development value, skips a few
                                  // checks (lynx and others)
            return (environment << 28);
        }
        case 0x23D2E8000:    // ????
            // return 0x32B3; // memory encryption AMK (Authentication Master Key)
            // disabled
            return 0xC2E9;    // memory encryption AMK (Authentication Master Key)
                              // enabled
        case 0x23D2E4800:     // sep: likely lock bits: 1 on <= A13, 3 on A14
            return pmgr_unk_e4800;
        case 0x23D2E4000 ... 0x23D2E417F:    // sep: something memory encryption and sepb
            return pmgr_unk_e4000[((base + addr) - 0x23D2E4000) / 4];
        case 0x23D2D0000 + REG_MINIPMGR_SECURITY_AES_DISABLE:
            return MINIPMGR_SECURITY_AES_DISABLE_GID1 | MINIPMGR_SECURITY_AES_DISABLE_UID;
        case 0x23D2D0000 + REG_MINIPMGR_SECURITY_GPIO_STRAPS:
            return MINIPMGR_SECURITY_GPIO_STRAPS_BOARD_ID(t8030->board_id) | MINIPMGR_SECURITY_GPIO_STRAPS_VALID;
        case 0x23D2D0000 + REG_MINIPMGR_SECURITY_SET_ONLY: return 0x00;
        case 0x23D2D0000 + REG_MINIPMGR_SECURITY_SEP:
            return MINIPMGR_SECURITY_SEP_FIRST_BOOT | MINIPMGR_SECURITY_SEP_FIRST_AWAKE_BOOT;
        case 0x23D2D0000 + REG_MINIPMGR_SECURITY_MCC_BOOTROM_DIS:
            // normally 0x0 for SecureROM, but it only checks after writing to it
            return MINIPMGR_SECURITY_MCC_BOOTROM_DIS;    // for iBoot (and SecureROM)
        case 0x23D2D0000 + 0x4040:                       // PKA_SECU 0xd4040
        case 0x23D2D0000 + 0x4084:                       // PKA_SECU 0xd4084
            return 0x10000;                              // prevent (e.g. SEPOS 18.5) PKA panic
        default:
            if (base >= 0x23D2D0000 && addr < 0x8000) { return 0xFF; }
            // eACC Temp Sensor
            if ((base + addr) >= 0x210ED0000 && (base + addr) < 0x210ED0000 + 0x10000) { return 0xAFAFAFAF; }
            // pACC Temp Sensor
            if ((base + addr) >= 0x211ED0000 && (base + addr) < 0x211ED0000 + 0x10000) { return 0xAFAFAFAF; }
            // pACC MTR Temp Sensor
            if ((base + addr) >= 0x211EE1000 && (base + addr) < 0x211EE1000 + 0x16000) { return 0xAFAFAFAF; }
            // SOC MTR Temp Sensor0
            if ((base + addr) >= 0x23B281000 && (base + addr) < 0x23B281000 + 0x8000) { return 0xAFAFAFAF; }
            // SOC MTR Temp Sensor3
            if ((base + addr) >= 0x23B289000 && (base + addr) < 0x23B289000 + 0x8000) { return 0xAFAFAFAF; }
            // ANE MTR Temp Sensor0
            if ((base + addr) >= 0x23B299000 && (base + addr) < 0x23B299000 + 0x8000) { return 0xAFAFAFAF; }
            if (((base + addr) & 0x210E70000) == 0x210E70000) {
                return (108 << 4) | 0x200000;    // ?
            }
            return 0;
    }
}

static const MemoryRegionOps pmgr_unk_reg_ops = {
    .write                 = pmgr_unk_reg_write,
    .read                  = pmgr_unk_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 8,
    .valid.unaligned       = false,
};

typedef struct T8030PMGRPowerWrite
{
    AppleT8030MachineState* t8030;
    hwaddr                  addr;
    uint64_t                data;
} T8030PMGRPowerWrite;

static void pmgr_power_write_bh(void* opaque)
{
    g_autofree T8030PMGRPowerWrite* w = opaque;
    AppleSEP*                       sep;

    switch (w->addr) {
        case 0xD4004: t8030_start_cpus(w->t8030, w->data); break;
        case 0x80C00:    // SEP_PS
            sep = APPLE_SEP(object_property_get_link(OBJECT(w->t8030), "sep", NULL));

            if (sep != NULL) {
                if (w->data & BIT32(31)) { apple_a13_reset(APPLE_A13(apple_sep_get_cpu(sep))); }
                else if (w->data & BIT32(10)) {
                    apple_a13_set_off(APPLE_A13(apple_sep_get_cpu(sep)));
                }
                else {
                    apple_a13_set_on(APPLE_A13(apple_sep_get_cpu(sep)));
                }
            }
            break;
        default: assert_not_reached();
    }
}

static void pmgr_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleT8030MachineState* t8030 = opaque;

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx
                  "\n",
                  addr, data);
#endif

    if (addr >= 0x80000 && addr <= 0x8C000) {
        data = deposit64(data, 4, 4, extract64(data, 0, 4));
        if (size == 8) { data = deposit64(data, 32 + 4, 4, extract64(data, 32, 4)); }
    }

    switch (addr) {
        case 0xD4004:
        case 0x80C00: {    // SEP_PS
            T8030PMGRPowerWrite* w = g_new(T8030PMGRPowerWrite, 1);

            w->t8030 = t8030;
            w->addr  = addr;
            w->data  = data;
            aio_bh_schedule_oneshot(qemu_get_aio_context(), pmgr_power_write_bh, w);
            if (addr == 0xD4004) { return; }
            break;
        }
        default: break;
    }

    if (size == 8) { stq_le_p(t8030->pmgr_reg + addr, data); }
    else {
        stl_le_p(t8030->pmgr_reg + addr, data);
    }
}

static uint64_t pmgr_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleT8030MachineState* t8030  = opaque;
    uint64_t                result = 0;

    switch (addr) {
        case 0xF0010:    // AppleT8030PMGR::commonSramCheck
            result = 0x5000;
            break;
        default: result = size == 8 ? ldq_le_p(t8030->pmgr_reg + addr) : ldl_le_p(t8030->pmgr_reg + addr); break;
    }

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg READ @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx
                  "\n",
                  addr, result);
#endif

    return result;
}

static const MemoryRegionOps pmgr_reg_ops = {
    .write                 = pmgr_reg_write,
    .read                  = pmgr_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 8,
    .valid.unaligned       = false,
};

static void amcc_handle_plane_reg_write(AppleT8030MachineState* t8030, hwaddr plane_i, hwaddr plane_off, uint64_t data)
{
    uint32_t val;
    if (plane_off == AMCC_PLANE_CACHE_CONTROL(0)) {
        val = AMCC_RREG32(t8030, AMCC_PLANE_CACHE_STATUS(plane_i));
        val = deposit32(val, 5, 5, data & 0x1F);     // cur tag ways
        val = deposit32(val, 10, 5, data & 0x1F);    // cur data ways
        AMCC_WREG32(t8030, AMCC_PLANE_CACHE_STATUS(plane_i), val);
    }
    stl_le_p(t8030->amcc_reg + (plane_i * AMCC_PLANE_STRIDE) + plane_off, data);
}

static void amcc_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleT8030MachineState* t8030 = opaque;

    if (addr < AMCC_PLANE_COUNT * AMCC_PLANE_STRIDE) {
        hwaddr plane_i   = addr / AMCC_PLANE_STRIDE;
        hwaddr plane_off = addr % AMCC_PLANE_STRIDE;

        if (((addr == AMCC_PLANE_LOWER_LIMIT(plane_i) || addr == AMCC_PLANE_UPPER_LIMIT(plane_i)
              || addr == AMCC_PLANE_LOCK(plane_i))
             && AMCC_RREG32(t8030, AMCC_PLANE_LOCK(plane_i)) != 0)
            || ((addr == AMCC_PLANE_TZ0_BASE(plane_i) || addr == AMCC_PLANE_TZ0_END(plane_i)
                 || addr == AMCC_PLANE_TZ0_LOCK(plane_i))
                && AMCC_RREG32(t8030, AMCC_PLANE_TZ0_LOCK(plane_i)) != 0))
        {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx ", this register is LOCKED, merdOS.\n",
                          __func__, addr, data);
            return;
        }

        if (AMCC_RREG32(t8030, AMCC_NON_PLANE_BROADCAST)) {
            for (hwaddr i = 0; i < AMCC_PLANE_COUNT; ++i) { amcc_handle_plane_reg_write(t8030, i, plane_off, data); }
            return;
        }
        amcc_handle_plane_reg_write(t8030, plane_i, plane_off, data);
        return;
    }

    stl_le_p(t8030->amcc_reg + addr, data);
}

static uint64_t amcc_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleT8030MachineState* t8030 = opaque;
    uint64_t                result;

    result = ldl_le_p(t8030->amcc_reg + addr);

#if 0
    qemu_log_mask(LOG_UNIMP, "AMCC reg READ @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", addr, result);
#endif

    return result;
}

static const MemoryRegionOps amcc_reg_ops = {
    .write                 = amcc_reg_write,
    .read                  = amcc_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void t8030_pmgr_init_mmio(AppleT8030MachineState* t8030, MemoryRegion* mem, const MemoryRegionOps* ops,
                                 void* opaque, const char* name, hwaddr addr, hwaddr size)
{
    memory_region_init_io(mem, OBJECT(t8030), ops, opaque == NULL ? (void*)addr : opaque, name, size);
    memory_region_enable_lockless_io(mem);
    memory_region_add_subregion(get_system_memory(), addr, mem);
}

#ifdef ENABLE_BASEBAND
static void t8030_create_baseband(AppleT8030MachineState* t8030, DeviceState* gpio)
{
    DeviceState*   baseband;
    ApplePCIEPort* port = APPLE_PCIE_PORT(object_resolve_path_at(OBJECT(t8030), "apcie/bridge3"));

    baseband = qdev_new(TYPE_APPLE_BASEBAND);
    object_property_add_child(OBJECT(t8030), "baseband", OBJECT(baseband));
    object_property_set_link(OBJECT(baseband), "port", OBJECT(port), &error_fatal);
    object_property_set_link(OBJECT(baseband), "smc", object_property_get_link(OBJECT(t8030), "smc", &error_fatal),
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(baseband), &error_fatal);
    qdev_connect_gpio_out(gpio, 0x2C, qdev_get_gpio_in_named(baseband, BASEBAND_GPIO_COREDUMP, 0));
    qdev_connect_gpio_out_named(baseband, BASEBAND_GPIO_RESET_DET_OUT, 0, qdev_get_gpio_in(gpio, 0x2D));

    #if 0
    uint32_t bridge_index = 3;
    qdev_connect_gpio_out_named(
        DEVICE(port->host), "interrupt_pci", bridge_index,
        qdev_get_gpio_in_named(baseband, "interrupt_pci", 0));
    #endif
}

static void t8030_create_baseband_spmi(AppleT8030MachineState* t8030, AppleSPMIState* spmi)
{
    DeviceState* baseband;

    baseband = qdev_new(TYPE_APPLE_SPMI_BASEBAND);
    object_property_add_child(OBJECT(t8030), "baseband-spmi", OBJECT(baseband));
    spmi_set_slave_sid(SPMI_SLAVE(baseband), 0xD);
    // The interrupts of the baseband node seem to be actually for baseband-spmi.
    qdev_connect_gpio_out(baseband, 0, qdev_get_gpio_in(DEVICE(spmi), 0x2D));
    spmi_slave_realize_and_unref(SPMI_SLAVE(baseband), spmi->bus, &error_fatal);

    qemu_register_wakeup_support();
}
#endif

static AppleSPMIState* t8030_create_spmi(AppleT8030MachineState* t8030, const char* name, hwaddr addr, uint32_t irq)
{
    SysBusDevice* sbd;

    sbd             = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_SPMI));
    DEVICE(sbd)->id = g_strdup(name);
    object_property_add_child(OBJECT(t8030), name, OBJECT(sbd));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, addr);
    sysbus_connect_irq(sbd, 0, t8030_irq(t8030, irq));

    return APPLE_SPMI(sbd);
}

static void t8030_cpu_reset(AppleT8030MachineState* t8030)
{
    CPUState*      cpu;
    ARMCPU*        arm_cpu;
    AppleA13State* acpu;
    uint64_t       m_lo;
    uint64_t       m_hi;

    if (apple_dfu_waiting(t8030->dfu)) { return; }

    qemu_guest_getrandom(&m_lo, sizeof(m_lo), NULL);
    qemu_guest_getrandom(&m_hi, sizeof(m_hi), NULL);

    CPU_FOREACH (cpu) {
        arm_cpu = container_of(cpu, ARMCPU, parent_obj);
        acpu    = container_of(arm_cpu, AppleA13State, parent_obj);

        arm_cpu->m_key_lo = m_lo;
        arm_cpu->m_key_hi = m_hi;

        if (t8030->securerom_filename == NULL) {
            if (acpu->cpu_id != A13_MAX_CPU) {
                object_property_set_uint(OBJECT(cpu), "rvbar", t8030->boot_info.kern_entry & ~0xFFFULL, &error_abort);
                cpu_reset(cpu);
            }
            if (acpu->cpu_id == 0) {
                arm_set_cpu_on(arm_cpu->mp_affinity, t8030->boot_info.kern_entry, t8030->boot_info.kern_boot_args_addr,
                               1, true);
            }
        }
        else {
            if (acpu->cpu_id != A13_MAX_CPU) {
                object_property_set_uint(OBJECT(cpu), "rvbar", SROM_BASE, &error_abort);
                cpu_reset(cpu);
            }
            if (acpu->cpu_id == 0) { arm_set_cpu_on(arm_cpu->mp_affinity, SROM_BASE, 0, 1, true); }
        }
        async_run_on_cpu(cpu, t8030_iboot_handoff_state, RUN_ON_CPU_NULL);
    }
}

typedef struct
{
    AppleT8030MachineState* t8030;
    uint32_t                width;
    uint32_t                height;
    pixman_transform_t      transform;
    double                  dest_width;
    int16_t                 dest_x;
    int16_t                 dest_y;
    pixman_image_t*         image;
    pixman_image_t*         disp_image;
} T8030DrawBootSplashContext;

static void t8030_draw_boot_splash(void* opaque)
{
    T8030DrawBootSplashContext* ctx = opaque;

    pixman_image_composite(PIXMAN_OP_SRC, ctx->image, NULL, ctx->disp_image, 0, 0, 0, 0, ctx->dest_x, ctx->dest_y,
                           ctx->dest_width, ctx->dest_width);
}

static void t8030_draw_boot_splash_timer(void* opaque)
{
    T8030DrawBootSplashContext* ctx = opaque;

    t8030_draw_boot_splash(ctx);

    pixman_image_unref(ctx->image);
    pixman_image_unref(ctx->disp_image);
    timer_free(ctx->t8030->boot_splash_timer);
    ctx->t8030->boot_splash_timer = NULL;
    g_free(ctx);
}

// Please see `ui/icons/CKBrandingNotice.md`
static void t8030_read_and_draw_boot_splash(AppleT8030MachineState* t8030)
{
    char*                       path;
    FILE*                       fp;
    uint8_t                     sig[8] = {0};
    png_structp                 png_ptr;
    png_infop                   info_ptr;
    T8030DrawBootSplashContext* ctx;
    uint32_t*                   data;
    png_bytep*                  row_ptrs;

    path = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/512x512/apps/inferno/bootsplash@2x.png");
    assert_nonnull(path);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        error_setg(&error_abort, "Missing emulator branding: %s.", path);
        return;
    }
    fread(sig, sizeof(sig), 1, fp);
    if (png_sig_cmp(sig, 0, sizeof(sig)) != 0) {
        error_setg(&error_abort, "Invalid emulator branding: %s.", path);
        return;
    }
    g_free(path);
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert_nonnull(png_ptr);
    info_ptr = png_create_info_struct(png_ptr);
    assert_nonnull(info_ptr);
    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, sizeof(sig));

    png_read_info(png_ptr, info_ptr);
    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png_ptr, info_ptr);

    ctx         = g_new(T8030DrawBootSplashContext, 1);
    ctx->width  = png_get_image_width(png_ptr, info_ptr);
    ctx->height = png_get_image_height(png_ptr, info_ptr);
    ctx->image =
        pixman_image_create_bits(PIXMAN_a8b8g8r8, ctx->width, ctx->height, NULL, ctx->width * sizeof(uint32_t));
    data = pixman_image_get_data(ctx->image);

    row_ptrs = g_new(png_bytep, ctx->height);
    for (size_t y = 0; y < ctx->height; y++) { row_ptrs[y] = (png_bytep)(data + (y * ctx->width)); }

    png_read_image(png_ptr, row_ptrs);

    g_free(row_ptrs);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    ctx->t8030      = t8030;
    ctx->dest_width = (double)t8030->disp_width / 1.25;
    ctx->dest_x     = (t8030->disp_width / 2) - (ctx->dest_width / 2);
    ctx->dest_y     = (t8030->disp_height / 2) - (ctx->dest_width / 2);
    ctx->disp_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, t8030->disp_width, t8030->disp_height,
                                               memory_region_get_ram_ptr(MACHINE(t8030)->ram)
                                                   + (t8030->video_args.base_addr - DRAM_BASE),
                                               t8030->disp_width * sizeof(uint32_t));

    pixman_image_set_filter(ctx->image, PIXMAN_FILTER_BEST, NULL, 0);
    pixman_transform_init_identity(&ctx->transform);
    pixman_transform_scale(&ctx->transform, NULL, pixman_double_to_fixed((double)ctx->width / ctx->dest_width),
                           pixman_double_to_fixed((double)ctx->height / ctx->dest_width));
    pixman_image_set_transform(ctx->image, &ctx->transform);

    t8030_draw_boot_splash(ctx);

    t8030->boot_splash_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, t8030_draw_boot_splash_timer, ctx);

    // Workaround for `-v` removing the boot splash.
    timer_mod(t8030->boot_splash_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (NANOSECONDS_PER_SECOND / 2));
}

static void t8030_reset(MachineState* machine, ResetType type)
{
    AppleT8030MachineState* t8030 = APPLE_T8030(machine);

    memset(t8030->pmgr_reg, 0, sizeof(t8030->pmgr_reg));
    memset(t8030->amcc_reg, 0, sizeof(t8030->amcc_reg));

    pmgr_unk_e4800 = 0;
    // maybe also reset pmgr_unk_e4000 array
    // Ah, what the heck. Let's do it.
    memset(pmgr_unk_e4000, 0, sizeof(pmgr_unk_e4000));

    qemu_devices_reset(type);

    t8030_memory_setup(t8030);

    t8030_cpu_reset(t8030);

    t8030_read_and_draw_boot_splash(t8030);
}

static void pmp_rtk_boot_done(void* opaque)
{
    AppleRTKit* rtk = opaque;
    apple_rtkit_send_user_msg(rtk, 0, 0);    // ready
}

static AppleRTKitOps pmp_rtk_ops = {
    .boot_done = pmp_rtk_boot_done,
};

static AppleDARTState* pmp_dart;
static AddressSpace    pmp_dma_as;

// 52..56 Cmd Type
// 0: Ctrl? Command, 48..52
//     0x00: Ready
//     0x01: Init Memory
//     0x02: Init Memory Ack
//     0x03: Set Property
//     0x04: Set Property Ack
//     0x05: Ping
//     0x06: Ping Ack
// 1: eHub Command
//     0x00/0x02: Node
//     0x04/0x06: Time Series
// 2: PM Command, 44..52
// 3: pMon
// 4..5: ??
// 6: pBatt / Battery Report, bit 32: enable
// 7: Fast Die
static void pmp_rtk_ep0_handler(void* opaque, uint8_t ep, uint64_t msg)
{
    static const char* const domains[] = {"SOC", "DCS", "DISP", "AVE"};
    static const char* const states[]  = {"VMIN", "VNOM", "VMAX"};
    static const char* const devices[] = {
        "CPU",  "ECPU", "PCPU", "GPU", "ISP",  "VDD_AVE", "DISP0", "VENC",
        "VDEC", "JPEG", "MSR",  "ANE", "PCIE", "SBR",     "SEP",
    };
    AppleRTKit* rtk        = opaque;
    uint8_t     buf[0x510] = {0};
    uint8_t     cmd;

    switch (extract64(msg, 52, 4)) {
        case 0:
            cmd = extract64(msg, 48, 4);
            // init memory: fill the domain/device tables, ack without eHub
            if (cmd == 1) {
                for (size_t i = 0; i < ARRAY_SIZE(domains); i++) {
                    strncpy((char*)buf + i * 0x2C, domains[i], 8);
                    for (size_t j = 0; j < ARRAY_SIZE(states); j++) {
                        strncpy((char*)buf + i * 0x2C + 8 + j * 0xC, states[j], 8);
                    }
                }
                for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
                    strncpy((char*)buf + 0xB0 + i * 0x18, devices[i], 8);
                }
                dma_memory_write(&pmp_dma_as, extract64(msg, 0, 48) + 0x4000, buf, sizeof(buf), MEMTXATTRS_UNSPECIFIED);
            }
            apple_rtkit_send_user_msg(rtk, ep, deposit64(0, 48, 4, cmd + 1));
            break;
        case 2:
            cmd = extract64(msg, 44, 8);
            if (cmd >= 0xE) { break; }
            msg = deposit64(msg, 44, 8, cmd + 1);
            if (extract64(msg, 0, 32) == 0) { msg |= 1; }    // BIRQ/CIRQ fail on a zero payload
            apple_rtkit_send_user_msg(rtk, ep, msg);
            break;
        default: break;
    }
}

static void t8030_init(MachineState* machine)
{
    AppleT8030MachineState* t8030;
    DeviceState*            dev;
    DeviceState*            gpio;
    DeviceState*            aop_gpio;
    AppleSPMIState*         spmi;
    SysBusDevice*           sbd;
    SysBusDevice*           aop;
    SysBusDevice*           sio;
    SysBusDevice*           sart;
    AppleSPIState*          spi;
    AppleI2CState*          i2c;
    AppleDARTState*         dart;
    uint32_t                i;
    uint32_t                cluster_id;
    char                    name[64];
    I2CSlave*               i2cs;
    DeviceState*            host;
    IOMMUMemoryRegion*      iommu;
    USBDevice*              dfu;
    AppleSEP*               sep;

    t8030 = APPLE_T8030(machine);

    if (t8030->sep_rom_filename == NULL) {
        if (machine->smp.cpus > A13_MAX_CPU) {
            error_setg(&error_fatal, "Superfluous CPU cores specified for simulated SEP!");
            return;
        }
    }
    else if (machine->smp.cpus < 2) {
        error_setg(&error_fatal, "Insufficient CPU cores specified for emulated SEP!");
        return;
    }

    t8030->chip_revision = 0x20;
    t8030->board_id      = 0x4;

    memory_region_add_subregion(get_system_memory(), DRAM_BASE, machine->ram);

    if (t8030->sep_rom_filename != NULL) {
        allocate_ram(get_system_memory(), "SEPROM", SEPROM_BASE, SEPROM_SIZE, 0);
        // SEP_UNKN0 is now MISC0
        allocate_ram(get_system_memory(), "SEP_UNKN1", 0x242200000ULL, 0x24000, 0);
        // for last_jump
        allocate_ram(get_system_memory(), "SEP_UNKN10", 0x242150000ULL, 0x4000, 0);
        allocate_ram(get_system_memory(), "SEP_UNKN11", 0x241010000ULL, 0x4000, 0);
        // SEP_UNKN12 is now MISC1
        // 0x242400000 is apple-a7iop.SEP.regs
        // sepfw 26.2beta2/26.3.1 says it's (a7iop) size would be 0x64000
        // stack for 0x340005BF4/SEPFW
        allocate_ram(get_system_memory(), "SEP_UNKN13", 0x24020C000ULL, 0x4000, 0);
        // for SEP Panic: [elfour panic] [/,&&&&+&] exception.c:'&.
        allocate_ram(get_system_memory(), "SEP_UNKN14", 0x240A80000ULL, 0x4000, 0);
        allocate_ram(get_system_memory(), "SEP_UNKN15", 0x240A90000ULL, 0x4000, 0);
        // for sepfw 26.2beta2/26.3.1
        allocate_ram(get_system_memory(), "SEP_UNKN16", 0x242000000ULL, 0x400000, 0);
        allocate_ram(get_system_memory(), "SEP_dram_nubs", 0x23DF00000ULL, 0x20000, 0);
        allocate_ram(get_system_memory(), "SEP_dram_nubg", 0x23D000000ULL, 0x4000, 0);
        // dram_scr address 0x23D2C0000 size 0x8000 inside pmgr-unk-reg-2
        // dram_pln0 address 0x200000000 size 0x8000, inside amcc
    }

    if (t8030->securerom_filename != NULL) {
        allocate_ram(get_system_memory(), "SROM", SROM_BASE, SROM_SIZE, 0);
        allocate_ram(get_system_memory(), "SRAM", SRAM_BASE, SRAM_SIZE, 0);
    }

    for (i = 0; i < A13_MAX_CLUSTER; ++i) {
        snprintf(name, sizeof(name), "cluster%d", i);
        object_initialize_child(OBJECT(t8030), name, &t8030->clusters[i], TYPE_APPLE_A13_CLUSTER);
        qdev_prop_set_uint32(DEVICE(&t8030->clusters[i]), "cluster-id", i);
    }

    for (i = 0; i < t8030_real_cpu_count(t8030); ++i) {
        snprintf(name, sizeof(name), "cpu%d", i);
        cluster_id     = i > 3;
        t8030->cpus[i] = apple_a13_create(name, i, i - (cluster_id == 0 ? 0 : 4) | (cluster_id << 8), cluster_id,
                                          cluster_id == 0 ? 'E' : 'P');

        object_property_add_child(OBJECT(&t8030->clusters[cluster_id]), DEVICE(t8030->cpus[i])->id,
                                  OBJECT(t8030->cpus[i]));
        qdev_realize(DEVICE(t8030->cpus[i]), NULL, &error_fatal);
        if (hwaccel_enabled()) { apple_virt_manage_cpu(CPU(t8030->cpus[i])); }
    }

    for (i = 0; i < A13_MAX_CLUSTER; ++i) { qdev_realize(DEVICE(&t8030->clusters[i]), NULL, &error_fatal); }

    t8030->aic = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_AIC));
    object_property_add_child(OBJECT(t8030), "aic", OBJECT(t8030->aic));
    qdev_prop_set_uint32(DEVICE(t8030->aic), "num-cpu", t8030_real_cpu_count(t8030));
    qdev_prop_set_uint32(DEVICE(t8030->aic), "num-eir", 0x12);
    sysbus_realize_and_unref(t8030->aic, &error_fatal);

    for (i = 0; i < t8030_real_cpu_count(t8030); ++i) {
        memory_region_add_subregion_overlap(&t8030->cpus[i]->memory, 0x23B100000,
                                            sysbus_mmio_get_region(t8030->aic, i * 2), 0);
        memory_region_add_subregion_overlap(&t8030->cpus[i]->memory, 0x23B108000,
                                            sysbus_mmio_get_region(t8030->aic, i * 2 + 1), 0);
        sysbus_connect_irq(t8030->aic, i, qdev_get_gpio_in(DEVICE(t8030->cpus[i]), ARM_CPU_IRQ));
    }

    for (i = 0; i < NUM_UARTS; ++i) {
        apple_uart_create(0x235200000 + (0x4000 * i), t8030_irq(t8030, 0x1B3 + i), serial_hd(i));
    }

    /*
     * DT reg indices:
     * 0 PMGR, 1 MINI, 2 AOP Global AP Controls, 3 ACC Power Control, 4 ACC Impl, 5 CPM Impl, 6 ACC Debug,
     * 7 ACC DVFM, 8..11 ACC CPU0..3, 12 ACC PM1 Power Control, 13 PACC Impl, 14..16 (unlabelled),
     * 17..18 PACC CPU0..1, 19 PMP Ext SW Gen, 20 (unlabelled), 21 Dock FIFO, 22 SOC DPE, 23 ANE DPE PPT SYS,
     * 24 ANE DPE PPT LEE, 25..43 Bridge 0..18
     */
    /* FIXME: Bridge 8 not arm-io, is APCIE */
    static const hwaddr pmgr_regs[][2] = {
        {0x23B000000, 0x100000}, {0x23D200000, 0xB0000}, {0x23D2B4000, 0x4020},  {0x23D2B8024, 0x47FDC},
        {0x23D004000, 0x1000},   {0x210F00000, 0x50000}, {0x210E70000, 0x90000}, {0x210050000, 0x10000},
        {0x210150000, 0x10000},  {0x210250000, 0x10000}, {0x210350000, 0x10000}, {0x211F00000, 0x50000},
        {0x211E70000, 0x90000},  {0x211050000, 0x10000}, {0x211150000, 0x10000}, {0x23BC1C000, 0x40000},
        {0x23B280000, 0x40000},  {0x23D100000, 0x80000}, {0x23B204000, 0x10000}, {0x26B8EC000, 0x8000},
        {0x26B8F4000, 0x10000},  {0x262000000, 0xD000},  {0x264000000, 0xD000},  {0x266000000, 0xD000},
        {0x268000000, 0xD000},   {0x26A000000, 0xD000},  {0x22A000000, 0xD000},  {0x231000000, 0xD000},
        {0x205000000, 0xD000},   {0x60C000000, 0xD000},  {0x234000000, 0xD000},  {0x279000000, 0xD000},
        {0x225000000, 0x15000},  {0x226000000, 0x15000},
    };
    /* 0: ASCWrap, 1: PMS CSR */
    static const hwaddr pmp_regs[][2] = {
        {0x23B850000, 0x200000}, {0x23B500000, 0x60000}, {0x204D14000, 0x4000}, {0x210E20000, 0x50000},
        {0x211E20000, 0x50000},  {0x26B854000, 0x4000},  {0x23500C000, 0x4000},
    };
    MemoryRegion* mem = g_new(MemoryRegion, ARRAY_SIZE(pmgr_regs) + ARRAY_SIZE(pmp_regs) + 2);
    uint32_t      idx = 0;

    MemoryRegion* pmgr_reg = &mem[idx++];
    hwaddr        mclk_off = T8030_MCA_MCLK_CFG_BASE - pmgr_regs[0][0];
    hwaddr        hi_off   = mclk_off + T8030_MCA_MCLK_CFG_SIZE;

    memory_region_init_io(pmgr_reg, OBJECT(t8030), &pmgr_reg_ops, t8030, "pmgr-reg", pmgr_regs[0][1]);
    memory_region_enable_lockless_io(pmgr_reg);
    memory_region_init_alias(&mem[idx], OBJECT(t8030), "pmgr-reg-lo", pmgr_reg, 0, mclk_off);
    memory_region_add_subregion(get_system_memory(), pmgr_regs[0][0], &mem[idx++]);
    memory_region_init_alias(&mem[idx], OBJECT(t8030), "pmgr-reg-hi", pmgr_reg, hi_off, pmgr_regs[0][1] - hi_off);
    memory_region_add_subregion(get_system_memory(), pmgr_regs[0][0] + hi_off, &mem[idx++]);
    for (size_t i = 1; i < ARRAY_SIZE(pmgr_regs); i++) {
        snprintf(name, sizeof(name), "pmgr-unk-reg-%zu", i - 1);
        t8030_pmgr_init_mmio(t8030, &mem[idx++], &pmgr_unk_reg_ops, NULL, name, pmgr_regs[i][0], pmgr_regs[i][1]);
    }

    t8030_pmgr_init_mmio(t8030, &mem[idx++], &pmgr_unk_reg_ops, NULL, "pmp-reg", pmp_regs[0][0], pmp_regs[0][1]);
    for (size_t i = 1; i < ARRAY_SIZE(pmp_regs); i++) {
        snprintf(name, sizeof(name), "pmp-unk-reg-%zu", i - 1);
        t8030_pmgr_init_mmio(t8030, &mem[idx++], &pmgr_unk_reg_ops, NULL, name, pmp_regs[i][0], pmp_regs[i][1]);
    }
    /* FIXME: Each "Temp Sensor" actually houses multiple, each with different offsets and IRQs */
    create_unimplemented_device_with_val("PMGR SOC Die Temp Sensor", 0x23B2C4000, 0x34000, 0xAFAFAFAFAFAFAFAF);

    dart = APPLE_DART(qdev_new(TYPE_APPLE_DART));
    qdev_prop_set_uint32(DEVICE(dart), "page-size", 0x4000);
    qdev_prop_set_uint64(DEVICE(dart), "sid-mask", 0x8001);
    qdev_prop_set_uint32(DEVICE(dart), "num-instances", 1);
    qdev_prop_set_uint32(DEVICE(dart), "dart-options", 0xD);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
    apple_dart_set_dart_instance(dart, 0, 0x23B300000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dart), 0, t8030_irq(t8030, 0x16A));
    pmp_dart = dart;
    address_space_init(&pmp_dma_as, MEMORY_REGION(apple_dart_iommu_mr(pmp_dart, 0)), "pmp.dma");

    AppleRTKit* pmp_rtk             = APPLE_RTKIT(qdev_new(TYPE_APPLE_RTKIT));
    APPLE_A7IOP(pmp_rtk)->role      = "PMP";
    APPLE_A7IOP(pmp_rtk)->mmio_size = 0x60000;
    pmp_rtk->ops                    = &pmp_rtk_ops;
    apple_rtkit_register_user_ep(pmp_rtk, 0, pmp_rtk, pmp_rtk_ep0_handler);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pmp_rtk), &error_fatal);
    apple_a7iop_connect_irqs(APPLE_A7IOP(pmp_rtk), DEVICE(t8030->aic), 0x166);
    sysbus_mmio_map(SYS_BUS_DEVICE(pmp_rtk), 0, 0x23BC00000);

    memory_region_init_io(&t8030->amcc, OBJECT(t8030), &amcc_reg_ops, t8030, "amcc", AMCC_SIZE);
    memory_region_add_subregion(get_system_memory(), AMCC_BASE, &t8030->amcc);
    create_unimplemented_device("amcc_dcs", AMCC_BASE + 0x200000, 0x100000);

    gpio = apple_gpio_create(0x23C100000, 0xD4, 7, DEVICE(t8030->aic), 0x83);
    qemu_set_irq(qdev_get_gpio_in(gpio, GPIO_FORCE_DFU), t8030->force_dfu);    // FIXME: ???
    apple_gpio_create(0x23E820000, 6, 7, DEVICE(t8030->aic), 0x105);           // SMC
    aop_gpio = apple_gpio_create(0x24A820000, 0x31, 7, DEVICE(t8030->aic), 0xC4);

    // The NUB GPIO's interrupt groups are 0xE9-0xEB, then 0xE2-0xE5.
    dev = qdev_new(TYPE_APPLE_GPIO);
    qdev_prop_set_uint32(dev, "pin-count", 15);
    qdev_prop_set_uint32(dev, "irq-group-count", 7);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x23D1F0000);
    for (i = 0; i < 7; ++i) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, t8030_irq(t8030, i < 3 ? 0xE9 + i : 0xE2 + (i - 3)));
    }

    i2c = apple_i2c_create(0x235010000, t8030_irq(t8030, 0x1C9));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_LM_BACKLIGHT, 0x62);

    i2c = apple_i2c_create(0x235014000, t8030_irq(t8030, 0x1CA));
    // TODO: Revert product id to 3 and vendor id to 2 once audio support is
    // implemented.
    i2cs = i2c_slave_new(TYPE_APPLE_MIC_TEMP_SENSOR, 0x56);
    qdev_prop_set_uint8(DEVICE(i2cs), "product-id", 1);
    qdev_prop_set_uint8(DEVICE(i2cs), "vendor-id", 0);
    qdev_prop_set_uint8(DEVICE(i2cs), "revision", 0);
    qdev_prop_set_uint8(DEVICE(i2cs), "fab-id", 5);
    i2c_slave_realize_and_unref(i2cs, i2c->bus, &error_fatal);

    i2c = apple_i2c_create(0x235018000, t8030_irq(t8030, 0x1CB));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_CHESTNUT, 0x27);
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_CS35L27, 0x40);
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_LM_BACKLIGHT, 0x62);

    i2c = apple_i2c_create(0x23501C000, t8030_irq(t8030, 0x1CC));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_ROSWELL, 0x10);

    apple_i2c_create(0x23ED04000, t8030_irq(t8030, 0x100));

    i2c = apple_i2c_create(0x23ED08000, t8030_irq(t8030, 0x101));
    i2c_slave_create_simple(i2c->bus, TYPE_FAN53740, 0x52);

    dart = apple_dart_create(0x4000, 0xF, 2, 0x5, t8030_irq(t8030, 0x1F5));
    apple_dart_set_dart_instance(dart, 0, 0x239024000);    // LLT
    apple_dart_set_dart_instance(dart, 1, 0x239028000);    // BLK

    dev  = qdev_new(TYPE_APPLE_TYPEC);
    host = usb_uplink_new(t8030->usb_uplink_type, t8030->usb_uplink_addr, t8030->usb_uplink_compression_limit,
                          &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(host), &error_fatal);
    object_property_set_link(OBJECT(dev), "host", OBJECT(host), &error_fatal);
    iommu = apple_dart_instance_iommu_mr(dart, 1, 0x3);
    object_property_add_const_link(OBJECT(dev), "dma-xhci", OBJECT(iommu));
    object_property_add_const_link(OBJECT(dev), "dma-drd", OBJECT(iommu));
    object_property_add_const_link(OBJECT(dev), "dma-otg", OBJECT(apple_dart_instance_iommu_mr(dart, 1, 0x0)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x239000000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, t8030_irq(t8030, 0x1F1));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, t8030_irq(t8030, 0x1F2));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 2, t8030_irq(t8030, 0x1F3));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 3, t8030_irq(t8030, 0x1F4));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 4, t8030_irq(t8030, DWC2_IRQ));

    dfu = USB_DEVICE(qdev_new(TYPE_APPLE_DFU));
    qdev_prop_set_uint32(DEVICE(dfu), "cpid", 0x8030);
    qdev_prop_set_uint32(DEVICE(dfu), "cprv", t8030->chip_revision);
    qdev_prop_set_uint32(DEVICE(dfu), "bdid", t8030->board_id);
    qdev_prop_set_uint64(DEVICE(dfu), "ecid", t8030->ecid);
    usb_realize_and_unref(dfu, USB_BUS(QLIST_FIRST(&host->child_bus)), &error_fatal);
    t8030->dfu = APPLE_DFU(dfu);

    dart = APPLE_DART(qdev_new(TYPE_APPLE_DART));
    object_property_add_child(OBJECT(t8030), "dart-sep", OBJECT(dart));
    qdev_prop_set_uint32(DEVICE(dart), "page-size", 0x4000);
    qdev_prop_set_uint64(DEVICE(dart), "sid-mask", 0x8001);
    qdev_prop_set_uint32(DEVICE(dart), "num-instances", 1);
    qdev_prop_set_uint32(DEVICE(dart), "dart-options", 0x5);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
    apple_dart_set_dart_instance(dart, 0, 0x2412C0000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dart), 0, t8030_irq(t8030, 0xBA));

    if (t8030->sep_rom_filename == NULL) {
#ifdef ENABLE_DATA_ENCRYPTION
        error_setg(&error_fatal, "Simulated SEP cannot be used with data "
                                 "encryption at the moment.");
        return;
#else
        AppleSEPSim* sep_sim = APPLE_SEP_SIM(qdev_new(TYPE_APPLE_SEP_SIM));
        object_property_add_child(OBJECT(t8030), "sep-sim", OBJECT(sep_sim));
        sep_sim->dma_mr = MEMORY_REGION(apple_dart_iommu_mr(dart, 0));
        object_property_add_const_link(OBJECT(sep_sim), "dma-mr", OBJECT(sep_sim->dma_mr));
        sep_sim->dma_as = g_new0(AddressSpace, 1);
        address_space_init(sep_sim->dma_as, sep_sim->dma_mr, "sep.dma");

        sbd = SYS_BUS_DEVICE(sep_sim);
        sysbus_realize_and_unref(sbd, &error_fatal);
#endif
    }
    else {
        sep = APPLE_SEP(qdev_new(TYPE_APPLE_SEP));
        object_property_add_child(OBJECT(t8030), "sep", OBJECT(sep));
        object_property_set_link(OBJECT(sep), "ool-mr", OBJECT(apple_dart_iommu_mr(dart, 0)), &error_fatal);
        qdev_prop_set_uint64(DEVICE(sep), "base", SEPROM_BASE);
        qdev_prop_set_uint32(DEVICE(sep), "cpu-id", A13_MAX_CPU);
        qdev_prop_set_uint32(DEVICE(sep), "chip-id", 0x8030);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(sep), &error_fatal);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_PMGR, 0x241000000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_TRNG_REGS, 0x241180000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_KEY, 0x2411C0000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_KEY_FCFG, 0x241440000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MONI, 0x2413C0000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MONI_THRM, 0x241400000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_EISP, 0x240800000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_EISP_HMAC, 0x240AA0000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_AESS, 0x241040000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_AESH, 0x241080000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_PKA, 0x241100000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_PKA_TMM, 0x241504000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MISC0, 0x242140000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MISC1, 0x241240000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MISC2, 0x2410C4000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_PROGRESS, 0x241280000);
        apple_sep_map_mmio(sep, SEP_MMIO_INDEX_BOOT_MONITOR, 0x241500000);

        sbd = SYS_BUS_DEVICE(sep);
    }

    sysbus_mmio_map(sbd, 0, 0x242400000);
    apple_a7iop_connect_irqs(APPLE_A7IOP(sbd), DEVICE(t8030->aic), 0xB4);

    dart = APPLE_DART(qdev_new(TYPE_APPLE_DART));
    object_property_add_child(OBJECT(t8030), "dart-apcie2", OBJECT(dart));
    qdev_prop_set_uint32(DEVICE(dart), "page-size", 0x1000);
    qdev_prop_set_uint64(DEVICE(dart), "sid-mask", 0xFFFF);
    qdev_prop_set_uint32(DEVICE(dart), "num-instances", 1);
    qdev_prop_set_uint32(DEVICE(dart), "dart-options", 0x17);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
    apple_dart_set_dart_instance(dart, 0, 0x603008000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dart), 0, t8030_irq(t8030, 0x21B));

    dart = APPLE_DART(qdev_new(TYPE_APPLE_DART));
    object_property_add_child(OBJECT(t8030), "dart-apcie3", OBJECT(dart));
    qdev_prop_set_uint32(DEVICE(dart), "page-size", 0x1000);
    qdev_prop_set_uint64(DEVICE(dart), "sid-mask", 0xFFFF);
    qdev_prop_set_uint32(DEVICE(dart), "num-instances", 1);
    qdev_prop_set_uint32(DEVICE(dart), "dart-options", 0x17);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
    apple_dart_set_dart_instance(dart, 0, 0x604008000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dart), 0, t8030_irq(t8030, 0x21E));

    dart = APPLE_DART(qdev_new(TYPE_APPLE_DART));
    object_property_add_child(OBJECT(t8030), "dart-rsm", OBJECT(dart));
    qdev_prop_set_uint32(DEVICE(dart), "page-size", 0x4000);
    qdev_prop_set_uint64(DEVICE(dart), "sid-mask", 0x1);
    qdev_prop_set_uint32(DEVICE(dart), "num-instances", 1);
    qdev_prop_set_uint32(DEVICE(dart), "dart-options", 0x5);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
    apple_dart_set_dart_instance(dart, 0, 0x23D328000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dart), 0, t8030_irq(t8030, 0xA0));

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_PCIE));
    object_property_add_child(OBJECT(t8030), "apcie", OBJECT(sbd));
    qdev_prop_set_uint32(DEVICE(sbd), "chip-id", 0x8030);
    qdev_prop_set_uint32(DEVICE(sbd), "msi-vector-offset", 0x220);
    qdev_prop_set_uint32(DEVICE(sbd), "port2-link-speed", 2);
    qdev_prop_set_bit(DEVICE(sbd), "port2-manual-enable", true);
    object_property_set_link(
        OBJECT(sbd), "port2-dma-mr",
        OBJECT(apple_dart_iommu_mr(APPLE_DART(object_resolve_path_component(OBJECT(t8030), "dart-apcie2")), 1)),
        &error_fatal);
    qdev_prop_set_uint32(DEVICE(sbd), "port3-link-speed", 2);
    qdev_prop_set_bit(DEVICE(sbd), "port3-manual-enable", true);
    object_property_set_link(
        OBJECT(sbd), "port3-dma-mr",
        OBJECT(apple_dart_iommu_mr(APPLE_DART(object_resolve_path_component(OBJECT(t8030), "dart-apcie3")), 1)),
        &error_fatal);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x610000000);
    sysbus_mmio_map(sbd, 1, 0x600000000);
    sysbus_mmio_map(sbd, 2, 0x600080000);
    sysbus_mmio_map(sbd, 3, 0x6000C0000);
    sysbus_mmio_map(sbd, 4, 0x60C000000);
    // The ports come last, as they take precedence over the root PHY they overlap.
    for (i = 0; i < APCIE_MAX_PORTS; ++i) {
        sysbus_mmio_map(sbd, 5 + i * 4, 0x601000000 + i * 0x1000000);
        sysbus_mmio_map(sbd, 6 + i * 4, 0x601010000 + i * 0x1000000);
        sysbus_mmio_map(sbd, 7 + i * 4, 0x600088000 + i * 0x4000);
        sysbus_mmio_map(sbd, 8 + i * 4, 0x6000C4000 + i * 0x4000);
    }
    for (i = 0; i < 4; ++i) { sysbus_connect_irq(sbd, i, t8030_irq(t8030, 0x214 + i * 3)); }
    for (i = 0; i < 32; ++i) { sysbus_connect_irq(sbd, 4 + i, t8030_irq(t8030, 0x220 + i)); }

    sart = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_SART));
    object_property_add_child(OBJECT(t8030), "sart-ans", OBJECT(sart));
    sysbus_realize_and_unref(sart, &error_fatal);
    sysbus_mmio_map(sart, 0, 0x27AC50000);

    // NVMe sits on bridge0, which the T8030 device tree does not have.
    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_ANS));
    object_property_add_child(OBJECT(t8030), "ans", OBJECT(sbd));
    object_property_add_const_link(OBJECT(sbd), "dma-mr", OBJECT(sysbus_mmio_get_region(sart, 1)));
    object_property_set_link(
        OBJECT(sbd), "pci-bus",
        OBJECT(pci_bridge_get_sec_bus(PCI_BRIDGE(object_resolve_path_at(OBJECT(t8030), "apcie/bridge0")))),
        &error_fatal);
    host = DEVICE(object_resolve_path_at(OBJECT(t8030), "apcie/host"));
    qdev_connect_gpio_out_named(host, "interrupt_pci", 0, qdev_get_gpio_in_named(DEVICE(sbd), "interrupt_pci", 0));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x277400000);
    sysbus_mmio_map(sbd, 1, 0x277050000);
    sysbus_mmio_map(sbd, 2, 0x27AD20044);
    sysbus_mmio_map(sbd, 3, 0x27ACC0000);
    sysbus_connect_irq(sbd, 0, t8030_irq(t8030, 0x1A1));
    sysbus_connect_irq(sbd, 1, t8030_irq(t8030, 0x1A0));
    sysbus_connect_irq(sbd, 2, t8030_irq(t8030, 0x1A3));
    sysbus_connect_irq(sbd, 3, t8030_irq(t8030, 0x1A2));
    sysbus_connect_irq(sbd, 4, t8030_irq(t8030, 0x1A6));

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_WDT));
    qdev_prop_set_uint64(DEVICE(sbd), "cntfrq", ARM_CPU(t8030->cpus[0])->gt_cntfrq_hz);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x23D2B0000);
    sysbus_mmio_map(sbd, 1, 0x23D2B8020);
    sysbus_connect_irq(sbd, 0, t8030_irq(t8030, 0xED));
    sysbus_connect_irq(sbd, 1, t8030_irq(t8030, 0x0));

    spmi = t8030_create_spmi(t8030, "spmi0", 0x23D180000, 0xEF);
    dev  = qdev_new(TYPE_APPLE_SPMI_PMU);
    spmi_set_slave_sid(SPMI_SLAVE(dev), 0xF);
    qdev_prop_set_uint32(dev, "reg-rtc", 0x1102);
    qdev_prop_set_uint32(dev, "reg-alarm", 0x1108);
    qdev_prop_set_uint32(dev, "reg-alarm-ctrl", 0x1100);
    qdev_prop_set_uint32(dev, "reg-alarm-event", 0x110C);
    qdev_prop_set_uint32(dev, "reg-rtc-irq-mask", 0x110D);
    qdev_prop_set_uint32(dev, "reg-leg-scrpad", 0x9F00);
    spmi_slave_realize_and_unref(SPMI_SLAVE(dev), spmi->bus, &error_fatal);
    qdev_connect_gpio_out(dev, 0, qdev_get_gpio_in(DEVICE(spmi), 0x2));

    spmi = t8030_create_spmi(t8030, "spmi1", 0x23D184000, 0xF4);
#ifdef ENABLE_BASEBAND
    t8030_create_baseband_spmi(t8030, spmi);
#endif
    t8030_create_spmi(t8030, "spmi2", 0x23D188000, 0xF9);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_SMC_IOP));
    object_property_add_child(OBJECT(t8030), "smc", OBJECT(sbd));
    qdev_prop_set_uint32(DEVICE(sbd), "sram-size", 0x80000);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x23E400000);
    sysbus_mmio_map(sbd, 1, 0x23E050000);
    sysbus_mmio_map(sbd, 2, 0x23FE00000);
    apple_a7iop_connect_irqs(APPLE_A7IOP(sbd), DEVICE(t8030->aic), 0x10E);

#ifdef ENABLE_BASEBAND
    t8030_create_baseband(t8030, gpio);
#endif

    dart = apple_dart_create(0x4000, 0x8003, 1, 0xD, t8030_irq(t8030, 0x1E2));
    apple_dart_set_dart_instance(dart, 0, 0x235004000);

    sio = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_SIO));
    object_property_add_child(OBJECT(t8030), "sio", OBJECT(sio));
    qdev_prop_set_uint64(DEVICE(sio), "gtimer-freq", 24000000);
    object_property_add_const_link(OBJECT(sio), "dma-mr", OBJECT(apple_dart_iommu_mr(dart, 0)));
    sysbus_realize_and_unref(sio, &error_fatal);
    sysbus_mmio_map(sio, 0, 0x236400000);
    sysbus_mmio_map(sio, 1, 0x236050000);
    apple_a7iop_connect_irqs(APPLE_A7IOP(sio), DEVICE(t8030->aic), 0x1E4);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_AES));
    object_property_add_const_link(OBJECT(sbd), "dma-mr", OBJECT(apple_dart_iommu_mr(dart, 1)));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x235008000);
    sysbus_connect_irq(sbd, 0, t8030_irq(t8030, 0x1C7));

    spi             = apple_spi_create(0x235100000, t8030_irq(t8030, 0x1BC), APPLE_SIO(sio), 0x14, 0x15);    // SPI0
    DEVICE(spi)->id = g_strdup("spi0");    // Needed for SecureROM
    qdev_connect_gpio_out(gpio, 0xCC, qdev_get_gpio_in_named(DEVICE(spi), SSI_GPIO_CS, 0));

    spi = apple_spi_create(0x235104000, t8030_irq(t8030, 0x1BD), APPLE_SIO(sio), 0x16, 0x17);    // SPI1
    qdev_connect_gpio_out(gpio, 0x5F, qdev_get_gpio_in_named(DEVICE(spi), SSI_GPIO_CS, 0));
    dev = qdev_new(TYPE_APPLE_MT_SPI);
    qdev_prop_set_uint32(dev, "display_width", t8030->disp_width);
    qdev_prop_set_uint32(dev, "display_height", t8030->disp_height);
    ssi_realize_and_unref(dev, apple_spi_get_bus(spi), &error_fatal);
    qdev_connect_gpio_out_named(dev, APPLE_MT_SPI_IRQ, 0, qdev_get_gpio_in(aop_gpio, 0x22));

#if 0
    spi = apple_spi_create(0x235108000, t8030_irq(t8030, 0x1BE), APPLE_SIO(sio), 0x18, 0x19);    // SPI2
    qdev_connect_gpio_out(gpio, ??, qdev_get_gpio_in_named(DEVICE(spi), SSI_GPIO_CS, 0));
#endif

    spi = apple_spi_create(0x23510C000, t8030_irq(t8030, 0x1BF), APPLE_SIO(sio), 0x1a, 0x1b);    // SPI3
    qdev_connect_gpio_out(gpio, 0x42, qdev_get_gpio_in_named(DEVICE(spi), SSI_GPIO_CS, 0));
    ssi_create_peripheral(apple_spi_get_bus(spi), TYPE_APPLE_CS42L77);

#if 0
    spi = apple_spi_create(0x235110000, t8030_irq(t8030, 0x1C0), APPLE_SIO(sio), 0x1c, 0x1d);    // SPI4
    qdev_connect_gpio_out(gpio, ??, qdev_get_gpio_in_named(DEVICE(spi), SSI_GPIO_CS, 0));
#endif

    dart = APPLE_DART(qdev_new(TYPE_APPLE_DART));
    object_property_add_child(OBJECT(t8030), "dart-disp0", OBJECT(dart));
    qdev_prop_set_uint32(DEVICE(dart), "page-size", 0x4000);
    qdev_prop_set_uint64(DEVICE(dart), "sid-mask", 0x8013);
    qdev_prop_set_uint32(DEVICE(dart), "num-instances", 2);
    qdev_prop_set_uint32(DEVICE(dart), "dart-options", 0x5);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
    apple_dart_set_dart_instance(dart, 0, 0x230704000);
    apple_dart_set_other_instance(dart, 1, DART_SMMU, 0x230700000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dart), 0, t8030_irq(t8030, 0x13F));

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_DISPLAY_PIPE_V4));
    object_property_add_child(OBJECT(t8030), "disp0", OBJECT(sbd));
    object_property_add_const_link(OBJECT(sbd), "dma-mr", OBJECT(apple_dart_iommu_mr(dart, 0)));
    object_property_add_const_link(OBJECT(sbd), "piodma-dma-mr", OBJECT(apple_dart_iommu_mr(dart, 4)));
    qdev_prop_set_uint32(DEVICE(sbd), "width", t8030->disp_width);
    qdev_prop_set_uint32(DEVICE(sbd), "height", t8030->disp_height);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x230000000);
    sysbus_connect_irq(sbd, 0, t8030_irq(t8030, 0x13D));
    sysbus_connect_irq(sbd, 1, t8030_irq(t8030, 0x143));
    sysbus_connect_irq(sbd, 2, t8030_irq(t8030, 0x13A));
    sysbus_connect_irq(sbd, 3, t8030_irq(t8030, 0x13B));
    sysbus_connect_irq(sbd, 4, t8030_irq(t8030, 0x13C));
    sysbus_connect_irq(sbd, 5, t8030_irq(t8030, 0x13E));
    sysbus_connect_irq(sbd, 6, t8030_irq(t8030, 0x139));
    sysbus_connect_irq(sbd, 7, t8030_irq(t8030, 0x144));
    sysbus_connect_irq(sbd, 8, t8030_irq(t8030, 0x145));
    create_unimplemented_device("disp0-ipawrap", 0x230708000, 0x1000);

    dart = apple_dart_create(0x4000, 0x8001, 1, 0x5, t8030_irq(t8030, 0xE0));
    apple_dart_set_dart_instance(dart, 0, 0x24AC00000);

    aop = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_AOP));
    object_property_add_const_link(OBJECT(aop), "dma-mr", OBJECT(apple_dart_iommu_mr(dart, 0)));

    // sbd = apple_aop_audio_create(APPLE_AOP(aop));
    // assert_nonnull(sbd);
    // object_property_add_child(OBJECT(aop), "aop-audio", OBJECT(sbd));
    // sysbus_realize_and_unref(sbd, &error_fatal);

    sysbus_realize_and_unref(aop, &error_fatal);

    sysbus_mmio_map(aop, 0, 0x24A400000);
    sysbus_mmio_map(aop, 1, 0x24A050000);

    apple_a7iop_connect_irqs(APPLE_A7IOP(aop), DEVICE(t8030->aic), 0xD1);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_MCA));
    object_property_add_child(OBJECT(t8030), "mca", OBJECT(sbd));
    object_property_add_const_link(OBJECT(sbd), "sio", OBJECT(sio));
    qdev_prop_set_uint32(DEVICE(sbd), "tx-chan-id", 0x7A);
    qdev_prop_set_uint32(DEVICE(sbd), "rx-chan-id", 0x7B);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, APPLE_MCA_MMIO_SIO, 0x238400000);
    sysbus_mmio_map(sbd, APPLE_MCA_MMIO_DMA, 0x238300000);
    sysbus_mmio_map(sbd, APPLE_MCA_MMIO_MCLK_CFG, T8030_MCA_MCLK_CFG_BASE);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_BUTTONS));
    object_property_set_link(OBJECT(sbd), "smc", object_property_get_link(OBJECT(t8030), "smc", &error_fatal),
                             &error_fatal);
    sysbus_realize_and_unref(sbd, &error_fatal);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_SYNOPSYS_MIPI_DSIM));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x230400000);
    sysbus_mmio_map(sbd, 1, 0x2304C0000);
    sysbus_connect_irq(sbd, 0, t8030_irq(t8030, 0x142));
    sysbus_connect_irq(sbd, 1, t8030_irq(t8030, 0x148));

    dart = apple_dart_create(0x4000, 0x1, 1, 0x5, t8030_irq(t8030, 0x1AE));
    apple_dart_set_dart_instance(dart, 0, 0x263200000);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_SCALER));
    object_property_add_const_link(OBJECT(sbd), "dma-mr", OBJECT(apple_dart_iommu_mr(dart, 0)));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x263000000);
    sysbus_mmio_map(sbd, 1, 0x263210000);
    sysbus_connect_irq(sbd, 0, t8030_irq(t8030, 0x1AD));
    sysbus_connect_irq(sbd, 1, t8030_irq(t8030, 0x1AF));

    qemu_register_wakeup_support();
}

static ram_addr_t t8030_fixup_ram_size(ram_addr_t size)
{
    ram_addr_t ret = ROUND_UP_16K(size);
    if (ret > 4 * GiB) {
        error_setg(&error_fatal, "Specified RAM size exceeds supported SoC maximum (4 GiB)");
        assert_not_reached();
    }
    return ret;
}

static void t8030_set_boot_mode(Object* obj, const char* value, Error** errp)
{
    AppleBootMode boot_mode;

    if (g_str_equal(value, "auto")) { boot_mode = kAppleBootModeAuto; }
    else if (g_str_equal(value, "exit_recovery")) {
        boot_mode = kAppleBootModeExitRecovery;
    }
    else if (g_str_equal(value, "dfu")) {
        boot_mode = kAppleBootModeDFU;
    }
    else {
        error_setg(errp, "Invalid boot mode: %s", value);
        return;
    }

    APPLE_T8030(obj)->boot_info.boot_mode = boot_mode;
}

static char* t8030_get_boot_mode(Object* obj, Error** errp)
{
    switch (APPLE_T8030(obj)->boot_info.boot_mode) {
        case kAppleBootModeExitRecovery: return g_strdup("exit_recovery");
        case kAppleBootModeDFU         : return g_strdup("dfu");
        default                        : return g_strdup("auto");
    }
}

PROP_VISIT_GETTER_SETTER(uint64, ecid);
PROP_GETTER_SETTER(bool, kaslr_off);
PROP_GETTER_SETTER(bool, force_dfu);
PROP_STR_GETTER_SETTER(sep_rom_filename);
PROP_STR_GETTER_SETTER(securerom_filename);
PROP_STR_GETTER_SETTER(usb_uplink_addr);
PROP_GETTER_SETTER(int, usb_uplink_type);
PROP_VISIT_GETTER_SETTER(uint32, usb_uplink_compression_limit);
PROP_STR_GETTER_SETTER(model_number);
PROP_STR_GETTER_SETTER(region_info);
PROP_STR_GETTER_SETTER(config_number);
PROP_STR_GETTER_SETTER(serial_number);
PROP_STR_GETTER_SETTER(mlb_serial_number);
PROP_STR_GETTER_SETTER(regulatory_model);
PROP_VISIT_GETTER_SETTER(uint32, disp_width);
PROP_VISIT_GETTER_SETTER(uint32, disp_height);

static void t8030_class_init(ObjectClass* klass, const void* data)
{
    MachineClass*   mc = MACHINE_CLASS(klass);
    ObjectProperty* oprop;

    mc->desc              = "Apple T8030 SoC (iPhone 11)";
    mc->init              = t8030_init;
    mc->reset             = t8030_reset;
    mc->max_cpus          = A13_MAX_CPU + 1;
    mc->default_cpu_type  = TYPE_APPLE_A13;
    mc->minimum_page_bits = 14;
    mc->default_ram_size  = 4 * GiB;
    mc->fixup_ram_size    = t8030_fixup_ram_size;
    mc->default_ram_id    = "t8030.ram";

    object_class_property_add_str(klass, "sep-rom", t8030_get_sep_rom_filename, t8030_set_sep_rom_filename);
    object_class_property_set_description(klass, "sep-rom", "SEP ROM");
    object_class_property_add_str(klass, "securerom", t8030_get_securerom_filename, t8030_set_securerom_filename);
    object_class_property_set_description(klass, "securerom", "SecureROM");
    oprop = object_class_property_add_str(klass, "boot-mode", t8030_get_boot_mode, t8030_set_boot_mode);
    object_property_set_default_str(oprop, "auto");
    object_class_property_set_description(klass, "boot-mode", "Boot Mode");
    oprop = object_class_property_add(klass, "ecid", "uint64", t8030_get_ecid, t8030_set_ecid, NULL, NULL);
    object_property_set_default_uint(oprop, 0x1122334455667788);
    object_class_property_set_description(klass, "ecid", "Device ECID");
    object_class_property_add_bool(klass, "kaslr-off", t8030_get_kaslr_off, t8030_set_kaslr_off);
    object_class_property_set_description(klass, "kaslr-off", "Disable KASLR");
    object_class_property_add_bool(klass, "force-dfu", t8030_get_force_dfu, t8030_set_force_dfu);
    object_class_property_set_description(klass, "force-dfu", "Force DFU");
    object_class_property_add_str(klass, "usb-uplink-addr", t8030_get_usb_uplink_addr, t8030_set_usb_uplink_addr);
    object_class_property_set_description(klass, "usb-uplink-addr", "USB Uplink Address");
    oprop = object_class_property_add_enum(klass, "usb-uplink-type", "USBUplinkType", &USBUplinkType_lookup,
                                           t8030_get_usb_uplink_type, t8030_set_usb_uplink_type);
    object_property_set_default_str(oprop, qapi_enum_lookup(&USBUplinkType_lookup, USB_UPLINK_TYPE_VIRTUALHERE));
    object_class_property_set_description(klass, "usb-uplink-type", "USB Uplink Type");
    oprop = object_class_property_add(klass, "usb-uplink-compression-limit", "uint32",
                                      t8030_get_usb_uplink_compression_limit, t8030_set_usb_uplink_compression_limit,
                                      NULL, NULL);
    object_property_set_default_uint(oprop, VIRTUALHERE_COMPRESSION_LIMIT_DEFAULT);
    object_class_property_set_description(klass, "usb-uplink-compression-limit",
                                          "VirtualHere messages larger than this are LZ4-compressed");
    oprop = object_class_property_add_str(klass, "model", t8030_get_model_number, t8030_set_model_number);
    object_property_set_default_str(oprop, "CKI12");
    object_class_property_set_description(klass, "model", "Model Number");
    oprop = object_class_property_add_str(klass, "region-info", t8030_get_region_info, t8030_set_region_info);
    object_property_set_default_str(oprop, "LL/A");
    object_class_property_set_description(klass, "region-info", "Region Info");
    oprop = object_class_property_add_str(klass, "config-number", t8030_get_config_number, t8030_set_config_number);
    object_property_set_default_str(oprop, "");
    object_class_property_set_description(klass, "config-number", "Config Number");
    oprop = object_class_property_add_str(klass, "serial-number", t8030_get_serial_number, t8030_set_serial_number);
    object_property_set_default_str(oprop, "INFERNO_1122");
    object_class_property_set_description(klass, "serial-number", "Serial Number");
    oprop = object_class_property_add_str(klass, "mlb", t8030_get_mlb_serial_number, t8030_set_mlb_serial_number);
    object_property_set_default_str(oprop, "INFERNO_MLB1122");
    object_class_property_set_description(klass, "mlb", "MLB Serial Number");
    oprop = object_class_property_add_str(klass, "regulatory-model", t8030_get_regulatory_model,
                                          t8030_set_regulatory_model);
    object_property_set_default_str(oprop, "INFERN8030");
    object_class_property_set_description(klass, "regulatory-model", "Regulatory Model Number");
    oprop = object_class_property_add(klass, "disp-width", "uint32", t8030_get_disp_width, t8030_set_disp_width, NULL,
                                      NULL);
    object_property_set_default_uint(oprop, 828);
    oprop = object_class_property_add(klass, "disp-height", "uint32", t8030_get_disp_height, t8030_set_disp_height,
                                      NULL, NULL);
    object_property_set_default_uint(oprop, 1792);
}

static const TypeInfo t8030_info = {
    .name   = TYPE_APPLE_T8030,
    .parent = TYPE_MACHINE,
    OBJECT_TYPE_INSTANCE(AppleT8030MachineState),
    .class_size = sizeof(AppleT8030MachineClass),
    .class_init = t8030_class_init,
};

static void t8030_types(void) { type_register_static(&t8030_info); }

type_init(t8030_types)
