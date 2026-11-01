/*
 * Simulated Apple iBoot.
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
#include "crypto/hash.h"
#include "fs/apfs.h"
#include "graphics/inferno_pixbuf.h"
#include "hw/arm/dart.h"
#include "hw/arm/dt.h"
#include "hw/arm/iboot/boot.h"
#include "hw/arm/iboot/dfu.h"
#include "hw/arm/iboot/iboot.h"
#include "hw/arm/iboot/kernel_patches.h"
#include "hw/arm/iboot/virt_patches.h"
#include "hw/arm/mem.h"
#include "hw/arm/sep/emu.h"
#include "hw/display/apple_displaypipe_v4.h"
#include "hw/nvme/nvme.h"
#include "hw/nvram/apple_nvram.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "arm-powerctl.h"
#include "target/arm/internals.h"
#ifdef CONFIG_PNG
    #include <png.h>
#else
    #error "PNG support is required"
#endif

#if 0
    #define DINFO(fmt, ...) info_report(fmt, ##__VA_ARGS__)
#else
    #define DINFO(fmt, ...) \
        do { }              \
        while (0)
#endif

#define PREBOOT_APTICKET    "System/Library/Caches/apticket.der"
#define PREBOOT_KERNELCACHE "System/Library/Caches/com.apple.kernelcaches/kernelcache"
#define PREBOOT_DEVICETREE  "usr/standalone/firmware/devicetree.img4"
#define PREBOOT_TRUSTCACHE  "usr/standalone/firmware/FUD/StaticTrustCache.img4"
#define PREBOOT_ROOT_HASH   "usr/standalone/firmware/root_hash.img4"
#define PREBOOT_SPTM        "usr/standalone/firmware/FUD/Ap,SecurePageTableMonitor.img4"
#define PREBOOT_TXM         "usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4"

#define CARVEOUT_ALIGN (16 * KiB)

#define BOOT_ARGS_SIZE (0x4000)

#define DISPLAY_DART_PAGE_SIZE (16 * KiB)
#define DISPLAY_FB_IOVA        (0x10000000)

#define SPLASH_DIR      CONFIG_QEMU_ICONDIR "/hicolor/512x512/apps/inferno/"
#define SPLASH_BOOT     "bootsplash@2x.png"
#define SPLASH_RECOVERY "bootsplash-recovery@2x.png"
#define SPLASH_DFU      "bootsplash-dfu@2x.png"

#define PMAP_IO_RANGE_SIZE         (24)
#define PMAP_IO_RANGE_CARVEOUT_SIG (0x43415256)    // 'CARV'

#define L2_GRANULE      ((0x4000) * (0x4000 / 8))
#define L2_GRANULE_MASK (L2_GRANULE - 1)

typedef struct
{
    AppleCarveoutID id;
    hwaddr          base;
    hwaddr          size;
} AppleIBootRegion;

struct AppleIBootSplash
{
    AppleBootInfo* info;
    QEMUTimer*     timer;
    InfernoPixbuf  image;
    int            dest_x;
    int            dest_y;
};

/// What one boot of an OS loads and where it puts it.
typedef struct
{
    const char*    cmdline;
    bool           auto_boot;
    uint8_t*       nvram;
    uint32_t       nvram_size;
    bool           has_security_mode_change_enable;
    uint32_t       security_mode_change_enable;
    AppleDTNode*   device_tree;
    MachoHeader64* kernel;
    uint32_t       build_version;
    MachoHeader64* secure_monitor;
    MachoHeader64* sptm;
    MachoHeader64* txm;
    uint8_t*       trustcache;
    uint8_t*       ramdisk;
    gchar*         ticket;
    gsize          ticket_length;
    hwaddr         device_tree_addr;
    uint64_t       device_tree_size;
    hwaddr         ramdisk_addr;
    uint64_t       ramdisk_size;
    hwaddr         trustcache_addr;
    uint64_t       trustcache_size;
    hwaddr         sep_fw_addr;
    uint64_t       sep_fw_size;
    hwaddr         boot_args_addr;
    uint64_t       boot_args_size;
    hwaddr         tz0_addr;
    uint64_t       tz0_size;
    hwaddr         top_of_kernel_data;
} AppleIBootOS;

static const uint8_t apple_iboot_sep_nonce[] = {
    0xFE, 0xED, 0xFA, 0xCE, 0xFE, 0xED, 0xFA, 0xCE, 0xFE, 0xED,
    0xFA, 0xCE, 0xFE, 0xED, 0xFA, 0xCE, 0xFE, 0xED, 0xFA, 0xCE,
};

static const char* KEEP_COMP[] = {
    "adbe0,s8000\0$",
    // "aop-audio-haptic\0$",
    // "aop-audio\0$",
    // "aop-audio-control\0$",
    // "aop-audio-speaker\0$",
    // "audio-aop-lp-mic-in\0$",
    // "audio-aop-mca2\0$",
    // "audio-aop-pcmaudiomgr\0$",
    // "alc,t8030\0$",
    // "audio-data,aop-audio-haptic\0$",
    // "audio-data,halogen\0$",
    // "audio-data,hawking\0$",
    // "audio-data,baseband-voice\0$",
    // "audio-data,dsp-debug1\0$",
    // "audio-data,mikeybus-secondary\0$",
    // "audio-data,aop-audio-hpdb\0$",
    // "halle-sensor,aop-ad5860-config\0$",
    // "audio-aop-hall\0$",
    // "audio-aop-hpdbg\0$",
    // "audio-aop-haptic\0$",
    // "audio-aop-haptic-leap\0$",
    // "audio-aop-hapticmgr\0$",
    // "aop-audio-hpdbg\0$",
    // "audio-data,aop-audio-hpdbg\0$",
    // "haptics-support,leap\0$",
    // "audio,embedded-resource-manager\0$",
    // "audio-control,cs35l27\0$",
    // "audio-data,audio-loopback\0$",
    // "audio-data,cs35l27\0$",
    // "audio-data,cs42l77\0$",
    // "audio-control,cs42l77\0$",
    "aes,s8000\0$",
    "aic,1\0$",
    "apcie-bridge\0$",
    "apple,lightning\0ARM,v8\0$",
    "apple,thunder\0ARM,v8\0$",
    "apple,twister\0ARM,v8\0$",
    "arm-io,s8000\0$",
    "arm-io,t8030\0$",
    "atc-phy,t8030\0atc-phy,t8027\0$",
    "backlight\0$",
    "backlight,lm3539\0$",
#ifdef ENABLE_BASEBAND
    "baseband,i19\0$",
#endif
    // "biosensor,pearl\0$", // not implemented
    "buttons\0$",
    // "bluetooth,n88\0$",
    "dart,s8000\0dart,s5l8960x\0$",
    "dart,t8020\0$",
    "disp0,t8030\0$",
    "display-pmu,chestnut\0$",
    "dock,9pin\0$",
    "dock,0pin\0$",
    "gen-mt-decider\0$",
    "gpio,s8000\0gpio,s5l8960x\0$",
    "gpio,t8015\0gpio,s5l8960x\0$",
    "gpio,t8030\0gpio,s5l8960x\0$",
    "i2c,s8000\0i2c,s5l8940x\0iic,soft\0$",
    "i2c,t8030\0i2c,s5l8940x\0iic,soft\0$",
    "iic,soft\0$",
    "iommu-mapper\0$",
    "iop,ascwrap-v2\0$",
    "iop,t8030\0iop,t8015\0$",
    "iop-nub,rtbuddy-v2\0$",
    "iop,s8000\0$",
    "iop-nub,sep\0$",
    "lcd,pinot\0$",
    // "mca-switch,t8030\0$",
    // "mcaCluster,t8030\0$",
    // "mca,t8030\0$",
    "mipi-dsim-1,synopsys\0$",
    "N104AP\0iPhone12,1\0AppleARM\0$",
    "N104DEV\0iPhone12,1\0AppleARM\0$",
    "N66AP\0iPhone8,2\0AppleARM\0$",
    "N66DEV\0iPhone8,2\0AppleARM\0$",
    "nvme-mmu,s8000\0$",
    "otgphyctrl,s8000\0otgphyctrl,s5l8960x\0$",
    "pmgr1,s8000\0$",
    "pmgr1,t8030\0$",
    // "pmu,d2255\0$", // buggy
    "pmu,spmi\0pmu,avus\0$",
    "roswell\0$",
    "sart,coastguard\0$",
    "sart,t8030\0$",
    "sacm,1\0$",
    "scaler,t8030\0scaler,s5l8960x\0$",
    "sio-dma-controller\0$",
    "smc-pmu\0$",
    "smc-tempsensor\0$",
    "soc-tuner,s8000\0$",
    "soc-tuner,t8030\0$",
    "spi-1,samsung\0$",
    "spmi,gen0\0$",
    "spmi,t8015\0$",
    "uart-1,samsung\0$",
    "usb-complex,s8000\0usb-complex,s5l8960x\0$",
    "usb-device,s5l8900x\0$",
    "usb-device,s8000\0usb-device,t7000\0usb-device,s5l8900x\0$",
    "usb-device,t7000\0usb-device,s5l8900x\0$",
    "usb-drd,t8030\0usb-drd,t8027\0$",
    "wdt,s8000\0wdt,s5l8960x\0$",
    "wdt,t8030\0wdt,s5l8960x\0$",
    "apcie,t8030\0$",
    "apcie,s8000\0$",
    "rsm,t8030\0$",
    "tempsensor,t8020\0$",
    "tempsensor,t8030\0$",
    "mtrtempsensor,t8020\0$",
    "dietempctl,t8030\0$",
    "vtempctl,t8030\0$",
    "aod,t8030\0aod,s5l8960x\0$",
    "mic-temp-sens,ica60\0$",
    "aapl,spmi\0$",
    "accbuck,fan53740\0$",
    "mcc,t8030\0$",
};

static const char* REM_NAMES[] = {
    "accel\0$",
    "gyro\0$",
    "compass\0$",
    "spherecontrol\0$",
    "prox\0$",
    "SPUApp\0$",
    "aop-mca\0$",
    "smc-control\0$",
    "aop-smart-cover\0$",
    "rose\0$",
    "smc-aop\0$",
    "gfx-asc\0$",
    "amfm\0$",
    "dart-ane\0$",
    "dart-avd\0$",
    "dart-ave\0$",
    "dart-isp\0$",
    "dart-jpeg0\0$",
    "dart-jpeg1\0$",
    "dockchannel-uart\0$",
    "stockholm\0$",
    "stockholm-spmi\0$",
    "bluetooth-pcie\0$",
    "wlan\0$",
#ifndef ENABLE_BASEBAND
    "baseband\0$",
    "baseband-spmi\0$",
    "baseband-vol\0$",
    "baseband-pcie\0$",
    "audio-baseband-voice\0$",
#endif
    // doing this won't^H^H^Hwill fix sepfw 17
    // should also work with 14beta5
    "Lynx\0$",
    "iap\0$",    // iOS 13
};

static const char* REM_DEV_TYPES[] = {
    "wlan\0$",
};

static const char* REM_PROPS[] = {
#ifndef ENABLE_DATA_ENCRYPTION
    "content-protect",
    "encryptable",
#endif
    "function-dock_parent",
    "function-error_handler",
    "nand-debug",
    "nvme-coastguard",    // FIXME: panic: "pmap_iommu_map failed\n"
    "soc-tuning",
    "invokes-sochot",
    "has-dapf",
#ifndef ENABLE_BASEBAND
    "baseband-chipset",
    "has-baseband",
    "event_name-gpio9",
    "gps-capable",
    "location-reminders",
    "personal-hotspot",
    "bitrate-3g",
    "bitrate-lte",
    "bitrate-2g",
    "navigation",
#endif
    "pearl-camera",
    "face-detection-support",
    "siri-gesture",
#if 0
    "pci-l1pm-control", // TODO?
    "pci-aspm-default",
    "manual-enable-s2r",
    "pci-l1pm-control-postrom",
    "pci-l1pm-control-a0",
#endif
};

static const char* srawmemchr(const char* str, int chr)
{
    while (*str != chr) { str++; }

    return str;
}

static uint64_t sstrlen(const char* str) { return srawmemchr(str, '$') - str; }

static void apple_iboot_process_dt_node(AppleDTNode* node, AppleDTNode* parent)
{
    GList*       iter  = NULL;
    AppleDTNode* child = NULL;
    AppleDTProp* prop  = NULL;
    uint64_t     i;
    bool         found;

    if ((prop = apple_dt_get_prop(node, "compatible")) != NULL) {
        assert_nonnull(prop->data);
        found = false;
        for (i = 0; i < ARRAY_SIZE(KEEP_COMP); i++) {
            if (memcmp(prop->data, KEEP_COMP[i], MIN(prop->len, sstrlen(KEEP_COMP[i]))) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            assert_nonnull(parent);
            DINFO("Removing node `%s` (non-whitelisted compatible property `%s`)",
                  apple_dt_get_prop_str_or(node, "name", "(null)", &error_fatal), (char*)prop->data);
            apple_dt_del_node(parent, node);
            return;
        }
    }

    if ((prop = apple_dt_get_prop(node, "name")) != NULL) {
        assert_nonnull(prop->data);
        for (i = 0; i < ARRAY_SIZE(REM_NAMES); i++) {
            uint64_t size = MIN(prop->len, sstrlen(REM_NAMES[i]));
            if (memcmp(prop->data, REM_NAMES[i], size) == 0) {
                assert_nonnull(parent);
                DINFO("Removing node `%s` (blacklisted name)", (char*)prop->data);
                apple_dt_del_node(parent, node);
                return;
            }
        }
    }

    if ((prop = apple_dt_get_prop(node, "device_type")) != NULL) {
        assert_nonnull(prop->data);
        for (i = 0; i < ARRAY_SIZE(REM_DEV_TYPES); i++) {
            uint64_t size = MIN(prop->len, sstrlen(REM_DEV_TYPES[i]));
            if (memcmp(prop->data, REM_DEV_TYPES[i], size) == 0) {
                assert_nonnull(parent);
                DINFO("Removing node `%s` (blacklisted device type `%s`)",
                      apple_dt_get_prop_str_or(node, "name", "(null)", &error_fatal), (char*)prop->data);
                apple_dt_del_node(parent, node);
                return;
            }
        }
    }

    for (i = 0; i < ARRAY_SIZE(REM_PROPS); i++) {
        if (apple_dt_del_prop_named(node, REM_PROPS[i])) {
            DINFO("Removing prop `%s` from node `%s`", REM_PROPS[i],
                  apple_dt_get_prop_str_or(node, "name", "(null)", &error_fatal));
        }
    }

    for (iter = node->children; iter != NULL;) {
        child = iter->data;

        // iter might get invalidated
        iter = iter->next;
        apple_iboot_process_dt_node(child, node);
    }
}

static void apple_iboot_init_mem_ranges(AppleDTNode* root)
{
    AppleDTNode* child;

    child = apple_dt_get_node(root, "chosen/memory-map");
    assert_nonnull(child);

    apple_dt_set_prop(child, "RAMDisk", 16, NULL);
    apple_dt_set_prop(child, "TrustCache", 16, NULL);
    apple_dt_set_prop(child, "SEPFW", 16, NULL);
    apple_dt_set_prop(child, "BootArgs", 16, NULL);
    apple_dt_set_prop(child, "DeviceTree", 16, NULL);
}

static QCryptoHashAlgo apple_iboot_get_crypto_hash_alg(AppleDTNode* chosen)
{
    const char* crypto_hash_method = apple_dt_get_prop_str(chosen, "crypto-hash-method", &error_fatal);

    if (crypto_hash_method == NULL || strcmp(crypto_hash_method, "sha1") == 0) { return QCRYPTO_HASH_ALGO_SHA1; }
    if (strcmp(crypto_hash_method, "sha2-384") == 0) { return QCRYPTO_HASH_ALGO_SHA384; }

    assert_not_reached();
}

static void apple_iboot_setup_boot_nonce(AppleNvram* nvram, AppleBootInfo* info)
{
    const char* str = env_get(nvram, "com.apple.System.boot-nonce");

    if (str == NULL || qemu_strtou64(str, NULL, 16, &info->boot_nonce_generator) != 0) {
        g_autofree char* new_str = NULL;

        qemu_guest_getrandom_nofail(&info->boot_nonce_generator, sizeof(info->boot_nonce_generator));
        new_str = g_strdup_printf("0x%016" PRIx64, info->boot_nonce_generator);
        env_set(nvram, "com.apple.System.boot-nonce", new_str, 0);
    }
}

static void apple_iboot_compute_ap_nonce(QCryptoHashAlgo alg, uint64_t generator, uint8_t** nonce, size_t* nonce_len)
{
    uint8_t data[sizeof(generator)];
    size_t  hash_len = 0;

    stq_le_p(data, generator);
    qcrypto_hash_bytes(alg, (const char*)data, sizeof(data), nonce, &hash_len, &error_fatal);

    *nonce_len = alg == QCRYPTO_HASH_ALGO_SHA1 ? 20 : 32;
    assert_cmpuint(hash_len, >=, *nonce_len);
}

static void apple_iboot_populate_dt(AppleBootInfo* info, AppleIBootOS* os)
{
    AppleDTNode* root = os->device_tree;
    AppleDTNode* child;
    AppleDTProp* prop;
    uint8_t*     nonce     = NULL;
    size_t       nonce_len = 0;

    assert_cmphex(os->nvram_size, <=, XNU_MAX_NVRAM_SIZE);

    apple_dt_set_prop_null(root, "no-suspend");

    child = apple_dt_get_node(root, "chosen");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "random-seed");
    assert_nonnull(prop);
    qemu_guest_getrandom_nofail(prop->data, prop->len);

    prop = apple_dt_get_prop(child, "cl4-entropy");
    if (prop != NULL) { qemu_guest_getrandom_nofail(prop->data, prop->len); }

    prop = apple_dt_get_prop(child, "boot-nonce");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, ==, sizeof(info->boot_nonce_generator));
    stq_le_p(prop->data, info->boot_nonce_generator);

    apple_dt_set_prop_u64(child, "dram-base", info->dram_base);
    apple_dt_set_prop_u64(child, "dram-size", info->dram_size);
    apple_dt_set_prop_str(child, "firmware-version", "ChefKiss Inferno");
    apple_dt_set_prop_str(child, "system-firmware-version", QEMU_VERSION);

    apple_dt_set_prop_u32(child, "nvram-total-size", os->nvram_size);
    apple_dt_set_prop_u32(child, "nvram-bank-size", os->nvram_size);
    apple_dt_set_prop(child, "nvram-proxy-data", os->nvram_size, os->nvram);

    // apple_dt_set_prop_u32(child, "research-enabled", 1);
    apple_dt_set_prop_u32(child, "effective-production-status-ap", 1);
    apple_dt_set_prop_u32(child, "effective-security-mode-ap", 1);
    apple_dt_set_prop_u32(child, "security-domain", 1);
    apple_dt_set_prop_u32(child, "chip-epoch", 1);
    // apple_dt_set_prop_u32(child, "debug-enabled", 1);

    // fstab os_env_type:
    // 0x01: fstab
    // 0x02: fstab-ephemeral-recovery-data
    // 0x03: fstab-ephemeral-diag-data
    // darwinos-security-environment:
    // 0x04: macos-darwinos-environment
    // 0x05: embedded-darwinos-environment
    // 0x06: trusted-darwinos-environment/"PrivateCloudOS detected"
    // TODO: Don't set this one manually, process osenvironments node.
    apple_dt_set_prop_u32(child, "darwinos-security-environment", os->auto_boot ? 1 : 2);
    // 7: Erase/darwinOS RAMDisk?
    // 3: Upgrade RAMDisk?
    // 1: Main OS?
    apple_dt_set_prop_u32(child, "boot-command", os->auto_boot ? 1 : 7);

    child = apple_dt_get_node(root, "defaults");
    assert_nonnull(child);

    apple_dt_del_prop_named(child, "entangle-nonce");

    child = apple_dt_get_node(root, "chosen/manifest-properties");
    assert_nonnull(child);
    if (os->ticket != NULL && os->ticket_length != 0) {
        apple_boot_populate_manifest_props(child, (const uint8_t*)os->ticket, os->ticket_length);
    }
    if (apple_dt_get_prop(child, "BNCH") == NULL) {
        apple_iboot_compute_ap_nonce(apple_iboot_get_crypto_hash_alg(apple_dt_get_node(root, "chosen")),
                                     info->boot_nonce_generator, &nonce, &nonce_len);
        apple_dt_set_prop(child, "BNCH", nonce_len, nonce);
        g_free(nonce);
    }

    child = apple_dt_get_node(root, "product");
    assert_nonnull(child);
    apple_dt_set_prop_u32(child, "allow-hactivation", 1);

    apple_iboot_init_mem_ranges(root);

    apple_iboot_process_dt_node(root, NULL);

    // Prevent further additions.
    os->device_tree_size = ROUND_UP_16K(apple_dt_finalise(root));
}

static void apple_iboot_set_memory_range(AppleDTNode* root, const char* name, uint64_t addr, uint64_t size)
{
    AppleDTNode* child;
    AppleDTProp* prop;

    child = apple_dt_get_node(root, "chosen/memory-map");
    assert_nonnull(child);

    if (addr == 0) {
        assert_cmphex(size, ==, 0);
        apple_dt_del_prop_named(child, name);
        return;
    }

    prop = apple_dt_get_prop(child, name);
    assert_nonnull(prop);

    stq_le_p(prop->data, addr);
    stq_le_p(prop->data + sizeof(uint64_t), size);
}

static void apple_iboot_finalise_dt(AppleIBootOS* os)
{
    AppleDTNode*    root = os->device_tree;
    uint8_t*        buf;
    QCryptoHashAlgo alg;
    uint8_t*        hash     = NULL;
    size_t          hash_len = 0;
    AppleDTNode*    child;
    AppleDTProp*    prop;

    apple_iboot_set_memory_range(root, "DeviceTree", os->device_tree_addr, os->device_tree_size);
    apple_iboot_set_memory_range(root, "RAMDisk", os->ramdisk_addr, os->ramdisk_size);
    apple_iboot_set_memory_range(root, "TrustCache", os->trustcache_addr, os->trustcache_size);
    apple_iboot_set_memory_range(root, "SEPFW", os->sep_fw_addr, os->sep_fw_size);
    apple_iboot_set_memory_range(root, "BootArgs", os->boot_args_addr, os->boot_args_size);

    if (os->ticket != NULL && os->ticket_length != 0) {
        child = apple_dt_get_node(root, "chosen");
        assert_nonnull(child);

        alg = apple_iboot_get_crypto_hash_alg(child);

        prop = apple_dt_get_prop(child, "boot-manifest-hash");
        assert_nonnull(prop);

        if (qcrypto_hash_bytes(alg, os->ticket, os->ticket_length, &hash, &hash_len, &error_fatal) >= 0) {
            assert_cmpuint(hash_len, ==, prop->len);
            memcpy(prop->data, hash, hash_len);
            g_free(hash);
            hash     = NULL;
            hash_len = 0;
        }
        else {
            return;
        }
    }

    buf = g_malloc0(os->device_tree_size);
    apple_dt_serialise(root, buf);

    address_space_rw(&address_space_memory, os->device_tree_addr, MEMTXATTRS_UNSPECIFIED, buf, os->device_tree_size,
                     true);
    g_free(buf);
}

static bool apple_iboot_contains_boot_arg(const char* boot_args, const char* arg, bool match_prefix)
{
    g_autofree char* args = g_strdup(boot_args);
    char*            pos  = args;
    char*            token;
    size_t           arglen = strlen(arg);

    if (args == NULL) { return false; }

    while ((token = qemu_strsep(&pos, " ")) != NULL) {
        if ((match_prefix && strncmp(token, arg, arglen) == 0) || strcmp(token, arg) == 0) { return true; }
    }

    return false;
}

static void apple_iboot_setup_monitor_boot_args(AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                                                hwaddr mem_size, hwaddr kern_args, vaddr kern_entry,
                                                hwaddr kern_phys_base, hwaddr kern_phys_slide, vaddr kern_virt_slide,
                                                vaddr kern_text_section_off)
{
    AppleMonitorBootArgs args = {0};

    args.version               = 4;
    args.virt_base             = virt_base;
    args.phys_base             = phys_base;
    args.mem_size              = mem_size;
    args.kern_args             = kern_args;
    args.kern_entry            = kern_entry;
    args.kern_phys_base        = kern_phys_base;
    args.kern_phys_slide       = kern_phys_slide;
    args.kern_virt_slide       = kern_virt_slide;
    args.kern_text_section_off = kern_text_section_off;
    qemu_guest_getrandom_nofail(args.random_bytes, sizeof(args.random_bytes));

    address_space_rw(as, addr, MEMTXATTRS_UNSPECIFIED, &args, sizeof(args), true);
}

static void apple_iboot_setup_bootargs_rev2(AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                                            hwaddr mem_size, hwaddr kernel_top, vaddr dtb_va, vaddr dtb_size,
                                            AppleVideoArgs* video_args, const char* cmdline, hwaddr mem_size_actual)
{
    AppleKernelBootArgsRev2 args = {0};

    args.revision           = 2;
    args.version            = 2;
    args.virt_base          = virt_base;
    args.phys_base          = phys_base;
    args.mem_size           = mem_size;
    args.kernel_top         = kernel_top;
    args.video_args         = *video_args;
    args.device_tree_ptr    = dtb_va;
    args.device_tree_length = dtb_size;
    if (cmdline != NULL) { g_strlcpy(args.cmdline, cmdline, sizeof(args.cmdline)); }
    args.boot_flags      = 0;
    args.mem_size_actual = mem_size_actual;

    // iOS 13: mem_size_actual is not a thing
    // however, the boot args region should always
    // be large enough (one page).
    address_space_rw(as, addr, MEMTXATTRS_UNSPECIFIED, &args, sizeof(args), true);
}

static void apple_iboot_setup_bootargs_rev3(AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                                            hwaddr mem_size, hwaddr kernel_top, vaddr dtb_va, vaddr dtb_size,
                                            AppleVideoArgs* video_args, const char* cmdline, hwaddr mem_size_actual)
{
    AppleKernelBootArgsRev3 args = {0};

    args.revision           = 3;
    args.version            = 2;
    args.virt_base          = virt_base;
    args.phys_base          = phys_base;
    args.mem_size           = mem_size;
    args.kernel_top         = kernel_top;
    args.video_args         = *video_args;
    args.device_tree_ptr    = dtb_va;
    args.device_tree_length = dtb_size;
    if (cmdline != NULL) { g_strlcpy(args.cmdline, cmdline, sizeof(args.cmdline)); }
    args.boot_flags      = 0;
    args.mem_size_actual = mem_size_actual;

    address_space_rw(as, addr, MEMTXATTRS_UNSPECIFIED, &args, sizeof(args), true);
}

static void apple_iboot_setup_bootargs(uint32_t build_version, AddressSpace* as, hwaddr addr, vaddr virt_base,
                                       hwaddr phys_base, hwaddr mem_size, hwaddr kernel_top, vaddr dtb_va,
                                       vaddr dtb_size, AppleVideoArgs* video_args, const char* cmdline,
                                       hwaddr mem_size_actual)
{
    if (BUILD_VERSION_MAJOR(build_version) >= 17) {
        apple_iboot_setup_bootargs_rev3(as, addr, virt_base, phys_base, mem_size, kernel_top, dtb_va, dtb_size,
                                        video_args, cmdline, mem_size_actual);
    }
    else {
        apple_iboot_setup_bootargs_rev2(as, addr, virt_base, phys_base, mem_size, kernel_top, dtb_va, dtb_size,
                                        video_args, cmdline, mem_size_actual);
    }
}

static MachoSegmentCommand64* apple_iboot_get_first_seg(MachoHeader64* header)
{
    MachoSegmentCommand64* sgp;
    uint32_t               i;

    sgp = (MachoSegmentCommand64*)(header + 1);

    for (i = 0; i < header->n_cmds; i++, sgp = (MachoSegmentCommand64*)((char*)sgp + sgp->cmd_size)) {
        if (sgp->cmd == LC_SEGMENT_64) { return sgp; }
    }

    // not found
    return NULL;
}

static MachoSegmentCommand64* apple_iboot_get_next_seg(MachoHeader64* header, MachoSegmentCommand64* seg)
{
    MachoSegmentCommand64* sgp;
    uint32_t               i;
    bool                   found = false;

    sgp = (MachoSegmentCommand64*)(header + 1);

    for (i = 0; i < header->n_cmds; i++, sgp = (MachoSegmentCommand64*)((char*)sgp + sgp->cmd_size)) {
        if (found && sgp->cmd == LC_SEGMENT_64) { return sgp; }
        if (seg == sgp) { found = true; }
    }

    // not found
    return NULL;
}

static MachoSection64* apple_iboot_first_sect(MachoSegmentCommand64* seg) { return (MachoSection64*)(seg + 1); }

static MachoSection64* apple_iboot_next_sect(MachoSection64* sp) { return sp + 1; }

static MachoSection64* apple_iboot_end_sect(MachoSegmentCommand64* seg)
{
    MachoSection64* sp;

    sp = (MachoSection64*)(seg + 1);
    return &sp[seg->nsects];
}

static void apple_iboot_process_symbols(MachoHeader64* header, uint64_t slide)
{
    MachoLoadCommand*      cmd;
    uint8_t*               data;
    vaddr                  text_base;
    vaddr                  kernel_low;
    vaddr                  kernel_high;
    uint32_t               index;
    uint8_t*               base;
    MachoSegmentCommand64* linkedit_seg;
    MachoNList64*          sym;
    uint32_t               off;

    if (slide == 0) { return; }

    apple_boot_get_kc_bounds(header, &text_base, &kernel_low, &kernel_high, NULL, NULL);

    data         = apple_boot_get_macho_buffer(header);
    linkedit_seg = apple_boot_get_segment(header, "__LINKEDIT");

    cmd = (MachoLoadCommand*)(header + 1);
    for (index = 0; index < header->n_cmds; index++) {
        switch (cmd->cmd) {
            case LC_SYMTAB: {
                MachoSymtabCommand* symtab = (MachoSymtabCommand*)cmd;
                if (linkedit_seg == NULL) {
                    error_report("Did not find __LINKEDIT segment");
                    return;
                }
                base = data + (linkedit_seg->vmaddr - kernel_low);
                off  = linkedit_seg->fileoff;
                sym  = (MachoNList64*)(base + (symtab->sym_off - off));
                for (int i = 0; i < symtab->nsyms; i++) {
                    if (sym[i].n_type & N_STAB) { continue; }
                    sym[i].n_value += slide;
                }
                break;
            }
            case LC_DYSYMTAB: {
                MachoDysymtabCommand* dysymtab = (MachoDysymtabCommand*)cmd;
                if (!dysymtab->loc_rel_n) { break; }

                if (linkedit_seg == NULL) {
                    error_report("Did not find __LINKEDIT segment");
                    return;
                }

                base = data + (linkedit_seg->vmaddr - kernel_low);
                off  = linkedit_seg->fileoff;
                for (size_t i = 0; i < dysymtab->loc_rel_n; i++) {
                    int32_t r_address = *(int32_t*)(base + (dysymtab->loc_rel_off - off) + (i * 8));
                    *(uint64_t*)(data + ((text_base - kernel_low) + r_address)) += slide;
                }
                break;
            }
            default: break;
        }
        cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size);
    }
}

static void apple_iboot_allocate_segment_records(AppleDTNode* memory_map, MachoHeader64* header)
{
    unsigned int      index;
    MachoLoadCommand* cmd;

    cmd = (MachoLoadCommand*)((char*)header + sizeof(MachoHeader64));
    for (index = 0; index < header->n_cmds; index++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd != LC_SEGMENT_64) { continue; }
        MachoSegmentCommand64* segCmd = (MachoSegmentCommand64*)cmd;
        char                   region_name[32];

        snprintf(region_name, sizeof(region_name), "Kernel-%s", segCmd->segname);
        struct MemoryMapFileInfo
        {
            uint64_t paddr;
            uint64_t length;
        } file_info = {0};
        apple_dt_set_prop(memory_map, region_name, sizeof(file_info), &file_info);
    }
}

static void apple_iboot_allocate_sptm_records(AppleDTNode* memory_map)
{
    static const char* const regions[] = {
        "TXM-ro",        "TXM-rx",    "TXM-bx",    "TXM-rw",    "TXM-le",     "BootKC-rx",   "BootKC-bx",
        "BootKC-ro",     "BootKC-rs", "BootKC-rw", "BootKC-le", "SPTM-ro",    "SPTM-rx",     "SPTM-rw",
        "SPTM-le",       "TXM-virt",  "TXM-entry", "SPTM-virt", "SPTM-entry", "BootKC-virt", "BootKC-entry",
        "CL4-dummypage", "slide",     "CL4-rx",    "CL4-ro",    "CL4-entry",  "CL4-virt",
    };

    for (size_t i = 0; i < ARRAY_SIZE(regions); i++) { apple_dt_set_prop(memory_map, regions[i], 16, NULL); }
}

static vaddr apple_iboot_load_macho(MachoHeader64* header, AddressSpace* as, AppleDTNode* memory_map, hwaddr phys_base,
                                    vaddr virt_slide)
{
    uint8_t*          data = NULL;
    unsigned int      i;
    MachoLoadCommand* cmd;
    hwaddr            pc = 0;
    data                 = apple_boot_get_macho_buffer(header);
    vaddr          kc_base;
    vaddr          kc_end;
    bool           is_fileset = header->file_type == MH_FILESET;
    MachoHeader64* header2    = NULL;
    uint8_t*       load_from2 = NULL;

    apple_boot_get_kc_bounds(header, NULL, &kc_base, &kc_end, NULL, NULL);

    cmd = (MachoLoadCommand*)(header + 1);
    if (!is_fileset) { apple_iboot_process_symbols(header, virt_slide); }
    for (i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        switch (cmd->cmd) {
            case LC_SEGMENT_64: {
                MachoSegmentCommand64* segCmd = (MachoSegmentCommand64*)cmd;

                if (strncmp(segCmd->segname, "__PAGEZERO", 10) == 0) { continue; }

                char   region_name[64];
                void*  load_from = (void*)(data + segCmd->vmaddr - kc_base);
                hwaddr load_to   = (phys_base + segCmd->vmaddr - kc_base);

                if (memory_map) {
                    snprintf(region_name, sizeof(region_name), "Kernel-%s", segCmd->segname);
                    struct MemoryMapFileInfo
                    {
                        uint64_t paddr;
                        uint64_t length;
                    } file_info = {load_to, segCmd->vmsize};
                    apple_dt_set_prop(memory_map, region_name, sizeof(file_info), &file_info);
                }
                else {
                    snprintf(region_name, sizeof(region_name), "TZ1-%s", segCmd->segname);
                }

                if (segCmd->vmsize == 0) { break; }

                if (!is_fileset) {
                    MachoSection64* sp;
                    for (sp = apple_iboot_first_sect(segCmd); sp != apple_iboot_end_sect(segCmd);
                         sp = apple_iboot_next_sect(sp))
                    {
                        if ((sp->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
                            load_from2 = (uint8_t*)(data + sp->addr - kc_base);
                            void** nl_symbol_ptr;
                            for (nl_symbol_ptr = (void**)load_from2; nl_symbol_ptr < (void**)(load_from2 + sp->size);
                                 nl_symbol_ptr++)
                            {
                                *nl_symbol_ptr += virt_slide;
                            }
                        }
                    }
                }

                if (!is_fileset) {
                    if (strcmp(segCmd->segname, "__TEXT") == 0) {
                        header2 = load_from;
                        MachoSegmentCommand64* seg;
                        assert_cmphex(header2->magic, ==, MACH_MAGIC_64);
                        for (seg = apple_iboot_get_first_seg(header2); seg != NULL;
                             seg = apple_iboot_get_next_seg(header2, seg))
                        {
                            MachoSection64* sp;
                            seg->vmaddr += virt_slide;
                            for (sp = apple_iboot_first_sect(seg); sp != apple_iboot_end_sect(seg);
                                 sp = apple_iboot_next_sect(sp))
                            {
                                sp->addr += virt_slide;
                            }
                        }
                    }
                }

                DINFO("Loading %s to 0x%" PRIx64 " (filesize: 0x%" PRIx64 " vmsize: 0x%" PRIx64 ")", region_name,
                      load_to, segCmd->filesize, segCmd->vmsize);
                uint8_t* buf = g_malloc0(segCmd->vmsize);
                memcpy(buf, load_from, segCmd->filesize);
                address_space_rw(as, load_to, MEMTXATTRS_UNSPECIFIED, buf, segCmd->vmsize, true);
                g_free(buf);

                if (!is_fileset) {
                    if (strcmp(segCmd->segname, "__TEXT") == 0) {
                        header2 = load_from;
                        MachoSegmentCommand64* seg;
                        assert_cmphex(header2->magic, ==, MACH_MAGIC_64);
                        for (seg = apple_iboot_get_first_seg(header2); seg != NULL;
                             seg = apple_iboot_get_next_seg(header2, seg))
                        {
                            MachoSection64* sp;
                            for (sp = apple_iboot_first_sect(seg); sp != apple_iboot_end_sect(seg);
                                 sp = apple_iboot_next_sect(sp))
                            {
                                sp->addr -= virt_slide;
                            }
                            // probably won't change anything, it's just for the
                            // symmetry
                            seg->vmaddr -= virt_slide;
                        }
                    }
                }

                if (!is_fileset) {
                    MachoSection64* sp;
                    for (sp = apple_iboot_first_sect(segCmd); sp != apple_iboot_end_sect(segCmd);
                         sp = apple_iboot_next_sect(sp))
                    {
                        if ((sp->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
                            load_from2 = (uint8_t*)(data + sp->addr - kc_base);
                            void** nl_symbol_ptr;
                            for (nl_symbol_ptr = (void**)load_from2; nl_symbol_ptr < (void**)(load_from2 + sp->size);
                                 nl_symbol_ptr++)
                            {
                                *nl_symbol_ptr -= virt_slide;
                            }
                        }
                    }
                }
                break;
            }
            case LC_UNIXTHREAD: {
                // grab just the entry point PC
                uint64_t* ptrPc = (uint64_t*)((char*)cmd + 0x110);

                // 0x110 for arm64 only.
                pc = vtop_bases(*ptrPc, phys_base, kc_base);

                break;
            }
            default: {
                break;
            }
        }
    }

    if (!is_fileset) { apple_iboot_process_symbols(header, -virt_slide); }

    return pc;
}

static void apple_iboot_read_file(const char* filename, uint8_t** data, gsize* len)
{
    if (!g_file_get_contents(filename, (gchar**)data, len, NULL)) {
        error_setg(&error_fatal, "file read for `%s` failed", filename);
    }
}

static bool apple_iboot_read_preboot(const char* path, uint8_t** data, gsize* len)
{
    BlockBackend*       blk       = blk_by_name("root");
    g_autofree uint8_t* active    = NULL;
    g_autofree char*    full_path = NULL;
    gsize               active_len;

    if (blk == NULL) { return false; }

    if (!apfs_read_file(blk, APFS_VOL_ROLE_PREBOOT, "active", &active, &active_len, NULL)) {
        return apfs_read_file(blk, APFS_VOL_ROLE_SYSTEM, path, data, len, NULL);
    }

    full_path = g_strdup_printf("%s/%s", (char*)active, path);
    return apfs_read_file(blk, APFS_VOL_ROLE_PREBOOT, full_path, data, len, NULL);
}

/// Read an image from the last DFU upload of type `dfu_type`, the file `filename` or the Preboot file `preboot_path`,
/// in this order, naming its source in `name`.
static bool apple_iboot_read_image(AppleBootInfo* info, const char* dfu_type, const char* dfu_name,
                                   const char* filename, const char* preboot_path, const char** name, uint8_t** data,
                                   gsize* len)
{
    if (info->dfu != NULL && apple_dfu_copy_image(info->dfu, dfu_type, data, len)) {
        *name = dfu_name;
        return true;
    }

    if (filename != NULL) {
        apple_iboot_read_file(filename, data, len);
        *name = filename;
        return true;
    }

    if (preboot_path != NULL && apple_iboot_read_preboot(preboot_path, data, len)) {
        *name = preboot_path;
        return true;
    }

    return false;
}

/// The manifest of the IMG4 at the start of the `firmware` NVMe namespace.
static bool apple_iboot_read_firmware_ticket(AppleBootInfo* info, gchar** ticket, gsize* length)
{
    NvmeNamespace*      firmware;
    int64_t             len;
    g_autofree uint8_t* buffer = NULL;

    firmware = NVME_NS(object_resolve_path_at(OBJECT(info->machine), "peripheral/firmware"));
    if (firmware == NULL) { return false; }

    len = blk_getlength(firmware->blkconf.blk);
    if (len <= 0) { return false; }

    buffer = g_malloc(len);

    blk_flush(firmware->blkconf.blk);
    blk_drain(firmware->blkconf.blk);

    if (blk_pread(firmware->blkconf.blk, 0, len, buffer, 0) < 0) {
        error_report("Failed to read the firmware");
        return false;
    }

    apple_boot_extract_manifest(buffer, len, ticket, length);
    return *ticket != NULL;
}

static const AppleIBootRegion* apple_iboot_find_region(const AppleBootInfo* info, AppleCarveoutID id)
{
    for (guint i = 0; i < info->regions->len; i++) {
        const AppleIBootRegion* region = &g_array_index(info->regions, AppleIBootRegion, i);

        if (region->id == id) { return region; }
    }

    return NULL;
}

hwaddr apple_iboot_carveout_base(const AppleBootInfo* info, AppleCarveoutID id)
{
    const AppleIBootRegion* region = apple_iboot_find_region(info, id);

    assert_nonnull(region);
    return region->base;
}

static hwaddr apple_iboot_carve_out(AppleBootInfo* info, AppleCarveoutID id, hwaddr size)
{
    AppleIBootRegion region;

    assert_cmphex(size, !=, 0);
    assert_null(apple_iboot_find_region(info, id));

    info->carveout_end = ROUND_DOWN(info->carveout_end - size, CARVEOUT_ALIGN);

    region.id   = id;
    region.base = info->carveout_end;
    region.size = size;
    g_array_append_val(info->regions, region);

    return region.base;
}

static void apple_iboot_record_carveouts(AppleBootInfo* info, AppleDTNode* root)
{
    AppleDTNode* node = apple_dt_get_node(root, "chosen/carveout-memory-map");
    char         name[32];
    uint64_t     reg[2];

    if (node == NULL) { return; }

    for (guint i = 0; i < info->regions->len; i++) {
        const AppleIBootRegion* region = &g_array_index(info->regions, AppleIBootRegion, i);

        snprintf(name, sizeof(name), "region-id-%u", region->id);
        reg[0] = region->base;
        reg[1] = region->size;
        apple_dt_set_prop(node, name, sizeof(reg), reg);
    }
}

static hwaddr apple_iboot_kernel_region_size(const AppleBootInfo* info)
{ return ROUND_DOWN(info->carveout_end - info->dram_base, CARVEOUT_ALIGN); }

static void apple_iboot_kaslr_slides(const AppleBootInfo* info, hwaddr* phys_slide_out, vaddr* virt_slide_out)
{
    static const size_t slide_granular      = (1 << 21);
    static const size_t slide_granular_mask = slide_granular - 1;
    static const size_t slide_virt_max      = 0x100 * (2 * 1024 * 1024);

    hwaddr   slide_phys;
    vaddr    slide_virt;
    uint64_t random_value = 0;

    qemu_guest_getrandom(&random_value, sizeof(random_value), NULL);

    if (info->kaslr_off) {
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

static bool apple_iboot_check_panic(AppleBootInfo* info)
{
    const AppleIBootRegion*              pram       = apple_iboot_find_region(info, APPLE_CARVEOUT_PRAM);
    g_autofree AppleEmbeddedPanicHeader* panic_info = g_malloc0(pram->size);

    address_space_rw(&address_space_memory, pram->base, MEMTXATTRS_UNSPECIFIED, panic_info, pram->size, false);
    address_space_set(&address_space_memory, pram->base, 0, pram->size, MEMTXATTRS_UNSPECIFIED);

    return panic_info->magic == EMBEDDED_PANIC_MAGIC;
}

static void apple_iboot_load_seprom(AppleBootInfo* info)
{
    g_autofree char*           seprom = NULL;
    g_autofree CKPatcherRange* range  = NULL;
    gsize                      size;

    if (info->seprom_base == 0 || info->seprom_filename == NULL) { return; }

    if (!g_file_get_contents(info->seprom_filename, &seprom, &size, NULL)) {
        error_setg(&error_fatal, "Could not load data from file '%s'", info->seprom_filename);
        return;
    }

    range = ck_patcher_range_from_ptr("seprom", (uint8_t*)seprom, size);
    ck_sep_seprom_patches(range);
    if (hwaccel_enabled()) { ck_patch_virt_unmanaged(range, info->seprom_base); }

    address_space_rw(&address_space_memory, info->seprom_base, MEMTXATTRS_UNSPECIFIED, (uint8_t*)seprom, size, true);
}

static void apple_iboot_setup_video(AppleBootInfo* info)
{
    const char*     cmdline = info->machine->kernel_cmdline;
    AppleVideoArgs* video   = &info->video_args;

    video->base_addr    = apple_iboot_carveout_base(info, APPLE_CARVEOUT_VRAM) + info->fb_offset;
    video->row_bytes    = info->disp_width * sizeof(uint32_t);
    video->width        = info->disp_width;
    video->height       = info->disp_height;
    video->depth.depth  = sizeof(uint32_t) * 8;
    video->depth.rotate = 1;
    video->display =
        !apple_iboot_contains_boot_arg(cmdline, "-s", false) && !apple_iboot_contains_boot_arg(cmdline, "-v", false);
}

static void apple_iboot_draw_splash(AppleIBootSplash* splash)
{
    AppleBootInfo* info = splash->info;
    hwaddr         size = (hwaddr)info->disp_height * info->disp_width * sizeof(uint32_t);
    hwaddr         len  = size;
    InfernoPixbuf  disp_image;
    void*          fb;

    fb = address_space_map(&address_space_memory, info->video_args.base_addr, &len, true, MEMTXATTRS_UNSPECIFIED);
    if (fb == NULL) { return; }

    if (len != size) {
        address_space_unmap(&address_space_memory, fb, len, true, 0);
        return;
    }

    disp_image = (InfernoPixbuf){
        .plane  = {fb},
        .stride = {info->disp_width * sizeof(uint32_t)},
        .width  = info->disp_width,
        .height = info->disp_height,
        .kind   = INFERNO_PIXBUF_ARGB,
    };
    inferno_pixbuf_composite(&disp_image, &splash->image, splash->dest_x, splash->dest_y, false);

    address_space_unmap(&address_space_memory, fb, len, true, len);
}

static void apple_iboot_free_splash(AppleIBootSplash* splash)
{
    timer_free(splash->timer);
    inferno_pixbuf_destroy(&splash->image);
    splash->info->splash = NULL;
    g_free(splash);
}

static void apple_iboot_draw_splash_timer(void* opaque)
{
    AppleIBootSplash* splash = opaque;

    apple_iboot_draw_splash(splash);
    apple_iboot_free_splash(splash);
}

static FILE* apple_iboot_open_splash(const char* name, char** path)
{
    g_autofree char* icon = g_strconcat(SPLASH_DIR, name, NULL);

    *path = get_relocated_path(icon);
    assert_nonnull(*path);
    return fopen(*path, "rb");
}

// Please see `ui/icons/CKBrandingNotice.md`
static void apple_iboot_show_splash(AppleBootInfo* info, const char* name)
{
    static const uint8_t rgba_to_argb[4] = {2, 1, 0, 3};
    g_autofree char*     path            = NULL;
    FILE*                fp;
    uint8_t              sig[8] = {0};
    png_structp          png_ptr;
    png_infop            info_ptr;
    AppleIBootSplash*    splash;
    InfernoPixbuf        png;
    png_bytep*           row_ptrs;
    double               dest_width;

    if (info->splash != NULL) { apple_iboot_free_splash(info->splash); }

    fp = apple_iboot_open_splash(name, &path);
    if (fp == NULL && strcmp(name, SPLASH_BOOT) != 0) {
        g_free(path);
        fp = apple_iboot_open_splash(SPLASH_BOOT, &path);
    }
    if (fp == NULL) {
        error_setg(&error_abort, "Missing emulator branding: %s.", path);
        return;
    }
    fread(sig, sizeof(sig), 1, fp);
    if (png_sig_cmp(sig, 0, sizeof(sig)) != 0) {
        error_setg(&error_abort, "Invalid emulator branding: %s.", path);
        return;
    }
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert_nonnull(png_ptr);
    info_ptr = png_create_info_struct(png_ptr);
    assert_nonnull(info_ptr);
    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, sizeof(sig));

    png_read_info(png_ptr, info_ptr);
    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png_ptr, info_ptr);

    inferno_pixbuf_init(&png, INFERNO_PIXBUF_ARGB, png_get_image_width(png_ptr, info_ptr),
                        png_get_image_height(png_ptr, info_ptr));

    row_ptrs = g_new(png_bytep, png.height);
    for (size_t y = 0; y < png.height; y++) { row_ptrs[y] = png.plane[0] + y * png.stride[0]; }

    png_read_image(png_ptr, row_ptrs);

    g_free(row_ptrs);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    inferno_pixbuf_shuffle(&png, rgba_to_argb);

    dest_width     = info->disp_width / 1.25;
    splash         = g_new(AppleIBootSplash, 1);
    splash->info   = info;
    splash->dest_x = info->disp_width / 2 - dest_width / 2;
    splash->dest_y = info->disp_height / 2 - dest_width / 2;
    inferno_pixbuf_scale(&splash->image, &png, dest_width, dest_width);
    inferno_pixbuf_destroy(&png);

    apple_iboot_draw_splash(splash);

    splash->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_iboot_draw_splash_timer, splash);
    info->splash  = splash;

    // Workaround for `-v` removing the boot splash.
    timer_mod(splash->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (NANOSECONDS_PER_SECOND / 2));
}

/* Mark the DART page table regions as PPL-protected, as `_t8020dart_init` requires. */
static void apple_iboot_protect_dart_tables(AppleDTNode* root, const uint64_t (*regions)[2], uint32_t count)
{
    AppleDTNode* defaults;
    AppleDTProp* prop;
    uint8_t*     ranges;
    uint32_t     len;
    uint32_t     i;

    defaults = apple_dt_get_node(root, "defaults");
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
static hwaddr apple_iboot_fb_size(const AppleBootInfo* info)
{ return ROUND_UP((hwaddr)info->video_args.row_bytes * info->video_args.height, DISPLAY_DART_PAGE_SIZE); }

/* Map the framebuffer and leave the display scanning it out while waiting in DFU Mode. */
static void apple_iboot_dfu_display(AppleBootInfo* info)
{
    hwaddr tables = apple_iboot_carve_out(info, APPLE_CARVEOUT_DISP_DART, 2 * DISPLAY_DART_PAGE_SIZE);

    apple_dart_map_boot_range(info->disp_dart, info->disp_sid, tables, DISPLAY_FB_IOVA, info->video_args.base_addr,
                              apple_iboot_fb_size(info));
    adp_v4_set_boot_fb(info->disp, DISPLAY_FB_IOVA);
}

static void apple_iboot_dfu_attached(void* opaque, bool recovery)
{ apple_iboot_show_splash(opaque, recovery ? SPLASH_RECOVERY : SPLASH_DFU); }

static void apple_iboot_boot_display(AppleBootInfo* info, AppleIBootOS* os)
{
    AppleDTNode* node;
    uint32_t     fb_sid = info->disp_sid;
    uint32_t     sids;
    uint32_t     sid;
    hwaddr       fb_size;
    hwaddr       size;
    hwaddr       tables;
    uint64_t     pt_regions[32][2];
    uint32_t     count;
    uint64_t     l2_tt[2];
    char         name[16];

    node = apple_dt_get_node(os->device_tree, "arm-io/dart-disp0");
    assert_nonnull(node);
    // Allow persistent mappings.
    apple_dt_set_prop_u32(node, "dart-options", apple_dt_get_prop_u32(node, "dart-options", &error_abort) | BIT32(6));

    fb_size = apple_iboot_fb_size(info);

    // XNU requires persistent tables for every translated SID once any SID has them.
    sids = apple_dt_get_prop_u32(node, "sids", &error_abort) & ~apple_dt_get_prop_u32(node, "bypass", &error_abort);
    assert_cmphex(sids & BIT32(fb_sid), !=, 0);

    // An L1 table for each SID, plus the L2 table of the framebuffer's.
    size   = (ctpop32(sids) + 1) * DISPLAY_DART_PAGE_SIZE;
    tables = apple_iboot_carve_out(info, APPLE_CARVEOUT_DISP_DART, size);

    count = 0;
    for (sid = 0; sid < 32; sid++) {
        if ((sids & BIT32(sid)) == 0) { continue; }

        size = sid == fb_sid ? 2 * DISPLAY_DART_PAGE_SIZE : DISPLAY_DART_PAGE_SIZE;
        apple_dart_map_boot_range(info->disp_dart, sid, tables, DISPLAY_FB_IOVA, info->video_args.base_addr,
                                  sid == fb_sid ? fb_size : 0);

        pt_regions[count][0] = tables;
        pt_regions[count][1] = tables + size;
        snprintf(name, sizeof(name), "pt-region-%u", sid);
        apple_dt_set_prop(node, name, sizeof(pt_regions[count]), pt_regions[count]);
        count++;
        l2_tt[0] = tables;
        // iOS 15 reads the u32 after the address as the table's level, iOS 14 requires it to be 0.
        l2_tt[1] = BUILD_VERSION_MAJOR(os->build_version) >= 15 ? 2 : 0;
        snprintf(name, sizeof(name), "l2-tt-%u", sid);
        apple_dt_set_prop(node, name, sizeof(l2_tt), l2_tt);

        tables += size;
    }

    apple_iboot_protect_dart_tables(os->device_tree, pt_regions, count);

    adp_v4_set_boot_fb(info->disp, DISPLAY_FB_IOVA);
}

static void apple_iboot_boot_xnu(AppleBootInfo* info, AppleIBootOS* os)
{
    MachineState*           machine   = info->machine;
    const AppleIBootRegion* tz1       = apple_iboot_find_region(info, APPLE_CARVEOUT_AP_TRUSTZONE);
    hwaddr                  phys_base = info->dram_base;
    vaddr                   text_base;
    vaddr                   kc_base;
    vaddr                   kc_end;
    vaddr                   ro_lower;
    vaddr                   ro_upper;
    hwaddr                  phys_slide;
    vaddr                   virt_slide;
    vaddr                   virt_base;
    vaddr                   dt_va;
    hwaddr                  kern_entry;
    hwaddr                  phys_ptr;
    hwaddr                  mem_size;

    apple_boot_get_kc_bounds(os->kernel, &text_base, &kc_base, &kc_end, &ro_lower, &ro_upper);

    apple_iboot_kaslr_slides(info, &phys_slide, &virt_slide);

    virt_base = kc_base + (virt_slide - phys_slide);

    os->trustcache_addr = vtop_bases(ro_lower + virt_slide, phys_base, virt_base) - os->trustcache_size;
    info_report("TrustCache Physical Base: 0x" HWADDR_FMT_plx, os->trustcache_addr);

    address_space_rw(&address_space_memory, os->trustcache_addr, MEMTXATTRS_UNSPECIFIED, os->trustcache,
                     os->trustcache_size, true);

    kern_entry = apple_iboot_load_macho(os->kernel, &address_space_memory,
                                        apple_dt_get_node(os->device_tree, "/chosen/memory-map"),
                                        phys_base + phys_slide, virt_slide);

    info_report("Kernel Virtual Base: 0x%016" VADDR_PRIx, virt_base);
    info_report("Kernel Physical Base: 0x" HWADDR_FMT_plx, phys_base);
    info_report("Kernel Virtual Slide: 0x%016" VADDR_PRIx, virt_slide);
    info_report("Kernel Physical Slide: 0x" HWADDR_FMT_plx, phys_slide);
    info_report("Kernel Entry Point: 0x" HWADDR_FMT_plx, kern_entry);

    phys_ptr = vtop_bases(ROUND_UP_16K(kc_end + virt_slide), phys_base, virt_base);

    if (os->ramdisk != NULL) {
        os->ramdisk_addr = phys_ptr;
        address_space_rw(&address_space_memory, os->ramdisk_addr, MEMTXATTRS_UNSPECIFIED, os->ramdisk, os->ramdisk_size,
                         true);
        os->ramdisk_size  = ROUND_UP_16K(os->ramdisk_size);
        phys_ptr         += os->ramdisk_size;
    }

    if (info->tz0_size != 0) {
        os->tz0_size = info->tz0_size;
        phys_ptr     = ROUND_UP(phys_ptr, info->tz0_align);

        os->tz0_addr  = phys_ptr;
        phys_ptr     += os->tz0_size;
        info_report("TZ0 Physical Base: 0x" HWADDR_FMT_plx, os->tz0_addr);
    }

    os->sep_fw_addr = phys_ptr;
    os->sep_fw_size = info->sepfw_size;
    info_report("SEPFW Physical Base: 0x" HWADDR_FMT_plx, os->sep_fw_addr);
    info_report("SEPFW Size: 0x" HWADDR_FMT_plx, os->sep_fw_size);
    phys_ptr += os->sep_fw_size;

    os->boot_args_addr  = phys_ptr;
    os->boot_args_size  = BOOT_ARGS_SIZE;
    phys_ptr           += os->boot_args_size;

    os->device_tree_addr  = phys_ptr;
    dt_va                 = ptov_bases(os->device_tree_addr, phys_base, virt_base);
    phys_ptr             += os->device_tree_size;
    info_report("Device Tree Physical Base: 0x" HWADDR_FMT_plx, os->device_tree_addr);
    info_report("Device Tree Virtual Base: 0x%016" VADDR_PRIx, dt_va);
    info_report("Device Tree Size: 0x%" PRIx64, os->device_tree_size);

    os->top_of_kernel_data = ROUND_UP_16K(phys_ptr);
    info_report("Top Of Kernel Data: 0x" HWADDR_FMT_plx, os->top_of_kernel_data);

    apple_iboot_finalise_dt(os);

    mem_size = apple_iboot_kernel_region_size(info);
    info_report("Memory Size: 0x" HWADDR_FMT_plx, mem_size);

    apple_iboot_setup_bootargs(os->build_version, &address_space_memory, os->boot_args_addr, virt_base, phys_base,
                               mem_size, os->top_of_kernel_data, dt_va, os->device_tree_size, &info->video_args,
                               os->cmdline, machine->ram_size);

    if (tz1 != NULL) {
        vaddr  tz1_virt_low;
        vaddr  tz1_virt_high;
        hwaddr tz1_entry;
        hwaddr tz1_boot_args;

        if (os->secure_monitor == NULL) {
            error_setg(&error_fatal, "The kernelcache has no secure monitor");
            return;
        }

        apple_boot_get_kc_bounds(os->secure_monitor, NULL, &tz1_virt_low, &tz1_virt_high, NULL, NULL);
        info_report("TrustZone 1 virtual address low: 0x%016" VADDR_PRIx, tz1_virt_low);
        info_report("TrustZone 1 virtual address high: 0x%016" VADDR_PRIx, tz1_virt_high);
        tz1_entry = apple_iboot_load_macho(os->secure_monitor, &address_space_memory, NULL, tz1->base, 0);
        info_report("TrustZone 1 entry: 0x" HWADDR_FMT_plx, tz1_entry);
        tz1_boot_args = tz1->base + (tz1->size - sizeof(AppleMonitorBootArgs));
        info_report("TrustZone 1 boot args address: 0x" HWADDR_FMT_plx, tz1_boot_args);
        apple_iboot_setup_monitor_boot_args(&address_space_memory, tz1_boot_args, tz1_virt_low, tz1->base, tz1->size,
                                            os->boot_args_addr, kern_entry, phys_base, phys_slide, virt_slide,
                                            text_base - kc_base);

        info->entry     = tz1_entry;
        info->entry_arg = tz1_boot_args;
        info->rvbar     = tz1->base;
    }
    else {
        info->entry     = kern_entry;
        info->entry_arg = os->boot_args_addr;
        info->rvbar     = kern_entry & ~0xFFFULL;
    }

    if (info->lock_regions != NULL) {
        info->lock_regions(machine, os->trustcache_addr, vtop_bases(ro_upper + virt_slide, phys_base, virt_base), true,
                           os->tz0_addr, os->tz0_size);
    }
}

// The collection's own segments; `apple_boot_get_segment` looks into `com.apple.kernel` instead.
static MachoSegmentCommand64* apple_iboot_sptm_get_segment(MachoHeader64* header, const char* name)
{
    MachoLoadCommand* cmd = (MachoLoadCommand*)(header + 1);

    for (uint32_t i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        MachoSegmentCommand64* seg = (MachoSegmentCommand64*)cmd;

        if (cmd->cmd == LC_SEGMENT_64 && strncmp(seg->segname, name, sizeof(seg->segname)) == 0) { return seg; }
    }

    return NULL;
}

static hwaddr apple_iboot_sptm_segment_size(MachoHeader64* header, const char* name)
{
    MachoSegmentCommand64* seg = apple_iboot_sptm_get_segment(header, name);
    return seg == NULL ? 0 : ROUND_UP_16K(seg->vmsize);
}

static hwaddr apple_iboot_sptm_load_segment(MachoHeader64* header, const char* name, hwaddr pa)
{
    MachoSegmentCommand64* seg = apple_iboot_sptm_get_segment(header, name);
    vaddr                  base;

    if (seg == NULL || seg->vmsize == 0) { return pa; }

    apple_boot_get_kc_bounds(header, NULL, &base, NULL, NULL, NULL);
    address_space_rw(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                     apple_boot_get_macho_buffer(header) + seg->vmaddr - base, seg->vmsize, true);
    return pa + ROUND_UP_16K(seg->vmsize);
}

static void apple_iboot_sptm_set_region(AppleDTNode* memory_map, const char* name, uint64_t addr, uint64_t size)
{
    AppleDTProp* prop = apple_dt_get_prop(memory_map, name);

    assert_nonnull(prop);
    stq_le_p(prop->data, addr);
    stq_le_p(prop->data + sizeof(uint64_t), size);
}

// iBoot numbers the DARTs SPTM manages.
static void apple_iboot_sptm_assign_dart_ids(AppleDTNode* root)
{
    AppleDTNode* arm_io = apple_dt_get_node(root, "arm-io");
    uint32_t     id     = 0;

    assert_nonnull(arm_io);
    for (GList* l = arm_io->children; l != NULL; l = l->next) {
        AppleDTNode* node   = l->data;
        AppleDTProp* compat = apple_dt_get_prop(node, "compatible");

        if (compat != NULL && g_str_has_prefix(compat->data, "dart,")) { apple_dt_set_prop_u32(node, "dart-id", id++); }
    }
}

// SPTM checks that the regions from `TXM-ro` to `SPTM-rw` are contiguous and in this order.
static void apple_iboot_boot_sptm(AppleBootInfo* info, AppleIBootOS* os)
{
    MachineState*  machine    = info->machine;
    AppleDTNode*   memory_map = apple_dt_get_node(os->device_tree, "/chosen/memory-map");
    MachoHeader64* kc         = os->kernel;
    MachoHeader64* txm        = os->txm;
    MachoHeader64* sptm       = os->sptm;
    hwaddr         phys_base  = info->dram_base;
    vaddr          virt_base;
    vaddr          kc_text;
    vaddr          txm_text;
    vaddr          sptm_text;
    hwaddr         region;
    hwaddr         phys_ptr;
    hwaddr         ro_lower;
    hwaddr         ro_upper;
    hwaddr         sptm_base;
    hwaddr         before_sptm;
    hwaddr         kern_entry;
    hwaddr         mem_size;
    vaddr          dt_va;

    apple_boot_get_kc_bounds(kc, &kc_text, NULL, NULL, NULL, NULL);
    apple_boot_get_kc_bounds(txm, &txm_text, NULL, NULL, NULL, NULL);
    apple_boot_get_kc_bounds(sptm, &sptm_text, NULL, NULL, NULL, NULL);

    // SPTM maps itself at `virtBase + (SPTM-ro - physBase)`, and TXM and the kernel collection 1 and 2 strides above.
    // Placing SPTM-ro at the kernel collection's offset in its 64 GiB block keeps that slide page-table aligned.
    // TXM writes a spare page in the `DeviceTree` region.
    os->device_tree_size += 16 * KiB;

    before_sptm =
        apple_iboot_sptm_segment_size(txm, "__TEXT") + apple_iboot_sptm_segment_size(txm, "__DATA_CONST")
        + apple_iboot_sptm_segment_size(txm, "__TEXT_EXEC") + apple_iboot_sptm_segment_size(txm, "__TEXT_BOOT_EXEC")
        + os->trustcache_size + apple_iboot_sptm_segment_size(kc, "__TEXT_EXEC")
        + apple_iboot_sptm_segment_size(kc, "__TEXT_BOOT_EXEC") + apple_iboot_sptm_segment_size(kc, "__TEXT")
        + apple_iboot_sptm_segment_size(kc, "__PRELINK_TEXT") + apple_iboot_sptm_segment_size(kc, "__DATA_CONST")
        + apple_iboot_sptm_segment_size(kc, "__DATA_SPTM") + os->device_tree_size;
    assert_cmphex(extract64(kc_text, 0, 36), >, before_sptm);

    virt_base = kc_text & ~MAKE_64BIT_MASK(0, 36);
    phys_ptr  = phys_base + extract64(kc_text, 0, 36) - before_sptm;
    ro_lower  = phys_ptr;

    // The memory skipped for that is the physical slide, which XNU frees.
    apple_iboot_sptm_set_region(memory_map, "slide", phys_base, phys_ptr - phys_base);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(txm, "__TEXT", phys_ptr);
    phys_ptr = apple_iboot_sptm_load_segment(txm, "__DATA_CONST", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "TXM-ro", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(txm, "__TEXT_EXEC", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "TXM-rx", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(txm, "__TEXT_BOOT_EXEC", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "TXM-bx", region, phys_ptr - region);

    os->trustcache_addr = phys_ptr;
    address_space_rw(&address_space_memory, os->trustcache_addr, MEMTXATTRS_UNSPECIFIED, os->trustcache,
                     os->trustcache_size, true);
    phys_ptr += os->trustcache_size;

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__TEXT_EXEC", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "BootKC-rx", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__TEXT_BOOT_EXEC", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "BootKC-bx", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__TEXT", phys_ptr);
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__PRELINK_TEXT", phys_ptr);
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__DATA_CONST", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "BootKC-ro", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__DATA_SPTM", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "BootKC-rs", region, phys_ptr - region);

    os->device_tree_addr  = phys_ptr;
    phys_ptr             += os->device_tree_size;

    sptm_base = phys_ptr;
    phys_ptr  = apple_iboot_sptm_load_segment(sptm, "__TEXT", phys_ptr);
    phys_ptr  = apple_iboot_sptm_load_segment(sptm, "__DATA_CONST", phys_ptr);
    phys_ptr  = apple_iboot_sptm_load_segment(sptm, "__LATE_CONST", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "SPTM-ro", sptm_base, phys_ptr - sptm_base);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(sptm, "__TEXT_EXEC", phys_ptr);
    phys_ptr = apple_iboot_sptm_load_segment(sptm, "__LAST", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "SPTM-rx", region, phys_ptr - region);
    ro_upper = phys_ptr;

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(sptm, "__DATA", phys_ptr);
    phys_ptr = apple_iboot_sptm_load_segment(sptm, "__BOOTDATA", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "SPTM-rw", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(sptm, "__LINKEDIT", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "SPTM-le", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(txm, "__DATA", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "TXM-rw", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(txm, "__LINKEDIT", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "TXM-le", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__PRELINK_INFO", phys_ptr);
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__DATA", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "BootKC-rw", region, phys_ptr - region);

    region   = phys_ptr;
    phys_ptr = apple_iboot_sptm_load_segment(kc, "__LINKEDIT", phys_ptr);
    apple_iboot_sptm_set_region(memory_map, "BootKC-le", region, phys_ptr - region);

    // No exclaves.
    apple_iboot_sptm_set_region(memory_map, "CL4-rx", -1, 0);
    apple_iboot_sptm_set_region(memory_map, "CL4-ro", -1, 0);
    apple_iboot_sptm_set_region(memory_map, "CL4-dummypage", phys_ptr, 16 * KiB);
    phys_ptr += 16 * KiB;

    apple_iboot_sptm_set_region(memory_map, "TXM-virt", txm_text, 0);
    apple_iboot_sptm_set_region(memory_map, "TXM-entry", apple_boot_get_entry_point(txm), 0);
    apple_iboot_sptm_set_region(memory_map, "BootKC-virt", kc_text, 0);
    apple_iboot_sptm_set_region(memory_map, "BootKC-entry", apple_boot_get_entry_point(kc), 0);
    apple_iboot_sptm_set_region(memory_map, "SPTM-virt", sptm_text, 0);
    apple_iboot_sptm_set_region(memory_map, "SPTM-entry", apple_boot_get_entry_point(sptm), 0);

    kern_entry = sptm_base + apple_boot_get_entry_point(sptm) - sptm_text;
    info_report("SPTM Physical Base: 0x" HWADDR_FMT_plx, sptm_base);
    info_report("SPTM Entry Point: 0x" HWADDR_FMT_plx, kern_entry);

    os->boot_args_addr  = phys_ptr;
    os->boot_args_size  = BOOT_ARGS_SIZE;
    phys_ptr           += os->boot_args_size;

    if (os->ramdisk != NULL) {
        os->ramdisk_addr = phys_ptr;
        address_space_rw(&address_space_memory, os->ramdisk_addr, MEMTXATTRS_UNSPECIFIED, os->ramdisk, os->ramdisk_size,
                         true);
        os->ramdisk_size  = ROUND_UP_16K(os->ramdisk_size);
        phys_ptr         += os->ramdisk_size;
    }

    os->sep_fw_addr  = phys_ptr;
    os->sep_fw_size  = info->sepfw_size;
    phys_ptr        += os->sep_fw_size;
    info_report("SEPFW Physical Base: 0x" HWADDR_FMT_plx, os->sep_fw_addr);

    os->top_of_kernel_data = ROUND_UP_16K(phys_ptr);
    info_report("Top Of Kernel Data: 0x" HWADDR_FMT_plx, os->top_of_kernel_data);

    dt_va = ptov_bases(os->device_tree_addr, phys_base, virt_base);
    apple_iboot_finalise_dt(os);

    mem_size = apple_iboot_kernel_region_size(info);
    info_report("Memory Size: 0x" HWADDR_FMT_plx, mem_size);

    apple_iboot_setup_bootargs(os->build_version, &address_space_memory, os->boot_args_addr, virt_base, phys_base,
                               mem_size, os->top_of_kernel_data, dt_va, os->device_tree_size, &info->video_args,
                               os->cmdline, machine->ram_size);

    info->entry     = kern_entry;
    info->entry_arg = os->boot_args_addr;
    info->rvbar     = kern_entry & ~0xFFFULL;

    // SPTM locks the read-only region itself.
    if (info->lock_regions != NULL) {
        info->lock_regions(machine, ro_lower, ro_upper, false, os->tz0_addr, os->tz0_size);
    }
}

static void apple_iboot_load_images(AppleBootInfo* info, AppleIBootOS* os)
{
    MachineState* machine = info->machine;
    AppleDTNode*  chosen;
    const char*   name;
    uint8_t*      data;
    gsize         len;
    vaddr         kc_base;
    vaddr         kc_end;

    if (!apple_iboot_read_image(info, "rdtr", "DFU device tree", machine->dtb, PREBOOT_DEVICETREE, &name, &data, &len))
    {
        error_setg(&error_fatal, "No device tree to boot");
        return;
    }
    os->device_tree = apple_boot_load_dt_data(name, data, len);
    chosen          = apple_dt_get_node(os->device_tree, "chosen");
    assert_nonnull(chosen);

    if (info->dfu == NULL || !apple_dfu_copy_ticket(info->dfu, &os->ticket, &os->ticket_length)) {
        if (!apple_iboot_read_preboot(PREBOOT_APTICKET, (uint8_t**)&os->ticket, &os->ticket_length)) {
            apple_iboot_read_firmware_ticket(info, &os->ticket, &os->ticket_length);
        }
        if (apple_iboot_read_preboot(PREBOOT_ROOT_HASH, &data, &len)) {
            apple_boot_load_root_hash_data(chosen, PREBOOT_ROOT_HASH, data, len);
        }
    }

    if (!apple_iboot_read_image(info, "rkrn", "DFU kernelcache", machine->kernel_filename, PREBOOT_KERNELCACHE, &name,
                                &data, &len))
    {
        error_setg(&error_fatal, "No kernel to boot");
        return;
    }
    os->kernel = apple_boot_load_kernel_data(name, data, len, &os->secure_monitor);

    os->build_version = apple_boot_build_version(os->kernel);
    info_report("%s %u.%u.%u", apple_boot_platform_string(os->kernel), BUILD_VERSION_MAJOR(os->build_version),
                BUILD_VERSION_MINOR(os->build_version), BUILD_VERSION_PATCH(os->build_version));

    apple_boot_get_kc_bounds(os->kernel, NULL, &kc_base, &kc_end, NULL, NULL);
    info_report("Kernel virtual low: 0x%016" VADDR_PRIx, kc_base);
    info_report("Kernel virtual high: 0x%016" VADDR_PRIx, kc_end);

    ck_patch_kernel(os->kernel);
    if (info->patch_virt) {
        ck_patch_virt(os->kernel);
        ck_patch_virt_monitor(os->kernel);
    }

    if (apple_iboot_read_image(info, "rtsc", "DFU trust cache", info->trustcache_filename, PREBOOT_TRUSTCACHE, &name,
                               &data, &len))
    {
        os->trustcache = apple_boot_load_trustcache_data(name, data, len, &os->trustcache_size);
    }

    if (apple_boot_get_segment(os->kernel, "__DATA_SPTM") != NULL) {
        if (!apple_iboot_read_image(info, "rspt", "DFU SPTM", NULL, PREBOOT_SPTM, &name, &data, &len)) {
            error_setg(&error_fatal, "This kernelcache needs SPTM and TXM");
            return;
        }
        os->sptm = apple_boot_load_monitor_data(name, data, len, "sptm", "rspt");
        if (info->patch_virt) { ck_patch_virt_monitor(os->sptm); }

        if (!apple_iboot_read_image(info, "rtrx", "DFU TXM", NULL, PREBOOT_TXM, &name, &data, &len)) {
            error_setg(&error_fatal, "This kernelcache needs SPTM and TXM");
            return;
        }
        os->txm = apple_boot_load_monitor_data(name, data, len, "trxm", "rtrx");
        ck_patch_txm(os->txm);
        if (info->patch_virt) { ck_patch_virt_monitor(os->txm); }
    }

    if (apple_iboot_read_image(info, "rdsk", "DFU ramdisk", machine->initrd_filename, NULL, &name, &data, &len)) {
        os->ramdisk = apple_boot_load_ramdisk_data(name, data, len, &os->ramdisk_size);
    }
    else if (!os->auto_boot) {
        error_setg(&error_fatal, "No RAM disk to boot recovery from");
    }
}

static void apple_iboot_free_os(AppleIBootOS* os)
{
    if (os->device_tree != NULL) { apple_dt_destroy(os->device_tree); }
    if (os->kernel != NULL) { g_free(apple_boot_get_macho_buffer(os->kernel)); }
    if (os->sptm != NULL) { g_free(apple_boot_get_macho_buffer(os->sptm)); }
    if (os->txm != NULL) { g_free(apple_boot_get_macho_buffer(os->txm)); }
    g_free(os->secure_monitor);
    g_free(os->trustcache);
    g_free(os->ramdisk);
    g_free(os->ticket);
}

static void apple_iboot_boot_os(AppleBootInfo* info, AppleIBootOS* os)
{
    AppleDTNode* root;
    AppleDTNode* chosen;
    AppleDTNode* memory_map;
    AppleDTNode* node;
    uint64_t     reg[2];
    bool         use_sptm;

    apple_iboot_load_images(info, os);

    root   = os->device_tree;
    chosen = apple_dt_get_node(root, "chosen");
    assert_nonnull(chosen);
    memory_map = apple_dt_get_node(root, "chosen/memory-map");
    assert_nonnull(memory_map);

    if (info->update_device_tree != NULL) { info->update_device_tree(info->machine, root); }

    if (!apple_iboot_contains_boot_arg(os->cmdline, "rd=", true)) {
        apple_dt_set_prop_strn(chosen, "root-matching", 256,
                               "<dict><key>IOProviderClass</key><string>IOMedia</"
                               "string><key>IOPropertyMatch</key><dict><key>Partition "
                               "ID</key><integer>1</integer></dict></dict>");
    }

    if (os->has_security_mode_change_enable) {
        apple_dt_set_prop_u32(chosen, "security-mode-change-enable", os->security_mode_change_enable);
    }

    node = apple_dt_get_node(root, "pram");
    if (node != NULL) {
        const AppleIBootRegion* pram = apple_iboot_find_region(info, APPLE_CARVEOUT_PRAM);

        reg[0] = pram->base;
        reg[1] = pram->size;
        apple_dt_set_prop(node, "reg", sizeof(reg), reg);
        apple_dt_set_prop_u64(chosen, "embedded-panic-log-size", pram->size);
    }

    node = apple_dt_get_node(root, "vram");
    if (node != NULL) {
        const AppleIBootRegion* vram = apple_iboot_find_region(info, APPLE_CARVEOUT_VRAM);

        reg[0] = vram->base;
        reg[1] = vram->size;
        apple_dt_set_prop(node, "reg", sizeof(reg), reg);
    }

    if (info->disp_dart != NULL) { apple_iboot_boot_display(info, os); }

    use_sptm = os->sptm != NULL;
    if (use_sptm) {
        apple_iboot_allocate_sptm_records(memory_map);
        apple_iboot_sptm_assign_dart_ids(root);

        // SPTM types every frame below the top of kernel data, so TZ0 is carved out like on hardware.
        if (info->tz0_size != 0) {
            os->tz0_size = info->tz0_size;
            os->tz0_addr = ROUND_UP(apple_iboot_carve_out(info, APPLE_CARVEOUT_SEPFW, info->tz0_size + info->tz0_align),
                                    info->tz0_align);
            info_report("TZ0 Physical Base: 0x" HWADDR_FMT_plx, os->tz0_addr);
        }
    }
    else {
        apple_iboot_allocate_segment_records(memory_map, os->kernel);
    }

    apple_iboot_record_carveouts(info, root);

    apple_iboot_populate_dt(info, os);

    switch (os->kernel->file_type) {
        case MH_EXECUTE:
        case MH_FILESET:
            if (use_sptm) { apple_iboot_boot_sptm(info, os); }
            else {
                apple_iboot_boot_xnu(info, os);
            }
            break;
        default: error_setg(&error_fatal, "Unsupported kernelcache type: 0x%X\n", os->kernel->file_type); return;
    }

    info->build_version = os->build_version;
}

static void apple_iboot_setup(AppleBootInfo* info)
{
    MachineState*       machine    = info->machine;
    g_autofree char*    cmdline    = NULL;
    g_autofree uint8_t* nvram_data = NULL;
    AppleIBootOS        os         = {0};
    AppleNvram*         nvram;
    bool                have_autoboot;

    g_array_set_size(info->regions, info->num_static_regions);
    info->carveout_end = info->static_end;

    // Only a booting kernel or DFU Mode gets the display as iBoot leaves it, see `apple_iboot_boot_display`.
    if (info->disp_dart != NULL) {
        apple_dart_clear_boot_ranges(info->disp_dart);
        adp_v4_clear_boot_fb(info->disp);
    }

    if (apple_iboot_check_panic(info)) {
        qemu_system_guest_panicked(NULL);
        return;
    }

    apple_iboot_load_seprom(info);

    nvram = APPLE_NVRAM(object_resolve_path_at(OBJECT(machine), "peripheral/nvram"));
    if (nvram == NULL) {
        error_setg(&error_fatal, "Failed to find NVRAM device");
        return;
    }
    apple_nvram_load(nvram);

    info_report("Boot Mode: %u", info->boot_mode);
    switch (info->boot_mode) {
        case kAppleBootModeEnterRecovery: env_set_bool(nvram, "auto-boot", false, 0); break;
        case kAppleBootModeExitRecovery : env_set_bool(nvram, "auto-boot", true, 0); break;
        case kAppleBootModeDFU          : env_unset(nvram, "auto-boot"); break;
        default                         : break;
    }
    info->boot_mode = kAppleBootModeAuto;

    have_autoboot = env_find(nvram, "auto-boot") != NULL;
    os.auto_boot  = env_get_bool(nvram, "auto-boot", false);

    if (info->non_cold_boot && (!info->had_autoboot && have_autoboot && os.auto_boot)) {
        fprintf(stdout, "\n--------\nPotentially-completed restore process.\nPlease check the restore status, and "
                        "continue forth with the filesystem patches if success is observed.\n--------\n");
        exit(0);
        return;
    }

    info->had_autoboot  = have_autoboot;
    info->non_cold_boot = true;

    info_report("Auto Boot: %s", os.auto_boot ? "true" : "false");

    cmdline = g_strconcat(info->boot_args != NULL ? info->boot_args : "",
                          os.auto_boot ? "" : "-restore rd=md0 nand-enable-reformat=1 ", machine->kernel_cmdline, NULL);
    os.cmdline = cmdline;

    info_report("Boot Args: [%s]", cmdline);

    if (env_find(nvram, "security-mode-change-enable") != NULL) {
        os.has_security_mode_change_enable = true;
        os.security_mode_change_enable     = env_get_uint(nvram, "security-mode-change-enable", 0);
        env_unset(nvram, "security-mode-change-enable");
    }

    apple_iboot_setup_boot_nonce(nvram, info);

    apple_nvram_save(nvram);

    nvram_data    = g_malloc0(XNU_MAX_NVRAM_SIZE);
    os.nvram      = nvram_data;
    os.nvram_size = MIN(nvram->len, XNU_MAX_NVRAM_SIZE);
    if (apple_nvram_serialize(nvram, nvram_data, XNU_MAX_NVRAM_SIZE) < 0) { error_report("Failed to read NVRAM"); }

    apple_iboot_setup_video(info);

    if (info->securerom != NULL) {
        address_space_rw(&address_space_memory, info->srom_base, MEMTXATTRS_UNSPECIFIED, info->securerom,
                         info->securerom_size, true);
        info->entry     = info->srom_base;
        info->entry_arg = 0;
        info->rvbar     = info->srom_base;
        return;
    }

    if (!os.auto_boot && info->dfu != NULL && !apple_dfu_booted(info->dfu)) {
        g_autofree uint8_t* nonce     = NULL;
        size_t              nonce_len = 0;

        apple_iboot_compute_ap_nonce(QCRYPTO_HASH_ALGO_SHA384, info->boot_nonce_generator, &nonce, &nonce_len);
        apple_dfu_start(info->dfu, nonce, nonce_len, apple_iboot_sep_nonce, sizeof(apple_iboot_sep_nonce),
                        apple_iboot_dfu_attached, info);
        if (info->disp_dart != NULL) { apple_iboot_dfu_display(info); }
        return;
    }

    apple_iboot_boot_os(info, &os);
    apple_iboot_free_os(&os);
}

static void apple_iboot_handoff_state(CPUState* cpu, run_on_cpu_data data)
{
    CPUARMState* env = cpu_env(cpu);

    cpu_synchronize_state(cpu);
    env->cp15.cpacr_el1 = REG_FIELD_DP64(env->cp15.cpacr_el1, CPACR_EL1, FPEN, 3);
    if (tcg_enabled()) { arm_rebuild_hflags(env); }
}

// iBoot hands off with FP enabled; SPTM uses NEON before enabling it itself.
void apple_iboot_handoff_cpu(CPUState* cpu) { async_run_on_cpu(cpu, apple_iboot_handoff_state, RUN_ON_CPU_NULL); }

static void apple_iboot_start_cpus(AppleBootInfo* info)
{
    CPUState* cpu;

    if (info->dfu != NULL && apple_dfu_waiting(info->dfu)) { return; }

    CPU_FOREACH (cpu) {
        ARMCPU* arm_cpu = ARM_CPU(cpu);

        if ((uint32_t)cpu->cpu_index < info->num_cpus) {
            object_property_set_uint(OBJECT(cpu), "rvbar", info->rvbar, &error_abort);
            cpu_reset(cpu);
        }
        if (cpu->cpu_index == 0) {
            arm_set_cpu_on(arm_cpu->mp_affinity, info->entry, info->entry_arg, arm_highest_el(&arm_cpu->env), true);
        }
        apple_iboot_handoff_cpu(cpu);
    }
}

void apple_iboot_init(AppleBootInfo* info, MachineState* machine)
{
    assert_cmphex(info->dram_size, !=, 0);

    info->machine      = machine;
    info->regions      = g_array_new(false, false, sizeof(AppleIBootRegion));
    info->carveout_end = info->dram_base + info->dram_size;
    for (size_t i = 0; i < info->num_carveouts; i++) {
        apple_iboot_carve_out(info, info->carveouts[i].id, info->carveouts[i].size);
    }
    info->num_static_regions = info->regions->len;
    info->static_end         = info->carveout_end;

    assert_nonnull(apple_iboot_find_region(info, APPLE_CARVEOUT_PRAM));
    assert_nonnull(apple_iboot_find_region(info, APPLE_CARVEOUT_VRAM));

    if (info->securerom_filename != NULL
        && !g_file_get_contents(info->securerom_filename, &info->securerom, &info->securerom_size, NULL))
    {
        error_setg(&error_fatal, "Failed to load SecureROM from `%s`", info->securerom_filename);
    }
}

void apple_iboot_reset(AppleBootInfo* info)
{
    info->build_version = 0;

    apple_iboot_setup(info);

    apple_iboot_start_cpus(info);

    apple_iboot_show_splash(info, info->dfu != NULL && apple_dfu_waiting(info->dfu) ? SPLASH_DFU : SPLASH_BOOT);
}
