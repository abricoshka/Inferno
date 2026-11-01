/*
 * Apple S8000 SoC (iPhone 6s Plus).
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
#include "hw/arm/a9.h"
#include "hw/arm/dart.h"
#include "hw/arm/iboot/iboot.h"
#include "hw/arm/lm-backlight.h"
#include "hw/arm/mem.h"
#include "hw/arm/s8000-config.c.inc"
#include "hw/arm/s8000.h"
#include "hw/arm/sep/sim.h"
#include "hw/char/exynos4210_uart.h"
#include "hw/block/nvme_mmu.h"
#include "hw/display/apple_displaypipe_v2.h"
// #include "hw/dma/apple_sio.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/intc/apple_aic.h"
#include "hw/misc/aes.h"
#include "hw/misc/chestnut.h"
#include "hw/misc/unimp.h"
#include "hw/misc/pmu-d2255.h"
#include "hw/pci-host/apcie.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "hw/usb/apple_otg.h"
#include "hw/usb/usb-uplink.h"
#include "hw/watchdog/apple_wdt.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/system.h"

#define PROP_VISIT_GETTER_SETTER(_type, _name)                                                           \
    static void s8000_get_##_name(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) \
    {                                                                                                    \
        _type##_t value;                                                                                 \
                                                                                                         \
        value = APPLE_S8000(obj)->_name;                                                                 \
        visit_type_##_type(v, name, &value, errp);                                                       \
    }                                                                                                    \
                                                                                                         \
    static void s8000_set_##_name(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) \
    { visit_type_##_type(v, name, &APPLE_S8000(obj)->_name, errp); }

#define PROP_STR_GETTER_SETTER(_name)                                                                       \
    static char* s8000_get_##_name(Object* obj, Error** errp) { return g_strdup(APPLE_S8000(obj)->_name); } \
                                                                                                            \
    static void s8000_set_##_name(Object* obj, const char* value, Error** errp)                             \
    {                                                                                                       \
        AppleS8000MachineState* s8000;                                                                      \
                                                                                                            \
        s8000 = APPLE_S8000(obj);                                                                           \
        g_free(s8000->_name);                                                                               \
        s8000->_name = g_strdup(value);                                                                     \
    }

#define PROP_GETTER_SETTER(_type, _name)                                                                       \
    static void s8000_set_##_name(Object* obj, _type value, Error** errp) { APPLE_S8000(obj)->_name = value; } \
                                                                                                               \
    static _type s8000_get_##_name(Object* obj, Error** errp) { return APPLE_S8000(obj)->_name; }

#define PROP_BOOT_STR_GETTER_SETTER(_name)                                                                            \
    static char* s8000_get_##_name(Object* obj, Error** errp) { return g_strdup(APPLE_S8000(obj)->boot_info._name); } \
                                                                                                                      \
    static void s8000_set_##_name(Object* obj, const char* value, Error** errp)                                       \
    {                                                                                                                 \
        AppleBootInfo* info;                                                                                          \
                                                                                                                      \
        info = &APPLE_S8000(obj)->boot_info;                                                                          \
        g_free(info->_name);                                                                                          \
        info->_name = g_strdup(value);                                                                                \
    }

#define PROP_BOOT_GETTER_SETTER(_type, _name)                                                               \
    static void s8000_set_##_name(Object* obj, _type value, Error** errp)                                   \
    { APPLE_S8000(obj)->boot_info._name = value; }                                                          \
                                                                                                            \
    static _type s8000_get_##_name(Object* obj, Error** errp) { return APPLE_S8000(obj)->boot_info._name; }

#define SPI0_IRQ       188
#define GPIO_SPI0_CS   106
#define GPIO_FORCE_DFU 123

#define SPI0_BASE (0x20A080000ULL)

#define SROM_BASE (0x100000000)
#define SROM_SIZE (512 * KiB)

#define DRAM_BASE (0x800000000ULL)
#define DRAM_SIZE (2 * GiB)

#define SRAM_BASE (0x180000000ULL)
#define SRAM_SIZE (0x400000ULL)

#define SEPROM_BASE (0x20D000000ULL)
#define SEPROM_SIZE (0x1000000ULL)

#define NVME_SCRATCH_SIZE (0xC00000ULL)
#define DISPLAY_SIZE      (0x854000ULL)

static const AppleCarveout s8000_carveouts[] = {
    {APPLE_CARVEOUT_NVME_SCRATCH, NVME_SCRATCH_SIZE},
    {APPLE_CARVEOUT_IBOOT_HANDOFF, 0x4000},
    {APPLE_CARVEOUT_UNKNOWN_07, 0x4000},
    {APPLE_CARVEOUT_UNKNOWN_0A, 0x4000},
    {APPLE_CARVEOUT_PRAM, 0x80000},
    {APPLE_CARVEOUT_UNKNOWN_50, 0x18000},
    {APPLE_CARVEOUT_VRAM, DISPLAY_SIZE},
    {APPLE_CARVEOUT_SEPFW, 0x1E00000},
    {APPLE_CARVEOUT_AP_TRUSTZONE, 0x80000},
};

static void s8000_start_cpus(MachineState* machine, uint64_t cpu_mask)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    uint32_t                i;

    for (i = 0; i < machine->smp.cpus; i++) {
        if ((cpu_mask & BIT_ULL(i)) != 0 && apple_a9_cpu_is_off(s8000->cpus[i])) {
            apple_a9_cpu_set_on(s8000->cpus[i]);
        }
    }
}

static void pmgr_unk_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
#if 0
    hwaddr base = (hwaddr)opaque;
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE unk @ 0x" TARGET_FMT_lx
                  " base: 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx "\n",
                  base + addr, base, data);
#endif
}

/* TODO: 0x2102D0000; MiniPMGR Security */
static uint64_t pmgr_unk_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(qdev_get_machine());
    hwaddr                  base  = (hwaddr)opaque;

    uint32_t security_epoch          = 1;    // On IMG4: Security Epoch ; On IMG3: Minimum
                                             // Epoch, verified on SecureROM s5l8955xsi
    bool     current_prod            = true;
    bool     current_secure_mode     = true;    // T8015 SEPOS Kernel also requires this.
    uint32_t security_domain         = 1;
    bool     raw_prod                = true;
    bool     raw_secure_mode         = true;
    uint32_t sep_bit30_current_value = 0;
    bool     fuses_locked            = true;
    uint32_t ret                     = 0x0;

    switch (base + addr) {
        case 0x2102BC000:    // CFG_FUSE0
            //     // handle SEP DSEC demotion
            //     if (sep != NULL && sep->pmgr_fuse_changer_bit1_was_set)
            //         current_secure_mode = 0; // SEP DSEC img4 tag demotion active
            ret |= ((uint32_t)current_prod << 0);
            ret |= ((uint32_t)current_secure_mode << 1);
            ret |= ((security_domain & 3) << 2);
            ret |= ((s8000->board_id & 7) << 4);
            ret |= ((security_epoch & 0x7f) << 9);
            // ret |= (( & ) << );
            return ret;
        case 0x2102BC200:    // CFG_FUSE0_RAW
            ret |= ((uint32_t)raw_prod << 0);
            ret |= ((uint32_t)raw_secure_mode << 1);
            return ret;
        case 0x2102BC080:                             // ECID_LO
            return extract64(s8000->ecid, 0, 32);     // ECID lower
        case 0x2102BC084:                             // ECID_HI
            return extract64(s8000->ecid, 32, 32);    // ECID upper
        case 0x2102E8000:                             // ????
            return 0x4;
        case 0x2102BC104:    // ???? bit 24 => is fresh boot?
            return BIT32(24) | BIT32(25);
        default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "PMGR reg READ unk @ 0x" TARGET_FMT_lx
                      " base: 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx "\n",
                      base + addr, base, 0);
#endif
            break;
    }
    return 0;
}

static const MemoryRegionOps pmgr_unk_reg_ops = {
    .write = pmgr_unk_reg_write,
    .read  = pmgr_unk_reg_read,
};

static void pmgr_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    MachineState*           machine = opaque;
    AppleS8000MachineState* s8000   = opaque;
    uint32_t                value   = (uint32_t)data;

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE @ 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx
                  "\n",
                  addr, data);
#endif

    if (addr >= 0x80000 && addr <= 0x88010) { value = (value & 0xf) << 4 | (value & 0xf); }

    switch (addr) {
        case 0x80400:    // SEP Power State, Manual & Actual: Run Max
            value = 0xFF;
            break;
        case 0xD4004: s8000_start_cpus(machine, data);
        default     : break;
    }
    memcpy(s8000->pmgr_reg + addr, &value, size);
}

static uint64_t pmgr_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleS8000MachineState* s8000  = opaque;
    uint64_t                result = 0;

    memcpy(&result, s8000->pmgr_reg + addr, size);
#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg READ @ 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx
                  "\n",
                  addr, result);
#endif
    return result;
}

static const MemoryRegionOps pmgr_reg_ops = {
    .write = pmgr_reg_write,
    .read  = pmgr_reg_read,
};

static void s8000_update_device_tree(MachineState* machine, AppleDTNode* root)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    AppleDTNode*            child;
    AppleDTNode*            node;
    GList*                  iter;
    GList*                  next;
    uint32_t                i;
    uint64_t                region[2];

    // HACK: Use DEV Hardware model to restore without FDR errors
    apple_dt_set_prop(root, "compatible", 26, "N66DEV\0iPhone8,2\0AppleARM");

    apple_dt_set_prop_u32(root, "clock-frequency", 24000000);
    child = apple_dt_get_node(root, "arm-io");
    assert_nonnull(child);

    apple_dt_set_prop_u32(child, "chip-revision", 0);

    apple_dt_set_prop(child, "clock-frequencies", sizeof(s8000_clock_frequencies), s8000_clock_frequencies);

    apple_dt_set_prop_strn(root, "platform-name", 32, "s8000");
    apple_dt_set_prop_strn(root, "model-number", 32, "MWL72");
    apple_dt_set_prop_strn(root, "region-info", 32, "LL/A");
    apple_dt_set_prop_strn(root, "config-number", 64, "");
    apple_dt_set_prop_strn(root, "serial-number", 32, "C39ZRMDEN72J");
    apple_dt_set_prop_strn(root, "mlb-serial-number", 32, "C39948108J9N72J1F");
    apple_dt_set_prop_strn(root, "regulatory-model-number", 32, "A2111");

    child = apple_dt_get_node(root, "chosen");
    apple_dt_set_prop_u32(child, "chip-id", 0x8000);
    apple_dt_set_prop_u32(child, "board-id", s8000->board_id);

    apple_dt_set_prop_u64(child, "unique-chip-id", s8000->ecid);

    // Update the display parameters
    apple_dt_set_prop_u32(child, "display-rotation", 0);
    apple_dt_set_prop_u32(child, "display-scale", 2);

    child = apple_dt_get_node(root, "product");

    apple_dt_set_prop_u32(child, "oled-display", 1);
    apple_dt_set_prop_str(child, "graphics-featureset-class", "");
    apple_dt_set_prop_str(child, "graphics-featureset-fallbacks", "");
    apple_dt_set_prop_u32(child, "device-color-policy", 0);

    child = apple_dt_get_node(root, "cpus");
    assert_nonnull(child);
    for (iter = child->children, i = 0; iter != NULL; iter = next, i++) {
        next = iter->next;
        node = iter->data;
        if (i >= machine->smp.cpus) {
            apple_dt_del_node(child, node);
            continue;
        }
        apple_dt_del_prop_named(node, "reg-private");
        apple_dt_del_prop_named(node, "cpu-uttdbg-reg");
        if (i == 0) { apple_dt_set_prop_str(node, "state", "running"); }
        apple_dt_set_prop_u64(node, "timebase-frequency", 24000000);
        apple_dt_set_prop_u64(node, "fixed-frequency", 24000000);
        apple_dt_set_prop_u64(node, "peripheral-frequency", 24000000);
        apple_dt_set_prop_u64(node, "memory-frequency", 24000000);
        apple_dt_set_prop_u64(node, "bus-frequency", 24000000);
        apple_dt_set_prop_u64(node, "clock-frequency", 24000000);
        apple_dt_del_prop_named(node, "coresight-reg");
    }

    child = apple_dt_get_node(root, "arm-io/aic");
    assert_nonnull(child);
    apple_dt_set_prop_u32(child, "#main-cpus", machine->smp.cpus);
    apple_dt_set_prop_u32(child, "#shared-timestamps", 0);

    child = apple_dt_get_node(root, "arm-io/pmgr");
    assert_nonnull(child);
    apple_dt_set_prop(child, "voltage-states1", sizeof(s8000_voltage_states1), s8000_voltage_states1);

    child = apple_dt_get_node(root, "arm-io/usb-complex");
    assert_nonnull(child);
    // no-pmu is needed for T8015, and is also necessary for S8000.
    apple_dt_set_prop_u32(child, "no-pmu", 1);

    child = apple_dt_get_node(root, "arm-io/wdt");
    assert_nonnull(child);
    // TODO: MCC
    apple_dt_del_prop_named(child, "function-panic_flush_helper");
    apple_dt_del_prop_named(child, "function-panic_halt_helper");
    apple_dt_set_prop_u32(child, "no-pmu", 1);

    child = apple_dt_get_node(root, "arm-io/apcie");
    assert_nonnull(child);
    // TODO: S8000 needs it, and probably T8030 does need it as well.
    apple_dt_set_prop_null(child, "apcie-phy-tunables");

    // might also work without the sart regions?
    child = apple_dt_get_node(root, "arm-io/nvme-mmu0");
    assert_nonnull(child);
    region[0] = apple_iboot_carveout_base(&s8000->boot_info, APPLE_CARVEOUT_NVME_SCRATCH);
    region[1] = NVME_SCRATCH_SIZE;
    apple_dt_set_prop(child, "sart-region", sizeof(region), region);
    region[0] = apple_dt_get_prop_u32(child, "sart-virtual-base", &error_fatal);
    child     = apple_dt_get_node(root, "arm-io/apcie/pci-bridge0/s3e");
    assert_nonnull(child);
    apple_dt_set_prop(child, "nvme-scratch-virt-region", sizeof(region), region);

    child = apple_dt_get_node(root, "arm-io/disp0");
    assert_nonnull(child);
    apple_dt_set_prop_u32(child, "dot-pitch", 326);
}

static void s8000_cpu_setup(AppleS8000MachineState* s8000)
{
    MachineState* machine = MACHINE(s8000);
    char          name[8];
    uint32_t      i;

    object_initialize_child(OBJECT(s8000), "cluster", &s8000->cluster, TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s8000->cluster), "cluster-id", 0);

    for (i = 0; i < machine->smp.cpus; i++) {
        snprintf(name, sizeof(name), "cpu%u", i);
        s8000->cpus[i] = apple_a9_create(name, i, i);

        object_property_add_child(OBJECT(&s8000->cluster), DEVICE(s8000->cpus[i])->id, OBJECT(s8000->cpus[i]));

        qdev_realize(DEVICE(s8000->cpus[i]), NULL, &error_fatal);
    }
    qdev_realize(DEVICE(&s8000->cluster), NULL, &error_fatal);
}

static qemu_irq s8000_irq(AppleS8000MachineState* s8000, uint32_t irq)
{ return qdev_get_gpio_in(DEVICE(s8000->aic), irq); }

static void s8000_create_aic(AppleS8000MachineState* s8000)
{
    MachineState* machine = MACHINE(s8000);
    uint32_t      i;

    s8000->aic = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_AIC));
    object_property_add_child(OBJECT(s8000), "aic", OBJECT(s8000->aic));
    qdev_prop_set_uint32(DEVICE(s8000->aic), "num-cpu", machine->smp.cpus);
    qdev_prop_set_uint32(DEVICE(s8000->aic), "num-eir", 9);
    sysbus_realize_and_unref(s8000->aic, &error_fatal);

    for (i = 0; i < machine->smp.cpus; i++) {
        memory_region_add_subregion_overlap(&s8000->cpus[i]->memory, 0x20E100000,
                                            sysbus_mmio_get_region(s8000->aic, i * 2), 0);
        // The timebase sits inside the AIC registers here.
        memory_region_add_subregion_overlap(&s8000->cpus[i]->memory, 0x20E101000,
                                            sysbus_mmio_get_region(s8000->aic, i * 2 + 1), 1);
        sysbus_connect_irq(s8000->aic, i, qdev_get_gpio_in(DEVICE(s8000->cpus[i]), ARM_CPU_IRQ));
    }
}

static void s8000_pmgr_setup(AppleS8000MachineState* s8000)
{
    static const hwaddr pmgr_regs[][2] = {
        {0x20E000000, 0x100000}, {0x210200000, 0x100000}, {0x202220000, 0x2000}, {0x210004000, 0x14},
        {0x202240000, 0x10000},  {0x202250000, 0x10000},  {0x207000000, 0x1000}, {0x207800000, 0x9000},
        {0x207A00000, 0x9000},   {0x207C00000, 0x9000},   {0x207E00000, 0x5000}, {0x205B00000, 0x9000},
        {0x206000000, 0x1000},   {0x206100000, 0x9000},   {0x201F00000, 0x5000}, {0x201F80000, 0x1000},
        {0x20CB00000, 0x1000},   {0x600010000, 0x5000},   {0x207E80000, 0x1000}, {0x20F000000, 0x9000},
    };
    char   name[32];
    size_t i;

    for (i = 0; i < ARRAY_SIZE(pmgr_regs); i++) {
        MemoryRegion* mem  = g_new(MemoryRegion, 1);
        hwaddr        addr = pmgr_regs[i][0];
        hwaddr        size = pmgr_regs[i][1];

        if (i == 0) { memory_region_init_io(mem, OBJECT(s8000), &pmgr_reg_ops, s8000, "pmgr-reg", size); }
        else {
            snprintf(name, sizeof(name), "pmgr-unk-reg-%zu", i);
            memory_region_init_io(mem, OBJECT(s8000), &pmgr_unk_reg_ops, (void*)addr, name, size);
        }
        memory_region_add_subregion_overlap(s8000->sys_mem, addr, mem, -1);
    }
}

/* With `smmu_addr` set, instance 1 is an SMMU. */
static AppleDARTState* s8000_create_dart(AppleS8000MachineState* s8000, const char* name, hwaddr addr, hwaddr smmu_addr,
                                         uint32_t irq)
{
    AppleDARTState* dart = APPLE_DART(qdev_new(TYPE_APPLE_DART));

    object_property_add_child(OBJECT(s8000), name, OBJECT(dart));
    qdev_prop_set_uint32(DEVICE(dart), "page-size", 0x1000);
    qdev_prop_set_uint64(DEVICE(dart), "sid-mask", 0xFFFF);
    qdev_prop_set_uint32(DEVICE(dart), "num-instances", smmu_addr == 0 ? 1 : 2);
    qdev_prop_set_uint32(DEVICE(dart), "dart-options", 0);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
    apple_dart_set_dart_instance(dart, 0, addr);
    if (smmu_addr != 0) { apple_dart_set_other_instance(dart, 1, DART_SMMU, smmu_addr); }
    sysbus_connect_irq(SYS_BUS_DEVICE(dart), 0, s8000_irq(s8000, irq));

    return dart;
}

static void s8000_create_spi(AppleS8000MachineState* s8000, const char* name, hwaddr addr, uint32_t irq,
                             DeviceState* gpio, int cs_pin)
{
    DeviceState* spi = qdev_new(TYPE_APPLE_SPI);

    spi->id = g_strdup(name);
    object_property_add_child(OBJECT(s8000), name, OBJECT(spi));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0, addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0, s8000_irq(s8000, irq));
    if (cs_pin >= 0) { qdev_connect_gpio_out(gpio, cs_pin, qdev_get_gpio_in_named(spi, SSI_GPIO_CS, 0)); }
}

static void s8000_create_usb(AppleS8000MachineState* s8000)
{
    DeviceState* otg;
    DeviceState* host;

    otg = qdev_new(TYPE_APPLE_OTG);
    object_property_add_child(OBJECT(s8000), "otg", OBJECT(otg));

    host = usb_uplink_new(s8000->usb_uplink_type, s8000->usb_uplink_addr, s8000->usb_uplink_compression_limit,
                          &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(host), &error_fatal);
    object_property_set_link(OBJECT(otg), "host", OBJECT(host), &error_fatal);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(otg), &error_fatal);

    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 0, 0x20C000030);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 1, 0x20E0D8000);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 2, 0x20C100000);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 3, 0x20C900000);

    sysbus_connect_irq(SYS_BUS_DEVICE(otg), 0, s8000_irq(s8000, 0xD6));
}

static void s8000_create_pcie(AppleS8000MachineState* s8000)
{
    static const char* const darts[] = {"dart-apcie0", "dart-apcie1", "dart-apcie2"};
    SysBusDevice*            pcie;
    char                     name[32];
    uint32_t                 i;

    pcie = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_PCIE));
    object_property_add_child(OBJECT(s8000), "pcie", OBJECT(pcie));
    qdev_prop_set_uint32(DEVICE(pcie), "chip-id", 0x8000);
    qdev_prop_set_uint32(DEVICE(pcie), "msi-vector-offset", 0x100);
    qdev_prop_set_uint32(DEVICE(pcie), "port0-link-speed", 2);
    qdev_prop_set_uint32(DEVICE(pcie), "port1-link-speed", 1);
    qdev_prop_set_bit(DEVICE(pcie), "port1-manual-enable", true);
    qdev_prop_set_uint32(DEVICE(pcie), "port2-link-speed", 1);
    qdev_prop_set_bit(DEVICE(pcie), "port2-manual-enable", true);
    for (i = 0; i < ARRAY_SIZE(darts); i++) {
        snprintf(name, sizeof(name), "port%u-dma-mr", i);
        object_property_set_link(
            OBJECT(pcie), name,
            OBJECT(apple_dart_iommu_mr(APPLE_DART(object_resolve_path_component(OBJECT(s8000), darts[i])), 1)),
            &error_fatal);
    }
    sysbus_realize_and_unref(pcie, &error_fatal);

    sysbus_mmio_map(pcie, 0, 0x610000000);
    sysbus_mmio_map(pcie, 1, 0x600000000);
    sysbus_mmio_map(pcie, 2, 0x600008000);
    for (i = 0; i < APCIE_MAX_PORTS; i++) {
        sysbus_mmio_map(pcie, 3 + i, 0x601000000 + i * 0x1000000);
        snprintf(name, sizeof(name), "port%u_phy", i);
        create_unimplemented_device(name, 0x601004000 + i * 0x1000000, 0x4000);
    }

    for (i = 0; i < 4; i++) { sysbus_connect_irq(pcie, i, s8000_irq(s8000, 0xF4 + i * 3)); }
    for (i = 0; i < 32; i++) { sysbus_connect_irq(pcie, 4 + i, s8000_irq(s8000, 0x100 + i)); }
}

static void s8000_create_nvme(AppleS8000MachineState* s8000)
{
    SysBusDevice* nvme;

    nvme = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_NVME_MMU));
    object_property_add_child(OBJECT(s8000), "nvme", OBJECT(nvme));
    object_property_set_link(OBJECT(nvme), "port", object_resolve_path_at(OBJECT(s8000), "pcie/bridge0"), &error_fatal);
    sysbus_mmio_map(nvme, 0, 0x601004000);
    sysbus_connect_irq(nvme, 0, s8000_irq(s8000, 0xF6));
    sysbus_realize_and_unref(nvme, &error_fatal);
}

static void s8000_display_create(AppleS8000MachineState* s8000)
{
    SysBusDevice*   sbd;
    AppleDARTState* dart;
    uint32_t        i;

    dart = s8000_create_dart(s8000, "dart-disp0", 0x206304000, 0x206300000, 0x8D);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_DISPLAY_PIPE_V2));
    object_property_add_child(OBJECT(s8000), "disp0", OBJECT(sbd));
    object_property_add_const_link(OBJECT(sbd), "dma-mr", OBJECT(apple_dart_iommu_mr(dart, 0)));
    qdev_prop_set_uint64(DEVICE(sbd), "vram-size", DISPLAY_SIZE);
    sysbus_realize_and_unref(sbd, &error_fatal);

    s8000->boot_info.disp_width  = object_property_get_uint(OBJECT(sbd), "width", &error_abort);
    s8000->boot_info.disp_height = object_property_get_uint(OBJECT(sbd), "height", &error_abort);

    sysbus_mmio_map(sbd, 0, 0x206200000);
    sysbus_mmio_map(sbd, 1, 0x206400000);
    sysbus_mmio_map(sbd, 2, 0x206440000);
    sysbus_mmio_map(sbd, 3, 0x206480000);
    sysbus_mmio_map(sbd, 4, 0x2064C0000);
    sysbus_mmio_map(sbd, 5, 0x206500000);
    sysbus_connect_irq(sbd, 0, s8000_irq(s8000, 0x8C));
    for (i = 0; i < 4; i++) { sysbus_connect_irq(sbd, 1 + i, s8000_irq(s8000, 0x8F + i)); }

    adp_v2_update_vram_mapping(APPLE_DISPLAY_PIPE_V2(sbd), s8000->sys_mem,
                               apple_iboot_carveout_base(&s8000->boot_info, APPLE_CARVEOUT_VRAM));
}

static void s8000_create_devices(AppleS8000MachineState* s8000)
{
    DeviceState*   gpio;
    DeviceState*   dev;
    SysBusDevice*  sbd;
    AppleI2CState* i2c0;
    AppleI2CState* i2c2;

    exynos4210_uart_create(0x20A0C0000, 256, 0, serial_hd(0), s8000_irq(s8000, 0xC0));

    s8000_pmgr_setup(s8000);

    s8000_create_dart(s8000, "dart-apcie0", 0x601008000, 0, 0xF5);
    s8000_create_dart(s8000, "dart-apcie1", 0x602008000, 0, 0xF8);
    s8000_create_dart(s8000, "dart-apcie2", 0x603008000, 0, 0xFB);

    gpio = apple_gpio_create(0x20F100000, 0xD0, 7, DEVICE(s8000->aic), 0x2A);
    object_property_add_child(OBJECT(s8000), "gpio", OBJECT(gpio));
    dev = apple_gpio_create(0x2100F0000, 0x1C, 7, DEVICE(s8000->aic), 0x71);
    object_property_add_child(OBJECT(s8000), "aop-gpio", OBJECT(dev));

    i2c0 = apple_i2c_create(0x20A110000, s8000_irq(s8000, 0xCE));
    apple_i2c_create(0x20A111000, s8000_irq(s8000, 0xCF));
    i2c2 = apple_i2c_create(0x20A112000, s8000_irq(s8000, 0xD0));

    s8000_create_usb(s8000);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_WDT));
    object_property_add_child(OBJECT(s8000), "wdt", OBJECT(sbd));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x2102B0000);
    sysbus_mmio_map(sbd, 1, 0x2102B8020);
    sysbus_connect_irq(sbd, 0, s8000_irq(s8000, 0x4));

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_AES));
    object_property_add_child(OBJECT(s8000), "aes", OBJECT(sbd));
    object_property_add_const_link(OBJECT(sbd), "dma-mr", OBJECT(s8000->sys_mem));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x20A108000);
    sysbus_connect_irq(sbd, 0, s8000_irq(s8000, 0xD3));

    s8000_create_spi(s8000, "spi0", SPI0_BASE, SPI0_IRQ, gpio, GPIO_SPI0_CS);
    s8000_create_spi(s8000, "spi1", 0x20A084000, 0xBD, gpio, 0x10);
    s8000_create_spi(s8000, "spi2", 0x20A088000, 0xBE, gpio, 0x2C);
    s8000_create_spi(s8000, "spi3", 0x20A08C000, 0xBF, gpio, -1);

    s8000->sep = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_SEP_SIM));
    object_property_add_child(OBJECT(s8000), "sep", OBJECT(s8000->sep));
    qdev_prop_set_uint32(DEVICE(s8000->sep), "version", APPLE_A7IOP_V2);
    object_property_add_const_link(OBJECT(s8000->sep), "dma-mr", OBJECT(s8000->sys_mem));
    sysbus_realize_and_unref(s8000->sep, &error_fatal);
    sysbus_mmio_map_overlap(s8000->sep, 0, 0x20DA00000, 2);
    apple_a7iop_connect_irqs(APPLE_A7IOP(s8000->sep), DEVICE(s8000->aic), 0x62);

    dev = DEVICE(i2c_slave_create_simple(i2c0->bus, TYPE_PMU_D2255, 0x74));
    qdev_connect_gpio_out(dev, 0, qdev_get_gpio_in(gpio, 0x4D));

    s8000_create_pcie(s8000);
    s8000_create_nvme(s8000);

    i2c_slave_create_simple(i2c0->bus, TYPE_APPLE_CHESTNUT, 0x27);

    s8000_display_create(s8000);

    i2c_slave_create_simple(i2c0->bus, TYPE_APPLE_LM_BACKLIGHT, 0x62);
    i2c_slave_create_simple(i2c2->bus, TYPE_APPLE_LM_BACKLIGHT, 0x62);
}

static void s8000_reset(MachineState* machine, ResetType type)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    DeviceState*            gpio  = NULL;

    qemu_devices_reset(type);

    apple_iboot_reset(&s8000->boot_info);

    gpio = DEVICE(object_property_get_link(OBJECT(s8000), "gpio", &error_fatal));

    qemu_set_irq(qdev_get_gpio_in(gpio, GPIO_FORCE_DFU), s8000->force_dfu);
}

static void s8000_init(MachineState* machine)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);

    s8000->sys_mem = get_system_memory();
    allocate_ram(s8000->sys_mem, "SROM", SROM_BASE, SROM_SIZE, 0);
    allocate_ram(s8000->sys_mem, "SRAM", SRAM_BASE, SRAM_SIZE, 0);
    allocate_ram(s8000->sys_mem, "DRAM", DRAM_BASE, DRAM_SIZE, 0);
    allocate_ram(s8000->sys_mem, "SEPROM", SEPROM_BASE, SEPROM_SIZE, 0);
    MemoryRegion* mr = g_new0(MemoryRegion, 1);
    memory_region_init_alias(mr, OBJECT(s8000), "s8000.seprom.alias", s8000->sys_mem, SEPROM_BASE, SEPROM_SIZE);
    memory_region_add_subregion_overlap(s8000->sys_mem, 0, mr, 1);

    s8000->board_id = 1;    // Match with apple_aes.c

    s8000->boot_info.dram_base          = DRAM_BASE;
    s8000->boot_info.dram_size          = DRAM_SIZE;
    s8000->boot_info.carveouts          = s8000_carveouts;
    s8000->boot_info.num_carveouts      = ARRAY_SIZE(s8000_carveouts);
    s8000->boot_info.srom_base          = SROM_BASE;
    s8000->boot_info.sepfw_size         = 8 * MiB;
    s8000->boot_info.num_cpus           = machine->smp.cpus;
    s8000->boot_info.update_device_tree = s8000_update_device_tree;
    apple_iboot_init(&s8000->boot_info, machine);

    s8000_cpu_setup(s8000);
    s8000_create_aic(s8000);
    s8000_create_devices(s8000);
}

static ram_addr_t s8000_fixup_ram_size(ram_addr_t size)
{
    ram_addr_t ret = ROUND_UP_16K(size);
    if (ret != DRAM_SIZE) { error_setg(&error_fatal, "Specified RAM size must be 2 GiB"); }
    return ret;
}

static void s8000_set_boot_mode(Object* obj, const char* value, Error** errp)
{
    AppleS8000MachineState* s8000;

    s8000 = APPLE_S8000(obj);
    if (g_str_equal(value, "auto")) { s8000->boot_info.boot_mode = kAppleBootModeAuto; }
    else if (g_str_equal(value, "enter_recovery")) {
        s8000->boot_info.boot_mode = kAppleBootModeEnterRecovery;
    }
    else if (g_str_equal(value, "exit_recovery")) {
        s8000->boot_info.boot_mode = kAppleBootModeExitRecovery;
    }
    else {
        s8000->boot_info.boot_mode = kAppleBootModeAuto;
        error_setg(errp, "Invalid boot mode: %s", value);
    }
}

static char* s8000_get_boot_mode(Object* obj, Error** errp)
{
    AppleS8000MachineState* s8000;

    s8000 = APPLE_S8000(obj);
    switch (s8000->boot_info.boot_mode) {
        case kAppleBootModeEnterRecovery: return g_strdup("enter_recovery");
        case kAppleBootModeExitRecovery : return g_strdup("exit_recovery");
        case kAppleBootModeAuto         : return g_strdup("auto");
    }
}

PROP_VISIT_GETTER_SETTER(uint64, ecid);
PROP_BOOT_GETTER_SETTER(bool, kaslr_off);
PROP_BOOT_GETTER_SETTER(int, kernel_patchset);
PROP_GETTER_SETTER(bool, force_dfu);
PROP_BOOT_STR_GETTER_SETTER(trustcache_filename);
PROP_BOOT_STR_GETTER_SETTER(seprom_filename);
PROP_BOOT_STR_GETTER_SETTER(securerom_filename);
PROP_STR_GETTER_SETTER(usb_uplink_addr);
PROP_GETTER_SETTER(int, usb_uplink_type);
PROP_VISIT_GETTER_SETTER(uint32, usb_uplink_compression_limit);

static void s8000_class_init(ObjectClass* klass, const void* data)
{
    MachineClass*   mc;
    ObjectProperty* oprop;

    mc                    = MACHINE_CLASS(klass);
    mc->desc              = "Apple S8000 SoC (iPhone 6s Plus)";
    mc->init              = s8000_init;
    mc->reset             = s8000_reset;
    mc->max_cpus          = A9_MAX_CPU;
    mc->default_cpu_type  = TYPE_APPLE_A9;
    mc->minimum_page_bits = 14;
    mc->default_ram_size  = DRAM_SIZE;
    mc->fixup_ram_size    = s8000_fixup_ram_size;

    object_class_property_add_str(klass, "trustcache", s8000_get_trustcache_filename, s8000_set_trustcache_filename);
    object_class_property_set_description(klass, "trustcache", "TrustCache");
    object_class_property_add_str(klass, "sep-rom", s8000_get_seprom_filename, s8000_set_seprom_filename);
    object_class_property_set_description(klass, "sep-rom", "SEP ROM");
    object_class_property_add_str(klass, "securerom", s8000_get_securerom_filename, s8000_set_securerom_filename);
    object_class_property_set_description(klass, "securerom", "SecureROM");
    object_class_property_add_str(klass, "boot-mode", s8000_get_boot_mode, s8000_set_boot_mode);
    object_class_property_set_description(klass, "boot-mode", "Boot Mode");
    object_class_property_add_bool(klass, "kaslr-off", s8000_get_kaslr_off, s8000_set_kaslr_off);
    object_class_property_set_description(klass, "kaslr-off", "Disable KASLR");
    oprop = object_class_property_add_enum(klass, "kernel-patchset", "KernelPatchset", &KernelPatchset_lookup,
                                           s8000_get_kernel_patchset, s8000_set_kernel_patchset);
    object_property_set_default_str(oprop, qapi_enum_lookup(&KernelPatchset_lookup, KERNEL_PATCHSET_MINIMAL));
    object_class_property_set_description(klass, "kernel-patchset", "Kernel Patchset");
    oprop = object_class_property_add(klass, "ecid", "uint64", s8000_get_ecid, s8000_set_ecid, NULL, NULL);
    object_property_set_default_uint(oprop, 0x1122334455667788);
    object_class_property_set_description(klass, "ecid", "Device ECID");
    object_class_property_add_bool(klass, "force-dfu", s8000_get_force_dfu, s8000_set_force_dfu);
    object_class_property_set_description(klass, "force-dfu", "Force DFU");
    object_class_property_add_str(klass, "usb-uplink-addr", s8000_get_usb_uplink_addr, s8000_set_usb_uplink_addr);
    object_class_property_set_description(klass, "usb-uplink-addr", "USB Uplink Address");
    oprop = object_class_property_add_enum(klass, "usb-uplink-type", "USBUplinkType", &USBUplinkType_lookup,
                                           s8000_get_usb_uplink_type, s8000_set_usb_uplink_type);
    object_property_set_default_str(oprop, qapi_enum_lookup(&USBUplinkType_lookup, USB_UPLINK_TYPE_VIRTUALHERE));
    object_class_property_set_description(klass, "usb-uplink-type", "USB Uplink Type");
    oprop = object_class_property_add(klass, "usb-uplink-compression-limit", "uint32",
                                      s8000_get_usb_uplink_compression_limit, s8000_set_usb_uplink_compression_limit,
                                      NULL, NULL);
    object_property_set_default_uint(oprop, VIRTUALHERE_COMPRESSION_LIMIT_DEFAULT);
    object_class_property_set_description(klass, "usb-uplink-compression-limit",
                                          "VirtualHere messages larger than this are LZ4-compressed");
}

static const TypeInfo s8000_info = {
    .name   = TYPE_APPLE_S8000,
    .parent = TYPE_MACHINE,
    OBJECT_TYPE_INSTANCE(AppleS8000MachineState),
    .class_size = sizeof(AppleS8000MachineClass),
    .class_init = s8000_class_init,
};

static void s8000_types(void) { type_register_static(&s8000_info); }

type_init(s8000_types)
