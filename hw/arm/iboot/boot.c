/*
 * Apple OS Boot Logic.
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
#include "hw/arm/iboot/boot.h"
#include "hw/arm/dt.h"
#include "hw/arm/iboot/kernel_patches.h"
#include "hw/arm/mem.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/memory.h"
#include "img4.h"
#include "lzss.h"
#include <lzfse.h>

#if 0
    #define DINFO(fmt, ...) info_report(fmt, ##__VA_ARGS__)
#else
    #define DINFO(fmt, ...) \
        do { }              \
        while (0)
#endif

/*
 * @param file_data is consumed
 * @param payload_type must be at least 4 bytes long
 */
static void extract_im4p_payload(const char* filename, uint8_t* file_data, gsize fsize, char* payload_type,
                                 uint8_t** data, uint32_t* length, uint8_t** secure_monitor)
{
    char      error_desc[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    asn1_node img4_definitions = NULL;
    asn1_node img4;
    asn1_node im4p;
    int       ret;
    char      magic[4];
    char      description[128];
    int       len;
    uint8_t*  payload_data;
    int       start;
    int       end;
    uint8_t*  im4p_data = file_data;
    gsize     im4p_size = fsize;

    if (asn1_array2tree(img4_definitions_array, &img4_definitions, error_desc) != ASN1_SUCCESS) {
        error_setg(&error_fatal, "ASN.1 parser initialisation failed: `%s`", error_desc);
        return;
    }

    ret = asn1_create_element(img4_definitions, "Image4.IMG4", &img4);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IMG4 element creation failed: %d", ret);
        return;
    }

    if (asn1_der_decoding(&img4, file_data, (uint32_t)fsize, error_desc) == ASN1_SUCCESS
        && asn1_der_decoding_startEnd(img4, file_data, (uint32_t)fsize, "payload", &start, &end) == ASN1_SUCCESS)
    {
        im4p_data = file_data + start;
        im4p_size = end - start + 1;
    }
    asn1_delete_structure(&img4);

    ret = asn1_create_element(img4_definitions, "Image4.IM4P", &im4p);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P element creation failed: %d", ret);
        return;
    }

    ret = asn1_der_decoding(&im4p, im4p_data, (uint32_t)im4p_size, error_desc);
    if (ret != ASN1_SUCCESS) {
        *data   = file_data;
        *length = (uint32_t)fsize;
        strncpy(payload_type, "raw", 4);
        asn1_delete_structure(&im4p);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    len = 4;
    ret = asn1_read_value(im4p, "magic", magic, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P magic read for `%s` failed: %d", filename, ret);
        return;
    }

    if (memcmp(magic, "IM4P", 4) != 0) {
        error_setg(&error_fatal, "`%s` is not an IM4P", filename);
        return;
    }

    len = 4;
    ret = asn1_read_value(im4p, "type", payload_type, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P type read for `%s` failed: %d", filename, ret);
        return;
    }

    len = 128;
    ret = asn1_read_value(im4p, "description", description, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P description read for `%s` failed: %d", filename, ret);
        return;
    }

    len = 0;
    ret = asn1_read_value(im4p, "data", NULL, &len);
    if (ret != ASN1_MEM_ERROR) {
        error_setg(&error_fatal, "IM4P size read for `%s` failed: %d", filename, ret);
        return;
    }

    payload_data = g_malloc0(len);
    ret          = asn1_read_value(im4p, "data", payload_data, &len);
    g_free(file_data);

    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P read for `%s` failed: %d", filename, ret);
        return;
    }

    asn1_delete_structure(&im4p);
    asn1_delete_structure(&img4_definitions);

    if (memcmp(payload_data, "bvx", 3) == 0) {
        size_t   decode_buffer_size = len * 8;
        uint8_t* decode_buffer      = g_malloc0(decode_buffer_size);
        int      decoded_length     = lzfse_decode_buffer(decode_buffer, decode_buffer_size, payload_data, len, NULL);
        g_free(payload_data);

        if (decoded_length == 0 || decoded_length == decode_buffer_size) {
            error_setg(&error_fatal, "LZFSE decompression for `%s` failed; insufficient buffer size", filename);
            g_free(decode_buffer);
            return;
        }

        *data   = decode_buffer;
        *length = decoded_length;
        return;
    }

    if (memcmp(payload_data, "complzss", 8) == 0) {
        LzssCompHeader* comp_header       = (LzssCompHeader*)payload_data;
        size_t          uncompressed_size = be32_to_cpu(comp_header->uncompressed_size);
        size_t          compressed_size   = be32_to_cpu(comp_header->compressed_size);
        uint8_t*        decode_buffer     = g_malloc0(uncompressed_size);
        int             decoded_length    = decompress_lzss(decode_buffer, comp_header->data, compressed_size);
        if (decoded_length == 0 || decoded_length != uncompressed_size) {
            error_setg(&error_fatal, "LZSS decompression for `%s` failed", filename);
            g_free(decode_buffer);
            return;
        }

        size_t monitor_off = compressed_size + sizeof(LzssCompHeader);
        if (secure_monitor && monitor_off < len) {
            size_t monitor_size = len - monitor_off;
            DINFO("Found AP Secure Monitor in payload with size 0x%zX!", monitor_size);
            *secure_monitor = g_malloc0(monitor_size);
            memcpy(*secure_monitor, payload_data + monitor_off, monitor_size);
        }

        g_free(payload_data);

        *data   = decode_buffer;
        *length = decoded_length;
        return;
    }

    *data   = payload_data;
    *length = len;
}

AppleDTNode* apple_boot_load_dt_data(const char* filename, uint8_t* data, gsize len)
{
    AppleDTNode* root      = NULL;
    uint8_t*     file_data = NULL;
    uint32_t     fsize;
    char         payload_type[4];

    extract_im4p_payload(filename, data, len, payload_type, &file_data, &fsize, NULL);

    if (memcmp(payload_type, "dtre", 4) != 0 && memcmp(payload_type, "rdtr", 4) != 0
        && memcmp(payload_type, "raw", 4) != 0)
    {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `dtre`/`rdtr`)", filename, payload_type);
        return NULL;
    }

    root = apple_dt_deserialise(file_data);
    g_free(file_data);
    return root;
}

void apple_boot_load_root_hash_data(AppleDTNode* chosen, const char* filename, uint8_t* data, gsize len,
                                    bool root_snapshot_enabled)
{
    g_autoptr(GString) snapshot   = NULL;
    g_autofree uint8_t* file_data = NULL;
    uint32_t            length;
    uint32_t            hash_len;
    char                payload_type[4];

    extract_im4p_payload(filename, data, len, payload_type, &file_data, &length, NULL);

    if (memcmp(payload_type, "isys", 4) != 0) {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `isys`)", filename, payload_type);
        return;
    }

    apple_dt_set_prop(chosen, "system-volume-auth-blob", length, file_data);

    // iBoot names the root snapshot after the root hash, which sits at 0x10 with its length at 0xC.
    if ((length & ~0x80) != 0x50) {
        error_setg(&error_fatal, "`%s` has an unexpected `isys` size 0x%x", filename, length);
        return;
    }

    hash_len = (uint32_t)ldl_le_p(file_data + 0xC);
    if (hash_len > 0x40) {
        error_setg(&error_fatal, "`%s` has an oversized root hash (0x%x)", filename, hash_len);
        return;
    }

    if (!root_snapshot_enabled) { return; }

    snapshot = g_string_new("com.apple.os.update-");
    for (uint32_t i = 0; i < hash_len; ++i) { g_string_append_printf(snapshot, "%02X", file_data[0x10 + i]); }
    apple_dt_set_prop_str(chosen, "root-snapshot-name", snapshot->str);
}

static const uint8_t* apple_boot_der_next(const uint8_t** p, const uint8_t* end, uint8_t tag, gsize* len)
{
    const uint8_t* q = *p;
    gsize          n;
    uint8_t        len_bytes;

    if (end - q < 2 || *q++ != tag) { return NULL; }

    if ((tag & 0x1F) == 0x1F) {
        while (q != end && (*q & 0x80)) { q++; }
        if (end - q < 2) { return NULL; }
        q++;
    }

    n = *q++;
    if (n & 0x80) {
        len_bytes = n & 0x7F;
        if (len_bytes > sizeof(n) || end - q < len_bytes) { return NULL; }
        for (n = 0; len_bytes > 0; len_bytes--) { n = (n << 8) | *q++; }
    }

    if (n > (gsize)(end - q)) { return NULL; }

    *p   = q + n;
    *len = n;
    return q;
}

static bool apple_boot_der_enter(const uint8_t** p, const uint8_t** end, uint8_t tag)
{
    const uint8_t* q;
    gsize          n;

    q = apple_boot_der_next(p, *end, tag, &n);
    if (q == NULL) { return false; }

    *p   = q;
    *end = q + n;
    return true;
}

static bool apple_boot_der_enter_magic(const uint8_t** p, const uint8_t** end, const char* magic)
{
    const uint8_t* str;
    gsize          n;

    if (!apple_boot_der_enter(p, end, 0x30)) { return false; }

    str = apple_boot_der_next(p, *end, 0x16, &n);
    return str != NULL && n == 4 && memcmp(str, magic, 4) == 0;
}

void apple_boot_populate_manifest_props(AppleDTNode* node, const uint8_t* ticket, gsize ticket_len)
{
    const uint8_t* p   = ticket;
    const uint8_t* end = ticket + ticket_len;
    const uint8_t* q;
    const uint8_t* q_end;
    const uint8_t* val;
    gsize          n;
    gsize          i;
    uint8_t        tag;
    char           name[5];
    uint64_t       num;

    if (!apple_boot_der_enter_magic(&p, &end, "IM4M") || apple_boot_der_next(&p, end, 0x02, &n) == NULL
        || !apple_boot_der_enter(&p, &end, 0x31) || !apple_boot_der_enter(&p, &end, 0xFF)
        || !apple_boot_der_enter_magic(&p, &end, "MANB") || !apple_boot_der_enter(&p, &end, 0x31)
        || !apple_boot_der_enter(&p, &end, 0xFF) || !apple_boot_der_enter_magic(&p, &end, "MANP")
        || !apple_boot_der_enter(&p, &end, 0x31))
    {
        warn_report("Unable to find the AP ticket's manifest properties");
        return;
    }

    while (p != end) {
        q     = p;
        q_end = end;
        if (!apple_boot_der_enter(&q, &q_end, 0xFF)) { return; }
        p = q_end;

        if (!apple_boot_der_enter(&q, &q_end, 0x30)) { continue; }
        val = apple_boot_der_next(&q, q_end, 0x16, &n);
        if (val == NULL || n != 4 || q == q_end) { continue; }
        memcpy(name, val, 4);
        name[4] = '\0';

        tag = *q;
        val = apple_boot_der_next(&q, q_end, tag, &n);
        if (val == NULL) { continue; }

        switch (tag) {
            case 0x01:    // BOOLEAN
                if (n == 1) { apple_dt_set_prop_u32(node, name, val[0] != 0); }
                break;
            case 0x02:    // INTEGER
                if (n > sizeof(num) + 1) { break; }
                for (num = 0, i = 0; i < n; i++) { num = (num << 8) | val[i]; }
                apple_dt_set_prop_u64(node, name, num);
                break;
            case 0x04:    // OCTET STRING
                if (strcmp(name, "love") != 0) {
                    apple_dt_set_prop(node, name, n, val);
                    break;
                }
                QEMU_FALLTHROUGH;
            case 0x16: {    // IA5String
                g_autofree char* str = g_strndup((const char*)val, n);
                apple_dt_set_prop_str(node, name, str);
                break;
            }
            default: break;
        }
    }
}

uint8_t* apple_boot_load_trustcache_data(const char* filename, uint8_t* data, gsize len, uint64_t* size)
{
    uint32_t*           trustcache_data;
    uint64_t            trustcache_size;
    g_autofree uint8_t* file_data;
    unsigned long       file_size;
    uint32_t            length;
    char                payload_type[4];
    uint32_t            trustcache_version;
    uint32_t            trustcache_entry_count;
    uint32_t            expected_file_size;
    uint32_t            trustcache_entry_size;

    extract_im4p_payload(filename, data, len, payload_type, &file_data, &length, NULL);

    if (memcmp(payload_type, "trst", 4) != 0 && memcmp(payload_type, "rtsc", 4) != 0
        && memcmp(payload_type, "raw", 4) != 0)
    {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `trst`/`rtsc`)", filename, payload_type);
        return NULL;
    }

    file_size = (unsigned long)length;

    trustcache_size    = ROUND_UP_16K(file_size + 8);
    trustcache_data    = (uint32_t*)g_malloc0(trustcache_size);
    trustcache_data[0] = 1;    // #trustcaches
    trustcache_data[1] = 8;    // offset
    memcpy(&trustcache_data[2], file_data, file_size);

    // Validate the trustcache v1 header. The layout is:
    // uint32_t version
    // uuid (16 bytes)
    // uint32_t entry_count
    //
    // The cache is then followed by entry_count entries, each of which
    // contains a 20 byte hash and 2 additional bytes (hence is 22 bytes long)
    // for v1 and contains a 20 byte hash and 4 additional bytes (hence is 24
    // bytes long) for v2
    trustcache_version     = trustcache_data[2];
    trustcache_entry_count = trustcache_data[7];

    switch (trustcache_version) {
        case 1: trustcache_entry_size = 22; break;
        case 2: trustcache_entry_size = 24; break;
        default:
            error_setg(&error_fatal, "invalid trustcache header in `%s` (expected v1 or v2, got %d)", filename,
                       trustcache_version);
            return NULL;
    }

    // 24 is header size
    expected_file_size = 24 + trustcache_entry_count * trustcache_entry_size;

    assert_cmpuint(file_size, ==, expected_file_size);

    *size = trustcache_size;
    return (uint8_t*)trustcache_data;
}

uint8_t* apple_boot_load_ramdisk_data(const char* filename, uint8_t* data, gsize len, uint64_t* size)
{
    uint8_t*      file_data = NULL;
    unsigned long file_size = 0;
    uint32_t      length    = 0;
    char          payload_type[4];

    extract_im4p_payload(filename, data, len, payload_type, &file_data, &length, NULL);
    if (memcmp(payload_type, "rdsk", 4) != 0 && memcmp(payload_type, "raw", 4) != 0) {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `rdsk`)", filename, payload_type);
        return NULL;
    }

    file_size = length;
    file_data = g_realloc(file_data, file_size);

    ck_patch_ramdisk(file_data, file_size);

    *size = file_size;
    return file_data;
}

void apple_boot_load_raw_file(const char* filename, AddressSpace* as, hwaddr file_pa, uint64_t* size)
{
    uint8_t* file_data;
    gsize    sizef;

    if (g_file_get_contents(filename, (gchar**)&file_data, &sizef, NULL)) {
        *size = sizef;
        address_space_rw(as, file_pa, MEMTXATTRS_UNSPECIFIED, file_data, sizef, true);
        g_free(file_data);
    }
    else {
        error_setg(&error_fatal, "file read for `%s` failed", filename);
    }
}

void apple_boot_get_kc_bounds(MachoHeader64* header, vaddr* text_base, vaddr* kc_base, vaddr* kc_end, vaddr* ro_lower,
                              vaddr* ro_upper)
{
    MachoLoadCommand* cmd      = (MachoLoadCommand*)(header + 1);
    vaddr        text_base_cur = -1ULL, kc_base_cur = -1ULL, kc_end_cur = 0, ro_lower_cur = -1ULL, ro_upper_cur = 0;
    unsigned int i;

    for (i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd != LC_SEGMENT_64) { continue; }

        MachoSegmentCommand64* seg_cmd = (MachoSegmentCommand64*)cmd;

        if (strncmp(seg_cmd->segname, "__PAGEZERO", 10) == 0 || seg_cmd->vmsize == 0) { continue; }

        if (seg_cmd->filesize != 0 && seg_cmd->fileoff == 0) { text_base_cur = seg_cmd->vmaddr; }

        if (seg_cmd->vmaddr < kc_base_cur) { kc_base_cur = seg_cmd->vmaddr; }

        if (seg_cmd->vmaddr + seg_cmd->vmsize > kc_end_cur) { kc_end_cur = seg_cmd->vmaddr + seg_cmd->vmsize; }

        if ((seg_cmd->maxprot & VM_PROT_WRITE) != 0 || strncmp(seg_cmd->segname, "__LINKEDIT", 10) == 0) { continue; }

        if (ro_lower_cur >= seg_cmd->vmaddr) { ro_lower_cur = seg_cmd->vmaddr; }

        hwaddr vmend = seg_cmd->vmaddr + seg_cmd->vmsize;
        if (ro_upper_cur <= vmend) { ro_upper_cur = vmend; }
    }

    assert_cmphex(text_base_cur, !=, 0);
    assert_cmphex(text_base_cur, !=, -1ULL);
    assert_cmphex(kc_base_cur, !=, 0);
    assert_cmphex(kc_base_cur, !=, -1ULL);
    assert_cmphex(kc_end_cur, !=, 0);
    assert_cmphex(ro_lower_cur, !=, 0);
    assert_cmphex(ro_lower_cur, !=, -1ULL);
    assert_cmphex(ro_upper_cur, !=, 0);

    if (text_base != NULL) { *text_base = text_base_cur; }

    if (kc_base != NULL) { *kc_base = kc_base_cur & -0x2000000ull; }

    if (kc_end != NULL) { *kc_end = kc_end_cur; }

    if (ro_lower != NULL) { *ro_lower = ro_lower_cur; }

    if (ro_upper != NULL) { *ro_upper = ro_upper_cur; }
}

MachoHeader64* apple_boot_load_kernel_data(const char* filename, uint8_t* file_data, gsize file_len,
                                           MachoHeader64** secure_monitor)
{
    uint32_t       len;
    uint8_t*       data = NULL;
    char           payload_type[4];
    MachoHeader64* header;

    extract_im4p_payload(filename, file_data, file_len, payload_type, &data, &len, (uint8_t**)secure_monitor);

    if (memcmp(payload_type, "krnl", 4) != 0 && memcmp(payload_type, "rkrn", 4) != 0
        && memcmp(payload_type, "raw", 4) != 0)
    {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `krnl`/`rkrn`)", filename, payload_type);
        return NULL;
    }

    header = apple_boot_parse_macho(data, len);
    g_free(data);
    return header;
}

MachoHeader64* apple_boot_load_monitor_data(const char* filename, uint8_t* file_data, gsize file_len, const char* type,
                                            const char* restore_type)
{
    uint32_t       len;
    uint8_t*       data = NULL;
    char           payload_type[4];
    MachoHeader64* header;

    extract_im4p_payload(filename, file_data, file_len, payload_type, &data, &len, NULL);

    if (memcmp(payload_type, type, 4) != 0 && memcmp(payload_type, restore_type, 4) != 0
        && memcmp(payload_type, "raw", 4) != 0)
    {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `%s`/`%s`)", filename, payload_type, type,
                   restore_type);
        return NULL;
    }

    header = apple_boot_parse_macho(data, len);
    g_free(data);
    return header;
}

MachoHeader64* apple_boot_parse_macho(uint8_t* data, uint32_t len)
{
    uint8_t*          phys_base;
    MachoHeader64*    header;
    MachoLoadCommand* cmd;
    vaddr             text_base;
    vaddr             kc_base;
    vaddr             kc_end;
    int               i;

    header = (MachoHeader64*)data;
    assert_cmphex(header->magic, ==, MACH_MAGIC_64);

    apple_boot_get_kc_bounds(header, &text_base, &kc_base, &kc_end, NULL, NULL);
    assert_cmphex(kc_base, <, kc_end);

    phys_base = g_malloc0(kc_end - kc_base);

    cmd = (MachoLoadCommand*)(header + 1);
    for (i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd != LC_SEGMENT_64) { continue; }

        MachoSegmentCommand64* segCmd = (MachoSegmentCommand64*)cmd;
        if (strncmp(segCmd->segname, "__PAGEZERO", 10) == 0 || segCmd->vmsize == 0) { continue; }
        assert_cmphex(segCmd->fileoff, <, len);
        memcpy(phys_base + (segCmd->vmaddr - kc_base), data + segCmd->fileoff, segCmd->filesize);
    }

    return (MachoHeader64*)(phys_base + text_base - kc_base);
}

uint32_t apple_boot_build_version(MachoHeader64* header)
{
    MachoLoadCommand* cmd;
    int               index;

    if (header->file_type == MH_FILESET) { header = apple_boot_get_fileset_header(header, "com.apple.kernel"); }
    cmd = (MachoLoadCommand*)(header + 1);

    for (index = 0; index < header->n_cmds; index++) {
        switch (cmd->cmd) {
            case LC_BUILD_VERSION: {
                return ((MachoBuildVersionCommand*)cmd)->sdk;
            }

            default: break;
        }

        cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size);
    }
    return 0;
}

uint32_t apple_boot_platform(MachoHeader64* header)
{
    MachoLoadCommand* cmd;
    uint32_t          i;

    if (header->file_type == MH_FILESET) { header = apple_boot_get_fileset_header(header, "com.apple.kernel"); }

    cmd = (MachoLoadCommand*)(header + 1);

    for (i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd == LC_BUILD_VERSION) {
            MachoBuildVersionCommand* buildVerCmd = (MachoBuildVersionCommand*)cmd;
            return buildVerCmd->platform;
        }
    }

    return 0;
}

const char* apple_boot_platform_string(MachoHeader64* header)
{
    switch (apple_boot_platform(header)) {
        case PLATFORM_MACOS               : return "macOS";
        case PLATFORM_IOS                 : return "iOS";
        case PLATFORM_TVOS                : return "tvOS";
        case PLATFORM_WATCHOS             : return "watchOS";
        case PLATFORM_BRIDGEOS            : return "bridgeOS";
        case PLATFORM_MAC_CATALYST        : return "Mac Catalyst";
        case PLATFORM_IOS_SIMULATOR       : return "iOS Simulator";
        case PLATFORM_TVOS_SIMULATOR      : return "tvOS Simulator";
        case PLATFORM_WATCHOS_SIMULATOR   : return "watchOS Simulator";
        case PLATFORM_DRIVERKIT           : return "DriverKit";
        case PLATFORM_VISIONOS            : return "visionOS";
        case PLATFORM_VISIONOS_SIMULATOR  : return "visionOS Simulator";
        case PLATFORM_FIRMWARE            : return "Firmware";
        case PLATFORM_SEPOS               : return "sepOS";
        case PLATFORM_MACOS_EXCLAVECORE   : return "macOS ExclaveCore";
        case PLATFORM_MACOS_EXCLAVEKIT    : return "macOS ExclaveKit";
        case PLATFORM_IOS_EXCLAVECORE     : return "iOS ExclaveCore";
        case PLATFORM_IOS_EXCLAVEKIT      : return "iOS ExclaveKit";
        case PLATFORM_TVOS_EXCLAVECORE    : return "tvOS ExclaveCore";
        case PLATFORM_TVOS_EXCLAVEKIT     : return "tvOS ExclaveKit";
        case PLATFORM_WATCHOS_EXCLAVECORE : return "watchOS ExclaveCore";
        case PLATFORM_WATCHOS_EXCLAVEKIT  : return "watchOS ExclaveKit";
        case PLATFORM_VISIONOS_EXCLAVECORE: return "visionOS ExclaveCore";
        case PLATFORM_VISIONOS_EXCLAVEKIT : return "visionOS ExclaveKit";
        default                           : return "Unknown";
    }
}

vaddr apple_boot_get_entry_point(MachoHeader64* header)
{
    MachoLoadCommand* cmd = (MachoLoadCommand*)(header + 1);

    for (uint32_t i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        // 0x110 for arm64 only.
        if (cmd->cmd == LC_UNIXTHREAD) { return ldq_le_p((uint8_t*)cmd + 0x110); }
    }

    return 0;
}

uint8_t* apple_boot_get_macho_buffer(MachoHeader64* header)
{
    vaddr text_base, kc_base;

    apple_boot_get_kc_bounds(header, &text_base, &kc_base, NULL, NULL, NULL);

    return (uint8_t*)header - text_base + kc_base;
}

MachoFilesetEntryCommand* apple_boot_get_fileset(MachoHeader64* header, const char* entry)
{
    if (header->file_type != MH_FILESET) { return NULL; }

    MachoFilesetEntryCommand* fileset;
    fileset = (MachoFilesetEntryCommand*)((char*)header + sizeof(MachoHeader64));

    for (uint32_t i = 0; i < header->n_cmds;
         i++, fileset = (MachoFilesetEntryCommand*)((char*)fileset + fileset->cmd_size))
    {
        if (fileset->cmd == LC_FILESET_ENTRY) {
            const char* entry_id = (char*)fileset + fileset->entry_id;
            if (strcmp(entry_id, entry) == 0) { return fileset; }
        }
    }

    return NULL;
}

MachoHeader64* apple_boot_get_fileset_header(MachoHeader64* header, const char* entry)
{
    MachoFilesetEntryCommand* fileset = apple_boot_get_fileset(header, entry);
    if (fileset == NULL) { return NULL; }
    return (MachoHeader64*)((char*)header + fileset->file_off);
}

MachoSegmentCommand64* apple_boot_get_segment(MachoHeader64* header, const char* name)
{
    uint32_t               i;
    MachoSegmentCommand64* sgp;

    if (header->file_type == MH_FILESET) {
        return apple_boot_get_segment(apple_boot_get_fileset_header(header, "com.apple.kernel"), name);
    }

    for (sgp = (MachoSegmentCommand64*)(header + 1), i = 0; i < header->n_cmds;
         i++, sgp                                      = (MachoSegmentCommand64*)((char*)sgp + sgp->cmd_size))
    {
        if (sgp->cmd == LC_SEGMENT_64 && strncmp(sgp->segname, name, sizeof(sgp->segname) - 1) == 0) { return sgp; }
    }

    return NULL;
}

MachoSection64* apple_boot_get_section(MachoSegmentCommand64* segment, const char* name)
{
    MachoSection64* sp;
    uint32_t        i;

    for (sp = (MachoSection64*)(segment + 1), i = 0; i < segment->nsects; i++, sp++) {
        if (strncmp(sp->sect_name, name, sizeof(sp->sect_name) - 1) == 0) { return sp; }
    }

    return NULL;
}

void* apple_boot_va_to_ptr(MachoHeader64* header, vaddr va)
{
    vaddr kc_base;

    apple_boot_get_kc_bounds(header, NULL, &kc_base, NULL, NULL, NULL);

    return apple_boot_get_macho_buffer(header) + ((0xFFFF000000000000 | va) - kc_base);
}

bool apple_boot_get_im4p_type(const uint8_t* data, gsize len, char* type)
{
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    const uint8_t* seq;
    const uint8_t* str;
    gsize          n;

    seq = apple_boot_der_next(&p, end, 0x30, &n);
    if (seq == NULL) { return false; }
    p   = seq;
    end = seq + n;

    str = apple_boot_der_next(&p, end, 0x16, &n);
    if (str != NULL && n == 4 && memcmp(str, "IMG4", 4) == 0) {
        seq = apple_boot_der_next(&p, end, 0x30, &n);
        if (seq == NULL) { return false; }
        p   = seq;
        end = seq + n;
        str = apple_boot_der_next(&p, end, 0x16, &n);
    }

    if (str == NULL || n != 4 || memcmp(str, "IM4P", 4) != 0) { return false; }

    str = apple_boot_der_next(&p, end, 0x16, &n);
    if (str == NULL || n != 4) { return false; }

    memcpy(type, str, 4);
    return true;
}

void apple_boot_extract_manifest(const uint8_t* img4_data, uint64_t img4_data_len_max, gchar** out_data,
                                 gsize* out_size)
{
    asn1_node img4_definitions = NULL;
    asn1_node img4;
    char      error_desc[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    int       ret;
    char      magic[4];
    int       len;

    *out_data = NULL;
    *out_size = 0;

    if (asn1_array2tree(img4_definitions_array, &img4_definitions, error_desc) != ASN1_SUCCESS) {
        error_setg(&error_fatal, "ASN.1 parser initialisation failed: `%s`", error_desc);
        return;
    }

    ret = asn1_create_element(img4_definitions, "Image4.IMG4", &img4);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IMG4 element creation failed: %d", ret);
        return;
    }

    unsigned char cls;
    int           tag_len;
    unsigned long tag_num;
    ret = asn1_get_tag_der(img4_data, (int)img4_data_len_max, &cls, &tag_len, &tag_num);
    if (ret != ASN1_SUCCESS) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int  len_len;
    long content_len = asn1_get_length_der(img4_data + tag_len, (int)(img4_data_len_max - tag_len), &len_len);
    if (content_len < 0) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    uint64_t total_len = (uint64_t)tag_len + (uint64_t)len_len + (uint64_t)content_len;
    if (total_len > img4_data_len_max) { return; }

    ret = asn1_der_decoding(&img4, img4_data, (uint32_t)total_len, error_desc);
    if (ret != ASN1_SUCCESS) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    len = 4;
    ret = asn1_read_value(img4, "magic", magic, &len);
    if (ret != ASN1_SUCCESS) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    if (memcmp(magic, "IMG4", 4) != 0) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int start, end;
    ret = asn1_der_decoding_startEnd(img4, img4_data, total_len, "manifest", &start, &end);
    if (ret != ASN1_SUCCESS || start < 0 || end < 0 || start == end) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int           wrapper_tag_len;
    unsigned long wrapper_tag_num;
    ret = asn1_get_tag_der(img4_data + start, (int)(total_len - start), &cls, &wrapper_tag_len, &wrapper_tag_num);
    if (ret != ASN1_SUCCESS) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int  wrapper_len_len;
    long inner_len = asn1_get_length_der(img4_data + start + wrapper_tag_len,
                                         (int)(total_len - start - wrapper_tag_len), &wrapper_len_len);
    if (inner_len < 0) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int inner_start = start + wrapper_tag_len + wrapper_len_len;

    *out_size = inner_len;
    *out_data = g_malloc(*out_size);
    memcpy(*out_data, img4_data + inner_start, *out_size);

    asn1_delete_structure(&img4);
    asn1_delete_structure(&img4_definitions);
}
