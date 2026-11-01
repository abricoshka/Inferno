/*
 * ChefKiss Kernel Patches.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/arm/iboot/kernel_patches.h"
#include "fs/apfs.h"
#include "fs/hfs.h"
#include "qapi-types-machine-common.h"
#include "hw/arm/iboot/patcher.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "system/hw_accel.h"

#define NOP        (0xD503201F)
#define MOV_W0_WZR (0x2A1F03E0)
#define NOP_BYTES  0x1F, 0x20, 0x03, 0xD5
#define RET        (0xD65F03C0)
#define RET_BYTES  0xC0, 0x03, 0x5F, 0xD6
#define RETAB      (0xD65F0FFF)
#define PACIBSP    (0xD503237F)

static CKPatcherRange* ck_kp_range_from_va(MachoHeader64* kc, const char* name, vaddr base, vaddr size)
{
    CKPatcherRange* range = g_new0(CKPatcherRange, 1);
    range->addr           = base;
    range->length         = size;
    range->ptr            = apple_boot_va_to_ptr(kc, base);
    range->name           = name;
    return range;
}

static CKPatcherRange* ck_kp_find_section_range(MachoHeader64* kc, MachoHeader64* hdr, const char* segment,
                                                const char* section)
{
    MachoSection64*        sec;
    MachoSegmentCommand64* seg;

    seg = apple_boot_get_segment(hdr, segment);
    if (seg == NULL) { return NULL; }

    sec = apple_boot_get_section(seg, section);
    return sec == NULL ? NULL : ck_kp_range_from_va(kc, segment, sec->addr, sec->size);
}

// TODO: Fix host endianness BE vs LE troubles in here.
static MachoHeader64* ck_kp_find_image_header(MachoHeader64* kc, const char* bundle_id)
{
    g_autofree CKPatcherRange* kmod_info_range  = NULL;
    g_autofree CKPatcherRange* kext_info_range  = NULL;
    g_autofree CKPatcherRange* kmod_start_range = NULL;
    uint64_t*                  info;
    uint64_t*                  start;
    uint32_t                   count;
    uint32_t                   i;
    char                       kname[256] = {0};
    const char*                prelinkinfo;
    const char*                last_dict;

    if (kc->file_type == MH_FILESET) { return apple_boot_get_fileset_header(kc, bundle_id); }

    kmod_info_range = ck_kp_find_section_range(kc, kc, "__PRELINK_INFO", "__kmod_info");
    if (kmod_info_range == NULL) {
        kext_info_range = ck_kp_find_section_range(kc, kc, "__PRELINK_INFO", "__info");
        if (kext_info_range == NULL) {
            error_report("Unsupported XNU.");
            return NULL;
        }

        prelinkinfo = strstr((const char*)kext_info_range->ptr, "PrelinkInfoDictionary");
        last_dict   = strstr(prelinkinfo, "<array>") + 7;
        while (last_dict) {
            const char* nested_dict;
            const char* ident;
            const char* end_dict = strstr(last_dict, "</dict>");
            if (!end_dict) { break; }

            nested_dict = strstr(last_dict + 1, "<dict>");
            while (nested_dict) {
                if (nested_dict > end_dict) { break; }

                nested_dict = strstr(nested_dict + 1, "<dict>");
                end_dict    = strstr(end_dict + 1, "</dict>");
            }

            ident = g_strstr_len(last_dict, end_dict - last_dict, "CFBundleIdentifier");
            if (ident != NULL) {
                const char* value = strstr(ident, "<string>");
                if (value != NULL) {
                    const char* value_end;

                    value     += strlen("<string>");
                    value_end  = strstr(value, "</string>");
                    if (value_end != NULL) {
                        memcpy(kname, value, value_end - value);
                        kname[value_end - value] = 0;
                        if (strcmp(kname, bundle_id) == 0) {
                            const char* addr =
                                g_strstr_len(last_dict, end_dict - last_dict, "_PrelinkExecutableLoadAddr");
                            if (addr != NULL) {
                                const char* avalue = strstr(addr, "<integer");
                                if (avalue != NULL) {
                                    avalue = strstr(avalue, ">");
                                    if (avalue != NULL) { return apple_boot_va_to_ptr(kc, strtoull(++avalue, 0, 0)); }
                                }
                            }
                        }
                    }
                }
            }

            last_dict = strstr(end_dict, "<dict>");
        }

        return NULL;
    }
    kmod_start_range = ck_kp_find_section_range(kc, kc, "__PRELINK_INFO", "__kmod_start");
    if (kmod_start_range != NULL) {
        info  = (uint64_t*)kmod_info_range->ptr;
        start = (uint64_t*)kmod_start_range->ptr;
        count = kmod_info_range->length / 8;
        for (i = 0; i < count; i++) {
            const char* kext_name = (const char*)apple_boot_va_to_ptr(kc, info[i]) + 0x10;
            if (strcmp(kext_name, bundle_id) == 0) { return apple_boot_va_to_ptr(kc, start[i]); }
        }
    }

    return NULL;
}

static CKPatcherRange* ck_kp_find_image_text(MachoHeader64* kc, const char* bundle_id)
{
    MachoHeader64* hdr = ck_kp_find_image_header(kc, bundle_id);
    return hdr == NULL ? NULL : ck_kp_find_section_range(kc, hdr, "__TEXT_EXEC", "__text");
}

static CKPatcherRange* ck_kp_get_kernel_section(MachoHeader64* kc, const char* segment, const char* section)
{
    if (kc->file_type == MH_FILESET) {
        MachoHeader64* kernel = ck_kp_find_image_header(kc, "com.apple.kernel");
        return kernel == NULL ? NULL : ck_kp_find_section_range(kc, kernel, segment, section);
    }

    return ck_kp_find_section_range(kc, kc, segment, section);
}

static bool ck_kp_root_auth_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* func_start = ck_patcher_find_prev_insn(buffer, 30, PACIBSP, 0xFFFFFFFF, 0);
    if (func_start == NULL) {
        warn_report("%s: failed to find pacibsp, trying to find old logic", __func__);
        uint8_t* ret = ck_patcher_find_next_insn(buffer, 4, RET, 0xFFFFFFFF, 0);
        if (ret == NULL) {
            error_report("%s: neither variants matched", __func__);
            return false;
        }
        stl_le_p(ret - 4, MOV_W0_WZR);
        return true;
    }
    stl_le_p(func_start, MOV_W0_WZR);
    stl_le_p(func_start + 4, RET);

    return true;
}

static bool ck_kp_root_hash_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* func_start = ck_patcher_find_prev_insn(buffer, 0x40, PACIBSP, 0xFFFFFFFF, 0);
    if (func_start == NULL) {
        error_report("%s: failed to find pacibsp", __func__);
        return false;
    }
    stl_le_p(func_start, MOV_W0_WZR);
    stl_le_p(func_start + 4, RET);

    return true;
}

static void ck_kp_apfs_patches(CKPatcherRange* range, KernelPatchset patchset)
{
    static const uint8_t root_auth_pattern[] = {
        0x08, 0xE0, 0x40, 0x39,    // ldrb w8, [x?, #0x38]
        0x08, 0x00, 0x00, 0x37,    // tbnz w8, #0x5, #?
        0x00, 0x0A, 0x80, 0x52,    // mov w?, #0x50
    };
    static const uint8_t root_auth_mask[] = {0x1F, 0xFC, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(root_auth_pattern) != sizeof(root_auth_mask));
    ck_patcher_find_callback(range, "bypass root authentication", root_auth_pattern, root_auth_mask,
                             sizeof(root_auth_pattern), sizeof(uint32_t), 1, ck_kp_root_auth_callback);

    static const uint8_t root_hash_pattern[] = {
        0x88, 0x62, 0x40, 0xF9,    // ldr x8, [x20, #0xC0]
        0x08, 0x89, 0x47, 0x79,    // ldrh w8, [x8, #0x3C4]
        0x1F, 0x11, 0x00, 0x71,    // cmp w8, #0x4
    };
    ck_patcher_find_callback(range, "bypass root hash authentication", root_hash_pattern, NULL,
                             sizeof(root_hash_pattern), sizeof(uint32_t), 1, ck_kp_root_hash_callback);

    // Plain and decmpfs reads each have a compare; a patched one no longer matches.
    static const uint8_t data_hash_pattern[] = {
        0x00, 0x68, 0x40, 0x39,    // ldrb w0, [x?, #0x1A]
        0x00, 0x00, 0x00, 0x00,    // ?
        0x00, 0x00, 0x00, 0x00,    // ?
        0x00, 0x00, 0x00, 0x94,    // bl cc_cmp_safe
    };
    static const uint8_t data_hash_mask[] = {
        0x1F, 0xFC, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC,
    };
    QEMU_BUILD_BUG_ON(sizeof(data_hash_pattern) != sizeof(data_hash_mask));
    static const uint8_t data_hash_repl[] = {MOV_W0_WZR_BYTES};
    for (int i = 0; i < 2; ++i) {
        ck_patcher_find_replace(range, "bypass sealed volume data hash verification", data_hash_pattern, data_hash_mask,
                                sizeof(data_hash_pattern), sizeof(uint32_t), 1, data_hash_repl, NULL, 12,
                                sizeof(data_hash_repl));
    }

    if (patchset >= KERNEL_PATCHSET_FULL) {
        static const uint8_t root_rw_pattern[] = {
            0x00, 0x00, 0x00, 0x94,    // bl ?
            0x00, 0x00, 0x70, 0x37,    // tbnz w0, 0xE, ?
            0xA0, 0x03, 0x40, 0xB9,    // ldr x?, [x29/sp, ?]
            0x00, 0x78, 0x1F, 0x12,    // and w?, w?, 0xFFFFFFFE
            0xA0, 0x03, 0x00, 0xB9,    // str x?, [x29/sp, ?]
        };
        static const uint8_t root_rw_mask[] = {
            0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0xF8, 0xFF, 0xA0, 0x03,
            0xFE, 0xFF, 0x00, 0xFC, 0xFF, 0xFF, 0xA0, 0x03, 0xC0, 0xFF,
        };
        QEMU_BUILD_BUG_ON(sizeof(root_rw_pattern) != sizeof(root_rw_mask));
        static const uint8_t root_rw_repl[] = {MOV_W0_WZR_BYTES};
        if (!ck_patcher_find_replace(range, "allow mounting root as R/W", root_rw_pattern, root_rw_mask,
                                     sizeof(root_rw_pattern), sizeof(uint32_t), 1, root_rw_repl, NULL, 4,
                                     sizeof(root_rw_repl)))
        {
            static const uint8_t root_rw_pattern_new[] = {
                0x00, 0x00, 0x00, 0x94,    // bl ?
                0x00, 0x00, 0x70, 0x37,    // tbnz w0, 0xE, ?
                0x00, 0x00, 0x80, 0x52,    // mov w0, #0
                0x00, 0x00, 0x00, 0x14,    // bl ?
            };
            static const uint8_t root_rw_mask_new[] = {
                0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFC,
            };
            QEMU_BUILD_BUG_ON(sizeof(root_rw_pattern_new) != sizeof(root_rw_mask_new));
            ck_patcher_find_replace(range, "allow mounting root as R/W (new)", root_rw_pattern_new, root_rw_mask_new,
                                    sizeof(root_rw_pattern_new), sizeof(uint32_t), 1, root_rw_repl, NULL, 4,
                                    sizeof(root_rw_repl));
        }

        // Bypass "Rooting from the live fs of a sealed volume is not allowed on a RELEASE build"
        static const uint8_t no_snapshot_pattern[] = {
            0xe8, 0x62, 0x40, 0xf9,    // ldr x8, [x23, #0xc0]
            0x08, 0xe1, 0x40, 0x39,    // ldrb w8, [x8, #0x38]
            0x00, 0x00, 0x28, 0x37,    // tbnz w8, #0x5, 0x...
        };
        static const uint8_t no_snapshot_mask[] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF,
        };
        QEMU_BUILD_BUG_ON(sizeof(no_snapshot_pattern) != sizeof(no_snapshot_mask));
        static const uint8_t no_snapshot_repl[] = {NOP};
        ck_patcher_find_replace(range, "bypass no snapshot", no_snapshot_pattern, no_snapshot_mask,
                                sizeof(no_snapshot_pattern), sizeof(uint32_t), 1, no_snapshot_repl, NULL, 8,
                                sizeof(no_snapshot_repl));

        // Bypass "root volume seal is broken"
        static const uint8_t broken_root_seal_pattern[] = {
            0x00, 0x00, 0x00, 0x97,    // bl sub_...
            0x00, 0x00, 0x70, 0x37,    // tbnz w0, #0xe, 0x...
            0x20, 0x00, 0x80, 0x52,    // mov w0, #0x1
        };
        static const uint8_t broken_root_seal_mask[] = {
            0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        };
        QEMU_BUILD_BUG_ON(sizeof(broken_root_seal_pattern) != sizeof(broken_root_seal_mask));
        static const uint8_t broken_root_seal_repl[] = {NOP};
        ck_patcher_find_replace(range, "bypass broken root seal", broken_root_seal_pattern, broken_root_seal_mask,
                                sizeof(broken_root_seal_pattern), sizeof(uint32_t), 1, broken_root_seal_repl, NULL, 4,
                                sizeof(broken_root_seal_repl));
    }
}

static bool ck_kp_tc_callback(void* ctx, uint8_t* buffer)
{
    if (((ldl_le_p(buffer - 4) & 0xFF000000) != 0x91000000) && ((ldl_le_p(buffer - 8) & 0xFF000000) != 0x91000000)) {
        return false;
    }

    uint8_t* ldrb         = ck_patcher_find_next_insn(buffer, 256, 0x39402C00, 0xFFFFFC00, 0);
    uint32_t cdhash_param = extract32(ldl_le_p(ldrb), 5, 5);
    uint8_t* frame;
    uint8_t* start = buffer;
    bool     pac;

    frame = ck_patcher_find_prev_insn(buffer, 10, 0x910003FD, 0xFF8003FF, 0);
    if (frame == NULL) { info_report("%s: found AMFI (Leaf)", __func__); }
    else {
        info_report("%s: found AMFI (Routine)", __func__);
        start = ck_patcher_find_prev_insn(frame, 10, 0xA9A003E0, 0xFFE003E0, 0);
        if (start == NULL) {
            start = ck_patcher_find_prev_insn(frame, 10, 0xD10003FF, 0xFF8003FF, 0);
            if (start == NULL) {
                error_report("%s: failed to find AMFI start", __func__);
                return false;
            }
        }
    }

    pac = ck_patcher_find_prev_insn(start, 5, PACIBSP, 0xFFFFFFFF, 0) != NULL;
    switch (cdhash_param) {
        case 0: {
            // adrp x8, ?
            uint8_t* adrp = ck_patcher_find_prev_insn(start, 10, 0x90000008, 0x9F00001F, 0);
            if (adrp != NULL) { start = adrp; }
            stl_le_p(start, 0x52802020);    // mov w0, 0x101
            stl_le_p(start + 4, (pac ? RETAB : RET));
            return true;
        }
        case 1:
            stl_le_p(start, 0x52800040);         // mov w0, 2
            stl_le_p(start + 4, 0x39000040);     // strb w0, [x2]
            stl_le_p(start + 8, 0x52800020);     // mov w0, 1
            stl_le_p(start + 12, 0x39000060);    // strb w0, [x3]
            stl_le_p(start + 16, 0x52800020);    // mov w0, 1
            stl_le_p(start + 20, (pac ? RETAB : RET));
            return true;
        default: error_report("%s: found unexpected AMFI prototype: %u", __func__, cdhash_param); break;
    }
    return false;
}

static bool ck_kp_tc_ios16_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* start = ck_patcher_find_prev_insn(buffer, 100, PACIBSP, 0xFFFFFFFF, 0);

    if (start == NULL) { return false; }

    stl_le_p(start, 0x52802020);    // mov w0, 0x101
    stl_le_p(start + 4, RET);

    return true;
}

static void ck_kp_tc_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0x00, 0x02, 0x80, 0x52,    // mov w?, 0x16
        0x00, 0x00, 0x00, 0xD3,    // lsr ?
        0x00, 0x00, 0x00, 0x9B,    // madd ?
    };
    static const uint8_t mask[] = {0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    if (!ck_patcher_find_callback(range, "all binaries in trustcache", pattern, mask, sizeof(pattern), sizeof(uint32_t),
                                  1, ck_kp_tc_callback))
    {
        static const uint8_t ios16_pattern[] = {0xC0, 0xCF, 0x9D, 0xD2};    // mov w?, 0xEE7E
        static const uint8_t io16_mask[]     = {0xC0, 0xFF, 0xFF, 0xFF};
        QEMU_BUILD_BUG_ON(sizeof(ios16_pattern) != sizeof(io16_mask));
        ck_patcher_find_callback(range, "all binaries in trustcache (iOS 16+)", ios16_pattern, io16_mask,
                                 sizeof(ios16_pattern), sizeof(uint32_t), 1, ck_kp_tc_ios16_callback);
    }
}

static bool ck_kp_amfi_sha1_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* cmp = ck_patcher_find_next_insn(buffer, 0x10, 0x7100001F, 0xFFC003FF,
                                             0);    // cmp w0, #?

    if (cmp == NULL) {
        error_report("%s: failed to find cmp", __func__);
        return false;
    }

    uint8_t* bne = ck_patcher_find_next_insn(cmp, 4, 0x54000001, 0xFF00001F, 0);    // b.ne #?
    if (bne == NULL) {
        error_report("%s: failed to find b.ne", __func__);
        return false;
    }

    // iOS 27 flipped the gate: `b.ne` now goes to the tbz's allow path instead of the reject path.
    int32_t bne_imm = sextract32(ldl_le_p(bne), 5, 19);
    if (buffer + ((int64_t)sextract32(ldl_le_p(buffer), 5, 14) << 2) == bne + ((int64_t)bne_imm << 2)) {
        stl_le_p(bne, 0x14000000 | (bne_imm & 0x03FFFFFF));    // b #?
    }
    else {
        stl_le_p(cmp, 0x6B00001F);    // cmp w0, w0
    }
    return true;
}

static bool ck_kp_amfi_tc_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* start = ck_patcher_find_prev_insn(buffer, 0x20, PACIBSP, 0xFFFFFFFF, 0);
    if (start == NULL) {
        error_report("%s: failed to find start of function", __func__);
        return false;
    }

    uint8_t* mov_x19 = ck_patcher_find_next_insn(start, 0x10, 0xAA0003F3, 0xFFE0FFFF, 0);
    if (mov_x19 == NULL) {
        error_report("%s: failed to find out-param register", __func__);
        return false;
    }
    uint32_t reg = (ldl_le_p(mov_x19) >> 16) & 0x1F;

    stl_le_p(start, 0xD2800020);                     // mov x0, #1
    stl_le_p(start + 4, 0xB4000040 | reg);           // cbz x?, #0x8
    stl_le_p(start + 8, 0xF9000000 | (reg << 5));    // str x0, [x?]
    stl_le_p(start + 12, RET);
    return true;
}

static void ck_kp_amfi_patches(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {0x02, 0x00, 0xD0, 0x36};    // tbz w2, 0x1A, ?
    static const uint8_t mask[]    = {0x1F, 0x00, 0xF8, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    ck_patcher_find_callback(range, "allow SHA1 signatures in AMFI", pattern, mask, sizeof(pattern), sizeof(uint32_t),
                             1, ck_kp_amfi_sha1_callback);

    static const uint8_t amfi_tc_cache_pattern[] = {
        0xE0, 0x03, 0x00, 0x91,    // mov x0, sp
        0xE1, 0x03, 0x13, 0xAA,    // mov x1, x19
        0x00, 0x00, 0x00, 0x94,    // bl trustCacheQueryGetFlags
        0x9F, 0x02, 0x00, 0x71,    // cmp w20, 0
        0xE0, 0x17, 0x9F, 0x1A,    // cset w0, eq
    };
    static const uint8_t amfi_tc_cache_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
        0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(amfi_tc_cache_pattern) != sizeof(amfi_tc_cache_mask));
    ck_patcher_find_callback(range, "all binaries in TrustCache (AMFI)", amfi_tc_cache_pattern, amfi_tc_cache_mask,
                             sizeof(amfi_tc_cache_pattern), sizeof(uint32_t), 1, ck_kp_amfi_tc_callback);
}

static bool ck_kp_mac_mount_callback(void* ctx, uint8_t* buffer)
{
    // Search for tbnz w?, 5, ?
    uint8_t* inst = ck_patcher_find_prev_insn(buffer, 0x40, 0x37280000, 0xFFFE0000, 0);
    if (inst == NULL) {
        inst = ck_patcher_find_next_insn(buffer, 0x40, 0x37280000, 0xFFFE0000, 0);
        if (inst == NULL) {
            error_report("%s: failed to find nop point", __func__);
            return false;
        }
    }

    // Allow MNT_UNION mounts
    stl_le_p(inst, NOP);

    // Search for ldrb w8, [x?, 0x71]
    inst = ck_patcher_find_prev_insn(buffer, 0x40, 0x3941C408, 0xFFFFFC1F, 0);
    if (inst == NULL) {
        // Search for the same, but forwards.
        inst = ck_patcher_find_next_insn(buffer, 0x40, 0x3941C408, 0xFFFFFC1F, 0);
        if (inst == NULL) {
            // Search for add x8, x8/16, #0x70
            inst = ck_patcher_find_prev_insn(buffer, 0x40, 0x9101C008, 0xFFFFFCFF, 0);
            // Search for ldr w8, [x8, #0x1]
            if (inst != NULL && ldl_le_p(inst + 4) == 0x39400508) { inst += 4; }
            else {
                error_report("%s: failed to find xzr point", __func__);
                return false;
            }
        }
    }

    // Replace with a mov x8, xzr
    // This will bypass the (vp->v_mount->mnt_flag & MNT_ROOTFS) check
    stl_le_p(inst, 0xAA1F03E8);

    return true;
}

static void ck_kp_mac_mount_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0xE9,
        0x2F,
        0x1F,
        0x32,    // orr w9, wzr, 0x1FFE
    };
    if (!ck_patcher_find_callback(range, "allow remounting rootfs, union mounts (old)", pattern, NULL, sizeof(pattern),
                                  sizeof(uint32_t), 1, ck_kp_mac_mount_callback))
    {
        static const uint8_t new_pattern[] = {
            0xC9,
            0xFF,
            0x83,
            0x12,    // movz w/x9, 0x1FFE/-0x1FFF
        };
        static const uint8_t new_mask[] = {0xFF, 0xFF, 0xFF, 0x3F};
        QEMU_BUILD_BUG_ON(sizeof(new_pattern) != sizeof(new_mask));
        if (!ck_patcher_find_callback(range, "allow remounting rootfs, union mounts (new)", new_pattern, new_mask,
                                      sizeof(new_pattern), sizeof(uint32_t), 1, ck_kp_mac_mount_callback))
        {
            static const uint8_t v27_pattern[] = {
                0x00, 0x00, 0x28, 0x37,    // tbnz w?, #5, ? (MNT_UNION)
                0x08, 0x50, 0x41, 0x39,    // ldrb w8, [x?, #0x54]
                0x08, 0x00, 0x00, 0x36,    // tbz w8, #0, ?
            };
            static const uint8_t v27_mask[] = {
                0x00, 0x00, 0xFE, 0xFF, 0x1F, 0xFC, 0xFF, 0xFF, 0x1F, 0x00, 0xF8, 0xFF,
            };
            QEMU_BUILD_BUG_ON(sizeof(v27_pattern) != sizeof(v27_mask));
            ck_patcher_find_callback(range, "allow remounting rootfs, union mounts (iOS 27)", v27_pattern, v27_mask,
                                     sizeof(v27_pattern), sizeof(uint32_t), 1, ck_kp_mac_mount_callback);
        }
    }
}

static bool ck_kp_kprintf_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* comparison      = buffer + sizeof(uint32_t) * 3;
    uint32_t comparison_inst = ldl_le_p(comparison);
    // if cbnz
    if (comparison_inst & BIT32(24)) { stl_le_p(comparison, NOP); }
    else {
        // turn into unconditional branch
        stl_le_p(comparison, 0x14000000 | extract32(comparison_inst, 5, 19));
    }
    return true;
}

static void ck_kp_kprintf_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0xAA, 0x43, 0x00, 0x91,    // add x10, fp, #0x10
        0xEA, 0x07, 0x00, 0xF9,    // str x10, [sp, #0x8]
        0x08, 0x00, 0x00, 0x2A,    // orr w8, w?, w?
        0x08, 0x00, 0x00, 0x34,    // cbz w8, #?
    };
    static const uint8_t mask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0x1F, 0xFC, 0xE0, 0xFF, 0x1F, 0x00, 0x00, 0xFE};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    if (!ck_patcher_find_callback(range, "force enable kprintf", pattern, mask, sizeof(pattern), sizeof(uint32_t), 1,
                                  ck_kp_kprintf_callback))
    {
        static const uint8_t pattern_new[] = {
            0x08, 0x01, 0x40, 0x39,    // ldrb w8, [x8, #0x?]
            0x08, 0x00, 0x00, 0x36,    // tbz w8, #0, #?
            0xA0, 0x43, 0x00, 0x91,    // add x?, fp, #0x10
            0xE0, 0x17, 0x00, 0xF9,    // str x?, [sp, #0x28]
            0xE0, 0xA3, 0x00, 0x91,    // add x?, sp, #0x28
            0x00, 0x00, 0x00, 0x14,    // b #?
        };
        static const uint8_t mask_new[] = {0xFF, 0x03, 0xC0, 0xFF, 0x1F, 0x00, 0x00, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF,
                                           0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFC};
        QEMU_BUILD_BUG_ON(sizeof(pattern_new) != sizeof(mask_new));
        static const uint8_t repl_new[] = {NOP_BYTES, NOP_BYTES, NOP_BYTES, NOP_BYTES, NOP_BYTES, NOP_BYTES};
        ck_patcher_find_replace(range, "force enable kprintf (new)", pattern_new, mask_new, sizeof(pattern_new),
                                sizeof(uint32_t), 1, repl_new, NULL, 0, sizeof(repl_new));
    }
}

// gAMXVersion seemingly unused, but removing it just in case.
// New: Used in iOS 17+ to set the cpu_capabilities bit.
static bool ck_kp_amx_common(uint8_t* buffer, bool newer)
{
    uint8_t* amx_ver_str = ck_patcher_find_prev_insn(buffer, newer ? 6 : 10, 0xB8000000, 0xFEC00000, newer ? 0 : 1);
    if (amx_ver_str == NULL) {
        error_report("%s: Failed to find store to gAMXVersion.", __func__);
        return false;
    }
    stl_le_p(amx_ver_str, NOP);

    return true;
}

static bool ck_kp_amx_callback(void* ctx, uint8_t* buffer)
{
    stl_le_p(buffer, 0x52810009);    // mov w9, #0x800

    return ck_kp_amx_common(buffer, false);
}

static bool ck_kp_amx_new_callback(void* ctx, uint8_t* buffer)
{
    // remove AMX support bit from movk
    // movk w?, #0x100, lsl #0x10
    stl_le_p(buffer + 4, ldl_le_p(buffer + 4) & ~(0x800 << 5));

    return ck_kp_amx_common(buffer, false);
}

static bool ck_kp_amx_newer_callback(void* ctx, uint8_t* buffer) { return ck_kp_amx_common(buffer, true); }

static bool ck_kp_amx_imm_callback(void* ctx, uint8_t* buffer)
{
    uint32_t insn = ldl_le_p(buffer);
    if (((insn >> 5) & 0x1F) == 31) { return false; }
    stl_le_p(buffer, 0x32150000 | (insn & 0x3FF));    // orr w?, w?, #0x800
    return true;
}

static bool ck_kp_amx_cap_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* lsl = ck_patcher_find_prev_insn(buffer, 8, 0x53051000, 0xFFFFFC00, 0);    // lsl w?, w?, #0x1b
    if (lsl == NULL) {
        error_report("%s: failed to find AMX capability shift", __func__);
        return false;
    }
    stl_le_p(lsl, 0x52800000 | (ldl_le_p(lsl) & 0x1F));    // mov w?, #0
    return true;
}

// in _commpage_populate
static void ck_kp_amx_patch(CKPatcherRange* range)
{
    static const uint8_t cap_pattern[] = {
        0x00, 0x90, 0x87, 0x52,    // mov w?, #0x3C80
        0x00, 0x00, 0x00, 0xAA,    // orr x?, x?, x?
    };
    static const uint8_t cap_mask[] = {0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0xFC, 0xE0, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(cap_pattern) != sizeof(cap_mask));
    if (ck_patcher_find_callback(range, "disable AMX (iOS 27)", cap_pattern, cap_mask, sizeof(cap_pattern),
                                 sizeof(uint32_t), 1, ck_kp_amx_cap_callback))
    {
        return;
    }

    static const uint8_t pattern_new[] = {
        0x00, 0x90, 0x87, 0x52,    // mov w?, #0x3C80
        0x00, 0x20, 0xA1, 0x72,    // movk w?, #0x900, lsl #0x10
        0x00, 0x40, 0x00, 0x2A,    // orr w?, w?, w?, lsl #0x10
    };
    static const uint8_t mask_new[] = {0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0xFC, 0xE0, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern_new) != sizeof(mask_new));
    if (!ck_patcher_find_callback(range, "disable AMX (new)", pattern_new, mask_new, sizeof(pattern_new),
                                  sizeof(uint32_t), 1, ck_kp_amx_new_callback))
    {
        static const uint8_t pattern[] = {
            0xE9, 0x83, 0x05, 0x32,    // mov w9, #0x8000800
            0x09, 0x00, 0x00, 0xAA,    // orr x9, x?, x?
        };
        static const uint8_t mask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0xFC, 0xE0, 0xFF};
        QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
        if (!ck_patcher_find_callback(range, "disable AMX", pattern, mask, sizeof(pattern), sizeof(uint32_t), 1,
                                      ck_kp_amx_callback))
        {
            static const uint8_t pattern_imm[] = {0x00, 0x80, 0x05, 0x32};    // orr w?, w?, #0x8000800
            static const uint8_t mask_imm[]    = {0x00, 0xFC, 0xFF, 0xFF};
            QEMU_BUILD_BUG_ON(sizeof(pattern_imm) != sizeof(mask_imm));
            if (ck_patcher_find_callback(range, "disable AMX (imm)", pattern_imm, mask_imm, sizeof(pattern_imm),
                                         sizeof(uint32_t), 1, ck_kp_amx_imm_callback))
            {
                return;
            }

            static const uint8_t pattern_newer[] = {
                0x0A, 0xF1, 0x1C, 0xD5,    // msr amx_config_el1, x?
                0xDF, 0x3F, 0x03, 0xD5,    // isb
            };
            static const uint8_t mask_newer[] = {0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            ck_patcher_find_callback(range, "disable AMX (newer)", pattern_newer, mask_newer, sizeof(pattern_newer),
                                     sizeof(uint32_t), 1, ck_kp_amx_newer_callback);
        }
    }
}

// this will tell launchd this is an internal build,
// and that way we can get hactivation without bypassing
// or patching the activation procedure.
// This is NOT an iCloud bypass. This is utilising code that ALREADY exists
// in the activation daemon. This is essentially telling iOS, it's a
// development kernel/device, NOT the real product sold on market. IF you
// decide to use this knowledge to BYPASS technological countermeasures
// or any other intellectual theft or crime, YOU are responsible in full,
// AND SHOULD BE PROSECUTED TO THE FULL EXTENT OF THE LAW.
// We do NOT endorse nor approve the theft of property.
static void ck_kp_hactivation_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = "\0release";
    static const uint8_t repl[]    = "profile";
    ck_patcher_find_replace(range, "enable hactivation", pattern, NULL, sizeof(pattern), 0, 1, repl, NULL, 1,
                            sizeof(repl));
}

static bool ck_kp_img4_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* start = ck_patcher_find_prev_insn(buffer, 200, PACIBSP, 0xFFFFFFFF, 0);

    if (start == NULL) { return false; }

    stl_le_p(start, MOV_W0_WZR);
    stl_le_p(start + 4, RET);

    return true;
}

static void ck_kp_img4_patches(CKPatcherRange* range)
{
    // in Img4DecodePerformTrustEvaluationWithCallbacksInternal
    static const uint8_t pattern[] = {
        0x21,
        0x09,
        0x43,
        0xB2,    // orr x1, x9, #0xe000000000000000
    };
    ck_patcher_find_callback(range, "allow unsigned firmware in img4_firmware_evaluate", pattern, NULL, sizeof(pattern),
                             sizeof(uint32_t), 1, ck_kp_img4_callback);
}

static vaddr ck_kp_find_cstring(CKPatcherRange* range, const char* sub)
{
    size_t len = strlen(sub);

    if (range == NULL || len == 0) { return 0; }
    for (vaddr i = 0; i + len <= range->length; ++i) {
        vaddr start;
        if (memcmp(range->ptr + i, sub, len) != 0) { continue; }
        for (start = i; start > 0 && range->ptr[start - 1] != 0; --start) { }
        return range->addr + start;
    }
    return 0;
}

static bool ck_kp_stub_func_by_string(CKPatcherRange* text, CKPatcherRange* strings, const char* name, const char* str)
{
    vaddr sva = ck_kp_find_cstring(strings, str);
    if (sva == 0) { return false; }

    for (vaddr off = 0; off + 2 * sizeof(uint32_t) <= text->length; off += sizeof(uint32_t)) {
        vaddr    target;
        uint8_t* start;

        if (!ck_patcher_adrp_add_va(text->ptr + off, text->addr + off, &target) || target != sva) { continue; }

        start = ck_patcher_find_prev_insn(text->ptr + off, 200, PACIBSP, 0xFFFFFFFF, 0);
        if (start == NULL) {
            error_report("`%s`: failed to find function start", name);
            return false;
        }
        stl_le_p(start, MOV_W0_WZR);
        stl_le_p(start + sizeof(uint32_t), RET);
        info_report("`%s` patch applied in `%s`.", name, text->name);
        return true;
    }

    warn_report("`%s`: found the string but not its reference", name);
    return false;
}

static void ck_kp_image4_eval_patches(CKPatcherRange* text, CKPatcherRange* strings)
{
    static const uint8_t env_pattern[] = {
        0x5F, 0x24, 0x03, 0xD5,    // bti c
        0x1F, 0x04, 0x00, 0x71,    // cmp w0, #1
        0x41, 0x00, 0x00, 0x54,    // b.ne #8
        0xC0, 0x03, 0x5F, 0xD6,    // ret
        0xE0, 0x03, 0x01, 0xAA,    // mov x0, x1
        0xE1, 0x03, 0x02, 0xAA,    // mov x1, x2
        0x00, 0x00, 0x00, 0x14,    // b #?
    };
    static const uint8_t env_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFC,
    };
    QEMU_BUILD_BUG_ON(sizeof(env_pattern) != sizeof(env_mask));
    static const uint8_t env_repl[] = {NOP_BYTES};
    ck_patcher_find_replace(text, "Image4 Kernelspace environment: always evaluate", env_pattern, env_mask,
                            sizeof(env_pattern), sizeof(uint32_t), 1, env_repl, NULL, 0xC, sizeof(env_repl));

    ck_kp_stub_func_by_string(text, strings, "bypass Image4 manifest signature and identity evaluation",
                              "manifest evaluation: starting signature and identity evaluation");
    ck_kp_stub_func_by_string(text, strings, "bypass Image4 subsequent stage anti-replay",
                              "manifest evaluation: attempting subsequent stage anti-replay policy");
    ck_kp_stub_func_by_string(text, strings, "bypass Image4 payload signature evaluation",
                              "payload evaluation: starting signature evaluation");
}

static void ck_kp_cs_patches(CKPatcherRange* range)
{
    //   ldrb w8,[sp,#0xc] ; ldrb w9,[sp,#0xd] ; cmp w8,w9 ; b.ne <reject>   ->   NOP the b.ne
    static const uint8_t v27[] = {
        0xE8, 0x33, 0x40, 0x39,    // ldrb w8, [sp, #0xc]
        0xE9, 0x37, 0x40, 0x39,    // ldrb w9, [sp, #0xd]
        0x1F, 0x01, 0x09, 0x6B,    // cmp w8, w9
        0x01, 0x00, 0x00, 0x54,    // b.ne #?
    };
    static const uint8_t v27_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(v27) != sizeof(v27_mask));
    static const uint8_t v27_repl[] = {NOP_BYTES};
    if (ck_patcher_find_replace(range, "bypass code signature checks (iOS 27)", v27, v27_mask, sizeof(v27),
                                sizeof(uint32_t), 1, v27_repl, NULL, 0xC, sizeof(v27_repl)))
    {
        return;
    }

    static const uint8_t repl[] = {NOP_BYTES};

    static const uint8_t pattern[] = {
        0x00, 0x00, 0x18, 0x36,    // tbz w?, #3, #?
        0x00, 0x00, 0x80, 0x52,    // mov w?, #0
    };
    static const uint8_t mask[] = {0x00, 0x00, 0xF8, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    if (ck_patcher_find_replace(range, "bypass code signature checks", pattern, mask, sizeof(pattern), sizeof(uint32_t),
                                1, repl, NULL, 0, sizeof(repl)))
    {
        return;
    }

    static const uint8_t alt[] = {
        0x00, 0x00, 0x18, 0x36,    // tbz w?, #3, #?
        0x10, 0x02, 0x17, 0xAA,    // mov x?, x?
        0x00, 0x00, 0x80, 0x52,    // mov w?, #0
    };
    static const uint8_t mask_alt[] = {0x00, 0x00, 0xF8, 0xFF, 0x10, 0xFE, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(alt) != sizeof(mask_alt));
    ck_patcher_find_replace(range, "bypass code signature checks (alt)", alt, mask_alt, sizeof(alt), sizeof(uint32_t),
                            1, repl, NULL, 0, sizeof(repl));
}

static void ck_kp_pmap_cs_enforce_patch(CKPatcherRange* range)
{
    // in pmap_enter_options_internal
    static const uint8_t pattern[] = {
        0x00, 0x00, 0x00, 0x94,    // bl #?
        0x00, 0x00, 0x00, 0x35,    // cbnz w0, #?
        0x88, 0x63, 0x80, 0x92,    // mov x8, #0xfffffffffffffce3
    };
    static const uint8_t mask[] = {
        0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    static const uint8_t repl[] = {MOV_W0_WZR_BYTES};
    if (!ck_patcher_find_replace(range, "bypass pmap_cs_enforce", pattern, mask, sizeof(pattern), sizeof(uint32_t), 1,
                                 repl, NULL, 0, sizeof(repl)))
    {
        static const uint8_t alt[] = {
            0x00, 0x00, 0x00, 0x94,    // bl #?
            0x00, 0x00, 0x00, 0x90,    // adrp x?, #?
            0x00, 0x00, 0x00, 0x35,    // cbnz w0, #?
            0xE0, 0x03, 0x00, 0xAA,    // mov x?, x?
            0x88, 0x63, 0x80, 0x92,    // mov x8, #0xfffffffffffffce3
        };
        static const uint8_t mask_alt[] = {
            0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x9F, 0x1F, 0x00,
            0x00, 0xFF, 0xE0, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        };
        QEMU_BUILD_BUG_ON(sizeof(alt) != sizeof(mask_alt));
        if (!ck_patcher_find_replace(range, "bypass pmap_cs_enforce (alt)", alt, mask_alt, sizeof(alt),
                                     sizeof(uint32_t), 1, repl, NULL, 0, sizeof(repl)))
        {
            static const uint8_t alt2[] = {
                0x00, 0x00, 0x00, 0x94,    // bl #?
                0x00, 0x00, 0x00, 0x35,    // cbnz w0, #?
                0xE0, 0x03, 0x00, 0xAA,    // mov x?, x?
                0x88, 0x63, 0x80, 0x92,    // mov x8, #0xfffffffffffffce3
            };
            static const uint8_t mask_alt2[] = {
                0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0xFF, 0xE0, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            };
            QEMU_BUILD_BUG_ON(sizeof(alt2) != sizeof(mask_alt2));
            ck_patcher_find_replace(range, "bypass pmap_cs_enforce (alt 2)", alt2, mask_alt2, sizeof(alt2),
                                    sizeof(uint32_t), 1, repl, NULL, 0, sizeof(repl));
        }
    }
}

// From iOS 18, APFS compares data hashes with corecrypto's `cc_cmp_safe` instead of an inlined copy.
static void ck_kp_apfs_data_hash_patch(CKPatcherRange* apfs, CKPatcherRange* corecrypto)
{
    // From iOS 27, the compare is imported through a stub.
    static const uint8_t data_hash_pattern[] = {
        0x02, 0x6C, 0x00, 0x91,    // add x2, x?, #0x1b
        0xE0, 0x03, 0x00, 0xAA,    // mov x0, x?
        0x00, 0x00, 0x00, 0x94,    // bl <hash compare>
        0x00, 0x00, 0x00, 0x34,    // cbz w0, <match>
    };
    static const uint8_t data_hash_mask[] = {
        0x1F, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(data_hash_pattern) != sizeof(data_hash_mask));
    static const uint8_t stub_repl[] = {MOV_W0_WZR_BYTES};
    if (ck_patcher_find_replace(apfs, "bypass sealed volume data hash verification (iOS 27)", data_hash_pattern,
                                data_hash_mask, sizeof(data_hash_pattern), sizeof(uint32_t), 0, stub_repl, NULL,
                                2 * sizeof(uint32_t), sizeof(stub_repl)))
    {
        return;
    }

    static const uint8_t cc_cmp_safe_sig[] = {
        0x28, 0x04, 0x00, 0xD1,    // sub x8, x1, #1
        0x49, 0x04, 0x00, 0xD1,    // sub x9, x2, #1
        0x0A, 0x01, 0x00, 0x8B,    // add x10, x8, x0
        0x4A, 0x01, 0x40, 0x39,    // ldrb w10, [x10]
    };

    if (corecrypto == NULL) { return; }

    uint8_t* sig = NULL;
    for (vaddr i = 0; i + sizeof(cc_cmp_safe_sig) <= corecrypto->length; i += sizeof(uint32_t)) {
        if (memcmp(corecrypto->ptr + i, cc_cmp_safe_sig, sizeof(cc_cmp_safe_sig)) == 0) {
            sig = corecrypto->ptr + i;
            break;
        }
    }
    if (sig == NULL) { return; }

    uint8_t* inner = ck_patcher_find_prev_insn(sig, 0x20, PACIBSP, 0xFFFFFFFF, 0);
    if (inner == NULL) {
        error_report("Failed to find the `cc_cmp_safe` entry.");
        return;
    }
    vaddr inner_va = corecrypto->addr + (inner - corecrypto->ptr);

    bool patched = false;
    for (vaddr i = 0; i + sizeof(uint32_t) <= apfs->length; i += sizeof(uint32_t)) {
        uint8_t* insn = apfs->ptr + i;
        if ((ldl_le_p(insn) & 0xFC000000) != 0x94000000) { continue; }    // bl #?

        vaddr target = (apfs->addr + i) + ((int64_t)sextract32(ldl_le_p(insn), 0, 26) << 2);
        if (target < corecrypto->addr || target >= corecrypto->addr + corecrypto->length) { continue; }

        // The call reaches `cc_cmp_safe` directly, or through a thin forwarding wrapper.
        vaddr rel     = target - corecrypto->addr;
        bool  reaches = target == inner_va;
        for (vaddr j = 0; !reaches && j < 24 && rel + (j + 1) * sizeof(uint32_t) <= corecrypto->length; ++j) {
            uint32_t w = ldl_le_p(corecrypto->ptr + rel + j * sizeof(uint32_t));
            if ((w & 0xFC000000) == 0x94000000
                && (target + j * sizeof(uint32_t)) + ((int64_t)sextract32(w, 0, 26) << 2) == inner_va)
            {
                reaches = true;
            }
        }
        if (reaches) {
            stl_le_p(insn, MOV_W0_WZR);
            patched = true;
        }
    }

    if (patched) {
        info_report("`bypass sealed volume data hash verification (iOS 18+)` patch applied in `%s`.", apfs->name);
    }
}

static bool ck_kp_wdt_timebase_callback(void* ctx, uint8_t* buffer)
{
    uint32_t nsec = ldl_le_p(buffer);
    uint32_t mul  = ldl_le_p(buffer + 8);
    uint32_t cmp  = ldl_le_p(buffer + 12);

    if (extract32(mul, 16, 5) != extract32(nsec, 0, 5) || extract32(cmp, 16, 5) != extract32(mul, 0, 5)) {
        return false;
    }
    stl_le_p(buffer + 8, mul | BIT(31));
    return true;
}

// `AppleS5L8960XWatchDogTimer::start` checks `clock * numer == denom * NSEC_PER_SEC` with a 32-bit right side, which
// only fits 24 MHz's `denom`.
static void ck_kp_wdt_timebase_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0x00, 0x40, 0x99, 0x52,    // mov w?, #0xCA00
        0x40, 0x73, 0xA7, 0x72,    // movk w?, #0x3B9A, lsl #16
        0x00, 0x7C, 0x00, 0x1B,    // mul w?, w?, w?
        0x1F, 0x00, 0x00, 0xEB,    // cmp x?, x?
    };
    static const uint8_t mask[] = {
        0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0xFC, 0xE0, 0xFF, 0x1F, 0xFC, 0xE0, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    ck_patcher_find_callback(range, "64-bit watchdog timebase check", pattern, mask, sizeof(pattern), sizeof(uint32_t),
                             1, ck_kp_wdt_timebase_callback);
}

static void ck_kp_apfs_snapshot_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = "com.apple.os.update-";
    static const uint8_t repl[]    = "shitcode.os.bullshit";
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(repl));
    ck_patcher_find_replace(range, "disable APFS snapshots", pattern, NULL, sizeof(pattern), 0, 0, repl, NULL, 0,
                            sizeof(repl));
}

void ck_patch_kernel(MachoHeader64* hdr, KernelPatchset patchset)
{
    MachoHeader64*             apfs_hdr        = NULL;
    g_autofree CKPatcherRange* apfs_text       = NULL;
    g_autofree CKPatcherRange* apfs_cstring    = NULL;
    g_autofree CKPatcherRange* corecrypto_text = NULL;
    g_autofree CKPatcherRange* amfi_text       = NULL;
    g_autofree CKPatcherRange* img4_text       = NULL;
    MachoHeader64*             image4_hdr      = NULL;
    g_autofree CKPatcherRange* image4_text     = NULL;
    g_autofree CKPatcherRange* image4_cstring  = NULL;
    g_autofree CKPatcherRange* fwupdate_text   = NULL;
    g_autofree CKPatcherRange* wdt_text        = NULL;
    g_autofree CKPatcherRange* kernel_text     = NULL;
    g_autofree CKPatcherRange* kernel_const    = NULL;
    g_autofree CKPatcherRange* kernel_ppltext  = NULL;

    apfs_hdr  = ck_kp_find_image_header(hdr, "com.apple.filesystems.apfs");
    apfs_text = ck_kp_find_section_range(hdr, apfs_hdr, "__TEXT_EXEC", "__text");
    ck_kp_apfs_patches(apfs_text, patchset);
    corecrypto_text = ck_kp_find_image_text(hdr, "com.apple.kec.corecrypto");
    ck_kp_apfs_data_hash_patch(apfs_text, corecrypto_text);
    if (patchset >= KERNEL_PATCHSET_FULL) {
        apfs_cstring = ck_kp_find_section_range(hdr, apfs_hdr, "__TEXT", "__cstring");
        if (apfs_cstring == NULL) { apfs_cstring = ck_kp_find_section_range(hdr, hdr, "__TEXT", "__cstring"); }
        ck_kp_apfs_snapshot_patch(apfs_cstring);
    }

    amfi_text = ck_kp_find_image_text(hdr, "com.apple.driver.AppleMobileFileIntegrity");
    ck_kp_amfi_patches(amfi_text);

    img4_text = ck_kp_find_image_text(hdr, "com.apple.security.AppleImage4");
    ck_kp_img4_patches(img4_text);

    fwupdate_text = ck_kp_find_image_text(hdr, "com.apple.driver.AppleFirmwareUpdateKext");
    if (fwupdate_text != NULL) { ck_kp_img4_patches(fwupdate_text); }

    wdt_text = hwaccel_enabled() ? ck_kp_find_image_text(hdr, "com.apple.driver.AppleS5L8960XWatchDogTimer") : NULL;
    if (wdt_text != NULL) { ck_kp_wdt_timebase_patch(wdt_text); }

    image4_hdr = ck_kp_find_image_header(hdr, "com.apple.security.Image4");
    if (image4_hdr != NULL) {
        image4_text    = ck_kp_find_section_range(hdr, image4_hdr, "__TEXT_EXEC", "__text");
        image4_cstring = ck_kp_find_section_range(hdr, image4_hdr, "__TEXT", "__cstring");
        if (image4_text != NULL) { ck_kp_image4_eval_patches(image4_text, image4_cstring); }
    }

    kernel_text = ck_kp_get_kernel_section(hdr, "__TEXT_EXEC", "__text");
    if (patchset >= KERNEL_PATCHSET_FULL) { ck_kp_mac_mount_patch(kernel_text); }
    if (patchset >= KERNEL_PATCHSET_DEBUG) { ck_kp_kprintf_patch(kernel_text); }
    ck_kp_amx_patch(kernel_text);
    ck_kp_cs_patches(kernel_text);
    kernel_const = ck_kp_get_kernel_section(hdr, "__TEXT", "__const");
    ck_kp_hactivation_patch(kernel_const);

    kernel_ppltext = ck_kp_find_section_range(hdr, hdr, "__PPLTEXT", "__text");
    if (kernel_ppltext != NULL) {
        ck_kp_tc_patch(kernel_ppltext);
        ck_kp_pmap_cs_enforce_patch(kernel_ppltext);
    }
    else if (apple_boot_get_segment(hdr, "__DATA_SPTM") != NULL) {
        // No `pmap_cs_enforce` in SPTM kernels.
        ck_kp_tc_patch(kernel_text);
    }
    else {
        warn_report("Failed to find `__PPLTEXT.__text`.");
        ck_kp_tc_patch(kernel_text);
        ck_kp_pmap_cs_enforce_patch(kernel_text);
    }
}

void ck_patch_txm(MachoHeader64* hdr)
{
    MachoSegmentCommand64* seg = apple_boot_get_segment(hdr, "__TEXT_EXEC");
    vaddr                  base;

    if (seg == NULL) {
        warn_report("Failed to find TXM `__TEXT_EXEC`.");
        return;
    }

    apple_boot_get_kc_bounds(hdr, NULL, &base, NULL, NULL, NULL);
    g_autofree CKPatcherRange* text =
        ck_patcher_range_from_ptr("__TEXT_EXEC", apple_boot_get_macho_buffer(hdr) + seg->vmaddr - base, seg->vmsize);
    text->addr = seg->vmaddr;
    g_autofree CKPatcherRange* strings =
        ck_patcher_range_from_ptr("TXM constants", apple_boot_get_macho_buffer(hdr), seg->vmaddr - base);
    strings->addr = base;

    //   mov w0, #0x293e ; movk w0, #0x3, lsl #16   ->   mov w0, #0x3e ; nop
    static const uint8_t pattern[] = {
        0xC0, 0x27, 0x85, 0x52,    // mov w0, #0x293e
        0x60, 0x00, 0xA0, 0x72,    // movk w0, #0x3, lsl #16
    };
    static const uint8_t repl[] = {
        0xC0, 0x07, 0x80, 0x52,    // mov w0, #0x3e
        0x1F, 0x20, 0x03, 0xD5,    // nop
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(repl));
    ck_patcher_find_replace(text, "accept code signature slot mismatch (TXM)", pattern, NULL, sizeof(pattern),
                            sizeof(uint32_t), 1, repl, NULL, 0, sizeof(repl));

    static const uint8_t img4_pattern[] = {
        0x5F, 0x24, 0x03, 0xD5,    // bti c
        0x7F, 0x23, 0x03, 0xD5,    // pacibsp
        0xFF, 0x43, 0x03, 0xD1,    // sub sp, sp, #0xd0
        0xF4, 0x4F, 0x0B, 0xA9,    // stp x20, x19, [sp, #0xb0]
        0xFD, 0x7B, 0x0C, 0xA9,    // stp x29, x30, [sp, #0xc0]
        0xFD, 0x03, 0x03, 0x91,    // add x29, sp, #0xc0
        0x08, 0x00, 0x40, 0xF9,    // ldr x8, [x0]
        0xBF, 0x0F, 0x3B, 0xA9,    // stp xzr, x3, [x29, #-0x50]
    };
    static const uint8_t img4_repl[] = {
        MOV_W0_WZR_BYTES,    // mov w0, wzr (over pacibsp)
        RET_BYTES,           // ret         (over sub sp), leaving `bti c`
    };
    ck_patcher_find_replace(text, "img4 payload signature bypass (TXM)", img4_pattern, NULL, sizeof(img4_pattern),
                            sizeof(uint32_t), 1, img4_repl, NULL, 4, sizeof(img4_repl));

    static const uint8_t cs_pattern[] = {
        0x08, 0x25, 0x43, 0x39,    // ldrb w8, [x8, #0xc9]
        0x08, 0x00, 0x00, 0x36,    // tbz w8, #0, <reject (0x32202)>   (offset masked out)
        0x48, 0x01, 0x80, 0x52,    // mov w8, #0xa
        0x00, 0x00, 0x00, 0x14,    // b <accept>                       (target masked out)
        0x40, 0x40, 0x84, 0x52,    // movz w0, #0x2202
        0x60, 0x00, 0xA0, 0x72,    // movk w0, #3, lsl #16
    };
    static const uint8_t cs_pattern_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF,    //
        0x1F, 0x00, 0xF8, 0xFF,    // tbz: match op/reg/bit, ignore branch offset
        0xFF, 0xFF, 0xFF, 0xFF,    //
        0x00, 0x00, 0x00, 0xFC,    // b: match opcode only
        0xFF, 0xFF, 0xFF, 0xFF,    //
        0xFF, 0xFF, 0xFF, 0xFF,    //
    };
    QEMU_BUILD_BUG_ON(sizeof(cs_pattern) != sizeof(cs_pattern_mask));
    static const uint8_t cs_repl[] = {NOP_BYTES};
    ck_patcher_find_replace(text, "code signature bypass (TXM)", cs_pattern, cs_pattern_mask, sizeof(cs_pattern),
                            sizeof(uint32_t), 1, cs_repl, NULL, 4, sizeof(cs_repl));

    ck_kp_stub_func_by_string(text, strings, "img4 manifest signature and identity evaluation bypass (TXM)",
                              "manifest evaluation: starting signature and identity evaluation");
}

/// Patch `buffer` and find the span that changed; returns false if nothing did.
static bool ck_kp_patch_libimg4(uint8_t* buffer, uint64_t len, uint64_t* first, uint64_t* last)
{
    g_autofree uint8_t*        original = g_memdup2(buffer, len);
    g_autofree CKPatcherRange* range    = ck_patcher_range_from_ptr("libimg4.dylib", buffer, len);

    ck_kp_img4_patches(range);

    // Write back only the touched span so a single decmpfs chunk is recompressed.
    for (*first = 0; *first < len && buffer[*first] == original[*first]; (*first)++) { }
    if (*first == len) { return false; }
    for (*last = len; *last > *first && buffer[*last - 1] == original[*last - 1]; (*last)--) { }
    return true;
}

static void ck_patch_ramdisk_apfs(uint8_t* image, uint64_t size)
{
    g_autoptr(ApfsFile) file   = NULL;
    g_autofree uint8_t* buffer = NULL;
    Error*              err    = NULL;
    uint64_t            len;
    uint64_t            first;
    uint64_t            last;

    // iOS 26 made libimg4.dylib a symlink to libimage4.dylib.
    file = apfs_open_memory_file(image, size, "/usr/lib/libimage4.dylib", NULL);
    if (file == NULL) { file = apfs_open_memory_file(image, size, "/usr/lib/libimg4.dylib", &err); }
    if (file == NULL) {
        info_report("Not patching ramdisk libimg4: %s", error_get_pretty(err));
        error_free(err);
        return;
    }

    len    = apfs_file_size(file);
    buffer = g_malloc(len);
    if (!apfs_file_pread(file, 0, len, buffer, &err)) {
        warn_report_err(err);
        return;
    }

    if (ck_kp_patch_libimg4(buffer, len, &first, &last)
        && !apfs_file_pwrite(file, first, last - first, buffer + first, &err))
    {
        warn_report_err(err);
    }
}

void ck_patch_ramdisk(uint8_t* image, uint64_t size)
{
    g_autoptr(HfsFile) file    = NULL;
    g_autofree uint8_t* buffer = NULL;
    Error*              err    = NULL;
    uint64_t            len;
    uint64_t            first;
    uint64_t            last;

    if (size >= 0x24 && ldl_le_p(image + 0x20) == 0x4253584E) {    // 'NXSB'
        ck_patch_ramdisk_apfs(image, size);
        return;
    }

    file = hfs_open_file(image, size, "/usr/lib/libimg4.dylib", &err);
    if (file == NULL) {
        info_report("Not patching ramdisk libimg4: %s", error_get_pretty(err));
        error_free(err);
        return;
    }

    len    = hfs_file_size(file);
    buffer = g_malloc(len);
    if (!hfs_file_pread(file, 0, len, buffer, &err)) {
        warn_report_err(err);
        return;
    }

    if (ck_kp_patch_libimg4(buffer, len, &first, &last)
        && !hfs_file_pwrite(file, first, last - first, buffer + first, &err))
    {
        warn_report_err(err);
    }
}

// The `b.ne` to the failure, or the NOP a previous patch of the image left.
static bool ck_kp_sep_timebase_callback(void* ctx, uint8_t* buffer)
{
    uint32_t insn = ldl_le_p(buffer + 16);

    if ((insn & 0xFF00001F) != 0x54000001 && insn != NOP) { return false; }
    stl_le_p(buffer + 16, NOP);
    return true;
}

void ck_patch_sep_timebase(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0x00, 0xE0, 0x3B, 0xD5,    // mrs x?, cntfrq_el0
        0x00, 0xC0, 0x86, 0x52,    // mov w?, #0x3600
        0xC0, 0x2D, 0xA0, 0x72,    // movk w?, #0x16E, lsl #16
        0x1F, 0x00, 0x00, 0x6B,    // cmp w?, w?
    };
    static const uint8_t mask[] = {
        0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0x1F, 0xFC, 0xE0, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    ck_patcher_find_callback(range, "accept any timebase", pattern, mask, sizeof(pattern), sizeof(uint32_t), 1,
                             ck_kp_sep_timebase_callback);
}
