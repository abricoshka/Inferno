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
#include "hw/arm/boot.h"
#include "hw/arm/dart.h"
#include "hw/arm/kernel_patches.h"
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
#include "hw/nvram/apple_nvram.h"
#include "hw/pci-host/apcie.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "hw/usb/apple_otg.h"
#include "hw/usb/usb-uplink.h"
#include "hw/watchdog/apple_wdt.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/arm/arm-powerctl.h"

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

// Carveout region 0x2 ; this is the first region
#define NVME_SART_BASE (DRAM_BASE + 0x7F400000ULL)
#define NVME_SART_SIZE (0xC00000ULL)

// regions 0x1/0x7/0xa are in-between, each with a size of 0x4000 bytes.

// Carveout region 0xC
#define PANIC_SIZE (0x80000ULL)
#define PANIC_BASE (NVME_SART_BASE - PANIC_SIZE - 0xC000ULL)

// Carveout region 0x50
#define REGION_50_SIZE (0x18000ULL)
#define REGION_50_BASE (PANIC_BASE - REGION_50_SIZE)

// Carveout region 0xE
#define DISPLAY_SIZE (0x854000ULL)
#define DISPLAY_BASE (REGION_50_BASE - DISPLAY_SIZE)

// Carveout region 0x4
#define TZ0_SIZE (0x1E00000ULL)
#define TZ0_BASE (DISPLAY_BASE - TZ0_SIZE)

// Carveout region 0x6
#define TZ1_SIZE (0x80000ULL)
#define TZ1_BASE (TZ0_BASE - TZ1_SIZE)

// Carveout region 0x18
#define KERNEL_REGION_BASE DRAM_BASE
#define KERNEL_REGION_SIZE ((TZ1_BASE + ~KERNEL_REGION_BASE + 0x4000ULL) & -0x4000ULL)

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

static void s8000_patch_kernel(MachoHeader64* header) { ck_patch_kernel(header); }

static bool s8000_check_panic(AppleS8000MachineState* s8000)
{
    AppleEmbeddedPanicHeader* panic_info;
    bool                      ret;

    if (s8000->panic_size == 0) { return false; }

    panic_info = g_malloc0(s8000->panic_size);

    address_space_rw(&address_space_memory, s8000->panic_base, MEMTXATTRS_UNSPECIFIED, panic_info, s8000->panic_size,
                     false);
    address_space_set(&address_space_memory, s8000->panic_base, 0, s8000->panic_size, MEMTXATTRS_UNSPECIFIED);

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

static void get_kaslr_slides(AppleS8000MachineState* s8000, hwaddr* phys_slide_out, vaddr* virt_slide_out)
{
    static const size_t slide_granular      = (1 << 21);
    static const size_t slide_granular_mask = slide_granular - 1;
    static const size_t slide_virt_max      = 0x100 * (2 * 1024 * 1024);

    hwaddr slide_phys;
    vaddr  slide_virt;
    size_t random_value = get_kaslr_random();

    if (s8000->kaslr_off) {
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

static void s8000_load_kernelcache(AppleS8000MachineState* s8000, const char* cmdline)
{
    MachineState*  machine = MACHINE(s8000);
    vaddr          text_base;
    vaddr          kc_base;
    vaddr          kc_end;
    vaddr          apple_dt_va;
    hwaddr         top_of_kernel_data_pa;
    hwaddr         phys_ptr;
    AppleBootInfo* info = &s8000->boot_info;
    hwaddr         prelink_text_base;
    AppleDTNode*   memory_map = apple_dt_get_node(s8000->device_tree, "/chosen/memory-map");
    vaddr          tz1_virt_low;
    vaddr          tz1_virt_high;

    apple_boot_get_kc_bounds(s8000->kernel, &text_base, &kc_base, &kc_end, NULL, NULL);

    info->kern_text_off = text_base - kc_base;

    prelink_text_base = apple_boot_get_segment(s8000->kernel, "__PRELINK_TEXT")->vmaddr;

    get_kaslr_slides(s8000, &g_phys_slide, &g_virt_slide);

    g_phys_base = KERNEL_REGION_BASE;
    g_virt_base = kc_base + (g_virt_slide - g_phys_slide);

    info->trustcache_addr = vtop_static(prelink_text_base + g_virt_slide) - info->trustcache_size;

    address_space_rw(&address_space_memory, info->trustcache_addr, MEMTXATTRS_UNSPECIFIED, s8000->trustcache,
                     info->trustcache_size, true);

    info->kern_entry = apple_boot_load_macho(s8000->kernel, &address_space_memory, memory_map,
                                             g_phys_base + g_phys_slide, g_virt_slide);

    info_report("Kernel virtual base: 0x%016" VADDR_PRIx, g_virt_base);
    info_report("Kernel physical base: 0x" HWADDR_FMT_plx, g_phys_base);
    info_report("Kernel text off: 0x" HWADDR_FMT_plx, info->kern_text_off);
    info_report("Kernel virtual slide: 0x%016" VADDR_PRIx, g_virt_slide);
    info_report("Kernel physical slide: 0x" HWADDR_FMT_plx, g_phys_slide);
    info_report("Kernel entry point: 0x%016" VADDR_PRIx, info->kern_entry);

    phys_ptr = vtop_static(ROUND_UP_16K(kc_end + g_virt_slide));

    // Device tree
    info->device_tree_addr  = phys_ptr;
    apple_dt_va             = ptov_static(info->device_tree_addr);
    phys_ptr               += ROUND_UP_16K(info->device_tree_size);

    // RAM disk
    if (machine->initrd_filename) {
        info->ramdisk_addr = phys_ptr;
        apple_boot_load_ramdisk(machine->initrd_filename, &address_space_memory, info->ramdisk_addr,
                                &info->ramdisk_size);
        info->ramdisk_size  = ROUND_UP_16K(info->ramdisk_size);
        phys_ptr           += info->ramdisk_size;
    }

    phys_ptr           = ROUND_UP_16K(phys_ptr);
    info->sep_fw_addr  = phys_ptr;
    info->sep_fw_size  = 8 * MiB;
    phys_ptr          += info->sep_fw_size;

    // Kernel boot args
    info->kern_boot_args_addr  = phys_ptr;
    info->kern_boot_args_size  = 0x4000;
    phys_ptr                  += info->kern_boot_args_size;

    apple_boot_finalise_dt(s8000->device_tree, &address_space_memory, info);

    top_of_kernel_data_pa = (ROUND_UP_16K(phys_ptr) + 0x3000ull) & ~0x3FFFull;

    info_report("Boot args: [%s]", cmdline);
    apple_boot_setup_bootargs(s8000->build_version, &address_space_memory, info->kern_boot_args_addr, g_virt_base,
                              g_phys_base, KERNEL_REGION_SIZE, top_of_kernel_data_pa, apple_dt_va,
                              info->device_tree_size, &s8000->video_args, cmdline, machine->ram_size);
    g_virt_base = kc_base;

    apple_boot_get_kc_bounds(s8000->secure_monitor, NULL, &tz1_virt_low, &tz1_virt_high, NULL, NULL);
    info_report("TrustZone 1 virtual address low: 0x%016" VADDR_PRIx, tz1_virt_low);
    info_report("TrustZone 1 virtual address high: 0x%016" VADDR_PRIx, tz1_virt_high);
    vaddr tz1_entry = apple_boot_load_macho(s8000->secure_monitor, &address_space_memory, NULL, TZ1_BASE, 0);
    info_report("TrustZone 1 entry: 0x%016" VADDR_PRIx, tz1_entry);
    hwaddr tz1_boot_args_pa = TZ1_BASE + (TZ1_SIZE - sizeof(AppleMonitorBootArgs));
    info_report("TrustZone 1 boot args address: 0x" HWADDR_FMT_plx, tz1_boot_args_pa);
    apple_boot_setup_monitor_boot_args(&address_space_memory, tz1_boot_args_pa, tz1_virt_low, TZ1_BASE, TZ1_SIZE,
                                       s8000->boot_info.kern_boot_args_addr, s8000->boot_info.kern_entry, g_phys_base,
                                       g_phys_slide, g_virt_slide, info->kern_text_off);
    s8000->boot_info.tz1_entry        = tz1_entry;
    s8000->boot_info.tz1_boot_args_pa = tz1_boot_args_pa;
}

static void s8000_update_device_tree(AppleS8000MachineState* s8000);

/* The boot files are given on the command line, so they only have to be read once. */
static void s8000_load_boot_files(AppleS8000MachineState* s8000)
{
    MachineState* machine = MACHINE(s8000);
    uint32_t      build_version;
    vaddr         kc_base;
    vaddr         kc_end;

    if (s8000->securerom_filename != NULL) {
        if (s8000->securerom == NULL
            && !g_file_get_contents(s8000->securerom_filename, &s8000->securerom, &s8000->securerom_size, NULL))
        {
            error_setg(&error_fatal, "Failed to load SecureROM from `%s`", s8000->securerom_filename);
        }
        return;
    }

    if (s8000->kernel != NULL) { return; }

    if (machine->dtb == NULL) {
        error_setg(&error_fatal, "Booting a kernel needs a device tree (-dtb)");
        return;
    }
    s8000->device_tree = apple_boot_load_dt_file(machine->dtb);
    if (s8000->device_tree == NULL) {
        error_setg(&error_fatal, "Failed to load device tree");
        return;
    }
    s8000_update_device_tree(s8000);

    s8000->kernel = apple_boot_load_kernel(machine->kernel_filename, &s8000->secure_monitor);
    assert_nonnull(s8000->kernel);
    assert_nonnull(s8000->secure_monitor);
    build_version = apple_boot_build_version(s8000->kernel);
    info_report("%s %u.%u.%u...", apple_boot_platform_string(s8000->kernel), BUILD_VERSION_MAJOR(build_version),
                BUILD_VERSION_MINOR(build_version), BUILD_VERSION_PATCH(build_version));
    s8000->build_version = build_version;

    apple_boot_get_kc_bounds(s8000->kernel, NULL, &kc_base, &kc_end, NULL, NULL);
    info_report("Kernel virtual low: 0x%016" VADDR_PRIx, kc_base);
    info_report("Kernel virtual high: 0x%016" VADDR_PRIx, kc_end);

    g_virt_base = kc_base;
    g_phys_base = (hwaddr)apple_boot_get_macho_buffer(s8000->kernel);

    s8000_patch_kernel(s8000->kernel);

    s8000->trustcache = apple_boot_load_trustcache_file(s8000->trustcache_filename, &s8000->boot_info.trustcache_size);
}

static void s8000_memory_setup(MachineState* machine)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    AppleBootInfo*          info  = &s8000->boot_info;
    AppleNvram*             nvram;
    bool                    auto_boot;
    char*                   cmdline;
    MachoHeader64*          header;
    AppleDTNode*            memory_map;

    s8000_load_boot_files(s8000);

    if (s8000_check_panic(s8000)) {
        qemu_system_guest_panicked(NULL);
        return;
    }

    info->dram_base = DRAM_BASE;
    info->dram_size = DRAM_SIZE;

    nvram = APPLE_NVRAM(object_resolve_path_at(OBJECT(s8000), "peripheral/nvram"));
    if (!nvram) {
        error_setg(&error_fatal, "Failed to find NVRAM device");
        return;
    }
    apple_nvram_load(nvram);

    auto_boot = env_get_bool(nvram, "auto-boot", false);

    info_report("Boot Mode: %u", s8000->boot_info.boot_mode);
    switch (s8000->boot_info.boot_mode) {
        case kAppleBootModeEnterRecovery: auto_boot = false; goto set_boot_mode;
        case kAppleBootModeExitRecovery:
            auto_boot = true;
        set_boot_mode:
            env_set_bool(nvram, "auto-boot", auto_boot, 0);
            s8000->boot_info.boot_mode = kAppleBootModeAuto;
            break;
        default: break;
    }

    info_report("auto-boot=%s", auto_boot ? "true" : "false");

    if (auto_boot) { cmdline = g_strdup(machine->kernel_cmdline); }
    else {
        cmdline = g_strconcat("-restore rd=md0 nand-enable-reformat=1 ", machine->kernel_cmdline, NULL);
    }

    apple_boot_setup_boot_nonce(nvram, info);

    apple_nvram_save(nvram);

    info->nvram_size = nvram->len;

    if (info->nvram_size > XNU_MAX_NVRAM_SIZE) { info->nvram_size = XNU_MAX_NVRAM_SIZE; }

    if (apple_nvram_serialize(nvram, info->nvram_data, sizeof(info->nvram_data)) < 0) {
        error_report("Failed to read NVRAM");
    }

    if (s8000->securerom_filename != NULL) {
        address_space_rw(&address_space_memory, SROM_BASE, MEMTXATTRS_UNSPECIFIED, s8000->securerom,
                         s8000->securerom_size, 1);
        return;
    }

    if (machine->initrd_filename == NULL && !auto_boot) {
        error_setg(&error_fatal, "RAM Disk required for recovery, please specify it via `-initrd`.");
        return;
    }

    apple_dt_unfinalise(s8000->device_tree);

    memory_map = apple_dt_get_node(s8000->device_tree, "/chosen/memory-map");

    // HACK: Use DEV Hardware model to restore without FDR errors
    apple_dt_set_prop(s8000->device_tree, "compatible", 26, "N66DEV\0iPhone8,2\0AppleARM");

    AppleDTNode* chosen = apple_dt_get_node(s8000->device_tree, "chosen");
    if (!apple_boot_contains_boot_arg(cmdline, "rd=", true)) {
        apple_dt_set_prop_strn(chosen, "root-matching", 256,
                               "<dict><key>IOProviderClass</key><string>IOMedia</"
                               "string><key>IOPropertyMatch</key><dict><key>Partition "
                               "ID</key><integer>1</integer></dict></dict>");
    }

    AppleDTNode* pram = apple_dt_get_node(s8000->device_tree, "pram");
    if (pram) {
        uint64_t panic_reg[2] = {0};
        uint64_t panic_base   = PANIC_BASE;
        uint64_t panic_size   = PANIC_SIZE;

        panic_reg[0] = panic_base;
        panic_reg[1] = panic_size;

        apple_dt_set_prop(pram, "reg", sizeof(panic_reg), &panic_reg);
        apple_dt_set_prop_u64(chosen, "embedded-panic-log-size", panic_size);
        s8000->panic_base = panic_base;
        s8000->panic_size = panic_size;
    }

    AppleDTNode* vram = apple_dt_get_node(s8000->device_tree, "vram");
    if (vram) {
        uint64_t vram_reg[2] = {0};
        uint64_t vram_base   = DISPLAY_BASE;
        uint64_t vram_size   = DISPLAY_SIZE;
        vram_reg[0]          = vram_base;
        vram_reg[1]          = vram_size;
        apple_dt_set_prop(vram, "reg", sizeof(vram_reg), &vram_reg);
    }

    NvmeNamespace* firmware = NVME_NS(object_resolve_path_at(OBJECT(s8000), "peripheral/firmware"));
    if (firmware == NULL) {
        error_setg(&error_fatal, "Failed to find firmware device");
        return;
    }
    size_t fwlen = blk_getlength(firmware->blkconf.blk);

    uint8_t* buffer = g_malloc(fwlen);

    blk_flush(firmware->blkconf.blk);
    blk_drain(firmware->blkconf.blk);

    if (blk_pread(firmware->blkconf.blk, 0, fwlen, buffer, 0) < 0) {
        error_report("%s: Failed to read firmware", __func__);
        return;
    }

    if (s8000->boot_info.ticket_data != NULL) {
        g_free(s8000->boot_info.ticket_data);
        s8000->boot_info.ticket_data   = NULL;
        s8000->boot_info.ticket_length = 0;
    }

    apple_boot_extract_manifest(buffer, fwlen, &s8000->boot_info.ticket_data, &s8000->boot_info.ticket_length);
    g_free(buffer);

    header = s8000->kernel;
    assert_nonnull(header);

    apple_boot_allocate_segment_records(memory_map, header);

    apple_boot_populate_dt(s8000->device_tree, info, auto_boot);

    switch (header->file_type) {
        case MH_EXECUTE:
        case MH_FILESET:
            s8000_load_kernelcache(s8000, cmdline);
            g_free(cmdline);
            break;
        default:
            error_setg(&error_fatal, "Unsupported kernelcache type: 0x%x\n", header->file_type);
            assert_not_reached();
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

static void s8000_update_device_tree(AppleS8000MachineState* s8000)
{
    MachineState* machine = MACHINE(s8000);
    AppleDTNode*  child;
    AppleDTNode*  node;
    GList*        iter;
    GList*        next;
    uint32_t      i;
    uint64_t      region[2];

    apple_dt_set_prop_u32(s8000->device_tree, "clock-frequency", 24000000);
    child = apple_dt_get_node(s8000->device_tree, "arm-io");
    assert_nonnull(child);

    apple_dt_set_prop_u32(child, "chip-revision", 0);

    apple_dt_set_prop(child, "clock-frequencies", sizeof(s8000_clock_frequencies), s8000_clock_frequencies);

    apple_dt_set_prop_strn(s8000->device_tree, "platform-name", 32, "s8000");
    apple_dt_set_prop_strn(s8000->device_tree, "model-number", 32, "MWL72");
    apple_dt_set_prop_strn(s8000->device_tree, "region-info", 32, "LL/A");
    apple_dt_set_prop_strn(s8000->device_tree, "config-number", 64, "");
    apple_dt_set_prop_strn(s8000->device_tree, "serial-number", 32, "C39ZRMDEN72J");
    apple_dt_set_prop_strn(s8000->device_tree, "mlb-serial-number", 32, "C39948108J9N72J1F");
    apple_dt_set_prop_strn(s8000->device_tree, "regulatory-model-number", 32, "A2111");

    child = apple_dt_get_node(s8000->device_tree, "chosen");
    apple_dt_set_prop_u32(child, "chip-id", 0x8000);
    apple_dt_set_prop_u32(child, "board-id", s8000->board_id);

    apple_dt_set_prop_u64(child, "unique-chip-id", s8000->ecid);

    // Update the display parameters
    apple_dt_set_prop_u32(child, "display-rotation", 0);
    apple_dt_set_prop_u32(child, "display-scale", 2);

    child = apple_dt_get_node(s8000->device_tree, "product");

    apple_dt_set_prop_u32(child, "oled-display", 1);
    apple_dt_set_prop_str(child, "graphics-featureset-class", "");
    apple_dt_set_prop_str(child, "graphics-featureset-fallbacks", "");
    apple_dt_set_prop_u32(child, "device-color-policy", 0);

    child = apple_dt_get_node(s8000->device_tree, "cpus");
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

    child = apple_dt_get_node(s8000->device_tree, "arm-io/aic");
    assert_nonnull(child);
    apple_dt_set_prop_u32(child, "#main-cpus", machine->smp.cpus);
    apple_dt_set_prop_u32(child, "#shared-timestamps", 0);

    child = apple_dt_get_node(s8000->device_tree, "arm-io/pmgr");
    assert_nonnull(child);
    apple_dt_set_prop(child, "voltage-states1", sizeof(s8000_voltage_states1), s8000_voltage_states1);

    child = apple_dt_get_node(s8000->device_tree, "arm-io/usb-complex");
    assert_nonnull(child);
    // no-pmu is needed for T8015, and is also necessary for S8000.
    apple_dt_set_prop_u32(child, "no-pmu", 1);

    child = apple_dt_get_node(s8000->device_tree, "arm-io/wdt");
    assert_nonnull(child);
    // TODO: MCC
    apple_dt_del_prop_named(child, "function-panic_flush_helper");
    apple_dt_del_prop_named(child, "function-panic_halt_helper");
    apple_dt_set_prop_u32(child, "no-pmu", 1);

    child = apple_dt_get_node(s8000->device_tree, "arm-io/apcie");
    assert_nonnull(child);
    // TODO: S8000 needs it, and probably T8030 does need it as well.
    apple_dt_set_prop_null(child, "apcie-phy-tunables");

    // might also work without the sart regions?
    child = apple_dt_get_node(s8000->device_tree, "arm-io/nvme-mmu0");
    assert_nonnull(child);
    region[0] = NVME_SART_BASE;
    region[1] = NVME_SART_SIZE;
    apple_dt_set_prop(child, "sart-region", sizeof(region), region);
    region[0] = apple_dt_get_prop_u32(child, "sart-virtual-base", &error_fatal);
    child     = apple_dt_get_node(s8000->device_tree, "arm-io/apcie/pci-bridge0/s3e");
    assert_nonnull(child);
    apple_dt_set_prop(child, "nvme-scratch-virt-region", sizeof(region), region);

    child = apple_dt_get_node(s8000->device_tree, "arm-io/disp0");
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

    host = usb_uplink_new(s8000->usb_uplink_type, s8000->usb_uplink_addr, &error_fatal);
    object_property_add_child(OBJECT(s8000), "usb-host", OBJECT(host));
    object_property_set_link(OBJECT(otg), "host", OBJECT(host), &error_fatal);
    object_unref(OBJECT(host));

    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 0, 0x20C000030);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 1, 0x20E0D8000);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 2, 0x20C100000);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 3, 0x20C900000);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(otg), &error_fatal);

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
    MachineState*   machine = MACHINE(s8000);
    SysBusDevice*   sbd;
    AppleDARTState* dart;
    uint32_t        i;

    dart = s8000_create_dart(s8000, "dart-disp0", 0x206304000, 0x206300000, 0x8D);

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_DISPLAY_PIPE_V2));
    object_property_add_child(OBJECT(s8000), "disp0", OBJECT(sbd));
    object_property_add_const_link(OBJECT(sbd), "dma-mr", OBJECT(apple_dart_iommu_mr(dart, 0)));
    qdev_prop_set_uint64(DEVICE(sbd), "vram-size", DISPLAY_SIZE);
    sysbus_realize_and_unref(sbd, &error_fatal);

    s8000->video_args.width        = object_property_get_uint(OBJECT(sbd), "width", &error_abort);
    s8000->video_args.height       = object_property_get_uint(OBJECT(sbd), "height", &error_abort);
    s8000->video_args.row_bytes    = s8000->video_args.width * sizeof(uint32_t);
    s8000->video_args.depth.depth  = sizeof(uint32_t) * 8;
    s8000->video_args.depth.rotate = 1;
    s8000->video_args.base_addr    = DISPLAY_BASE;
    s8000->video_args.display      = !apple_boot_contains_boot_arg(machine->kernel_cmdline, "-s", false)
                                     && !apple_boot_contains_boot_arg(machine->kernel_cmdline, "-v", false);

    sysbus_mmio_map(sbd, 0, 0x206200000);
    sysbus_mmio_map(sbd, 1, 0x206400000);
    sysbus_mmio_map(sbd, 2, 0x206440000);
    sysbus_mmio_map(sbd, 3, 0x206480000);
    sysbus_mmio_map(sbd, 4, 0x2064C0000);
    sysbus_mmio_map(sbd, 5, 0x206500000);
    sysbus_connect_irq(sbd, 0, s8000_irq(s8000, 0x8C));
    for (i = 0; i < 4; i++) { sysbus_connect_irq(sbd, 1 + i, s8000_irq(s8000, 0x8F + i)); }

    adp_v2_update_vram_mapping(APPLE_DISPLAY_PIPE_V2(sbd), s8000->sys_mem, s8000->video_args.base_addr);
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
    qdev_prop_set_uint32(DEVICE(sbd), "board-id", s8000->board_id);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x20A108000);
    sysbus_mmio_map(sbd, 1, 0x2102D0000);
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

static void s8000_cpu_reset(AppleS8000MachineState* s8000)
{
    CPUState* cpu;
    AppleA9*  acpu;

    CPU_FOREACH (cpu) {
        acpu = APPLE_A9(cpu);
        if (s8000->securerom_filename == NULL) {
            object_property_set_int(OBJECT(cpu), "rvbar", TZ1_BASE, &error_abort);
            cpu_reset(cpu);
            if (acpu->cpu_id == 0) {
                arm_set_cpu_on(acpu->parent_obj.mp_affinity, s8000->boot_info.tz1_entry,
                               s8000->boot_info.tz1_boot_args_pa, 3, true);
            }
        }
        else {
            object_property_set_int(OBJECT(cpu), "rvbar", SROM_BASE, &error_abort);
            cpu_reset(cpu);
            if (acpu->cpu_id == 0) { arm_set_cpu_on(acpu->parent_obj.mp_affinity, SROM_BASE, 0, 3, true); }
        }
    }
}

static void s8000_reset(MachineState* machine, ResetType type)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    DeviceState*            gpio  = NULL;

    qemu_devices_reset(type);

    if (!runstate_check(RUN_STATE_PRELAUNCH)) { s8000_memory_setup(MACHINE(s8000)); }

    s8000_cpu_reset(s8000);

    gpio = DEVICE(object_property_get_link(OBJECT(s8000), "gpio", &error_fatal));

    qemu_set_irq(qdev_get_gpio_in(gpio, GPIO_FORCE_DFU), s8000->force_dfu);
}

static void s8000_init_done(Notifier* notifier, void* data)
{
    AppleS8000MachineState* s8000 = container_of(notifier, AppleS8000MachineState, init_done_notifier);
    s8000_memory_setup(MACHINE(s8000));
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

    s8000_cpu_setup(s8000);
    s8000_create_aic(s8000);
    s8000_create_devices(s8000);

    s8000->init_done_notifier.notify = s8000_init_done;
    qemu_add_machine_init_done_notifier(&s8000->init_done_notifier);
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
PROP_GETTER_SETTER(bool, kaslr_off);
PROP_GETTER_SETTER(bool, force_dfu);
PROP_STR_GETTER_SETTER(trustcache_filename);
PROP_STR_GETTER_SETTER(sep_rom_filename);
PROP_STR_GETTER_SETTER(securerom_filename);
PROP_STR_GETTER_SETTER(usb_uplink_addr);
PROP_GETTER_SETTER(int, usb_uplink_type);

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
    object_class_property_add_str(klass, "sep-rom", s8000_get_sep_rom_filename, s8000_set_sep_rom_filename);
    object_class_property_set_description(klass, "sep-rom", "SEP ROM");
    object_class_property_add_str(klass, "securerom", s8000_get_securerom_filename, s8000_set_securerom_filename);
    object_class_property_set_description(klass, "securerom", "SecureROM");
    object_class_property_add_str(klass, "boot-mode", s8000_get_boot_mode, s8000_set_boot_mode);
    object_class_property_set_description(klass, "boot-mode", "Boot Mode");
    object_class_property_add_bool(klass, "kaslr-off", s8000_get_kaslr_off, s8000_set_kaslr_off);
    object_class_property_set_description(klass, "kaslr-off", "Disable KASLR");
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
