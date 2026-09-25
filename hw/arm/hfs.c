/*
 * Minimal HFS+ file reader and in-place writer.
 *
 * Copyright (c) 2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/arm/hfs.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include <zlib.h>

#define HFS_VOLUME_HEADER_OFF (1024)
#define HFS_SIG_PLUS          (0x482B)    // 'H+'
#define HFS_SIG_X             (0x4858)    // 'HX'
#define HFS_EXTENT_COUNT      (8)
#define HFS_ROOT_FOLDER_ID    (2)

#define HFS_MAX_NODE_SIZE (32 * KiB)

#define HFS_REC_FOLDER (1)
#define HFS_REC_FILE   (2)

#define HFS_ATTR_INLINE_DATA (0x10)
#define HFS_UF_COMPRESSED    (0x20)

#define DECMPFS_XATTR          "com.apple.decmpfs"
#define DECMPFS_MAGIC          (0x636D7066)    // 'fpmc', little-endian on disk
#define DECMPFS_TYPE_ZLIB_RSRC (4)
#define DECMPFS_CHUNK_SIZE     (64 * KiB)
#define DECMPFS_CHUNK_RAW      (0xFF)

typedef struct QEMU_PACKED
{
    uint32_t start_block;
    uint32_t block_count;
} HfsExtent;

typedef struct QEMU_PACKED
{
    uint64_t  logical_size;
    uint32_t  clump_size;
    uint32_t  total_blocks;
    HfsExtent extents[HFS_EXTENT_COUNT];
} HfsForkData;

typedef struct QEMU_PACKED
{
    uint16_t    signature;
    uint16_t    version;
    uint32_t    attributes;
    uint32_t    last_mounted_version;
    uint32_t    journal_info_block;
    uint32_t    dates[4];
    uint32_t    file_count;
    uint32_t    folder_count;
    uint32_t    block_size;
    uint32_t    total_blocks;
    uint32_t    free_blocks;
    uint32_t    next_allocation;
    uint32_t    rsrc_clump_size;
    uint32_t    data_clump_size;
    uint32_t    next_catalog_id;
    uint32_t    write_count;
    uint64_t    encodings_bitmap;
    uint32_t    finder_info[8];
    HfsForkData allocation_file;
    HfsForkData extents_file;
    HfsForkData catalog_file;
    HfsForkData attributes_file;
    HfsForkData startup_file;
} HfsVolumeHeader;

typedef struct QEMU_PACKED
{
    uint32_t flink;
    uint32_t blink;
    int8_t   kind;
    uint8_t  height;
    uint16_t num_records;
    uint16_t reserved;
} HfsNodeDescriptor;

typedef struct QEMU_PACKED
{
    uint16_t tree_depth;
    uint32_t root_node;
    uint32_t leaf_records;
    uint32_t first_leaf_node;
    uint32_t last_leaf_node;
    uint16_t node_size;
} HfsBtreeHeader;

typedef struct QEMU_PACKED
{
    int16_t  record_type;
    uint16_t flags;
    uint32_t valence_or_reserved;
    uint32_t id;
} HfsCatalogRecord;

typedef struct QEMU_PACKED
{
    HfsCatalogRecord hdr;
    uint32_t         dates[5];
    uint32_t         owner_id;
    uint32_t         group_id;
    uint8_t          admin_flags;
    uint8_t          owner_flags;
    uint16_t         file_mode;
    uint32_t         special;
    uint8_t          user_info[16];
    uint8_t          finder_info[16];
    uint32_t         text_encoding;
    uint32_t         reserved;
    HfsForkData      data_fork;
    HfsForkData      resource_fork;
} HfsCatalogFile;

typedef struct QEMU_PACKED
{
    uint32_t record_type;
    uint32_t reserved[2];
    uint32_t size;
} HfsAttrData;

typedef struct
{
    uint8_t* image;
    size_t   image_len;
    uint32_t block_size;
} HfsVolume;

typedef struct
{
    HfsVolume vol;
    uint64_t  size;
    HfsExtent extents[HFS_EXTENT_COUNT];
} HfsFork;

/// Returns <0 on error, 0 to continue the walk and >0 to stop it.
typedef int (*HfsRecordFn)(void* opaque, const uint8_t* rec, uint16_t rec_len, Error** errp);

typedef struct
{
    uint32_t       parent;
    const char*    name;
    size_t         name_len;
    bool           found;
    bool           is_file;
    HfsCatalogFile file;
} HfsCatalogLookup;

typedef struct
{
    uint32_t file_id;
    bool     found;
    uint32_t magic;
    uint32_t type;
    uint64_t size;
} HfsDecmpfsLookup;

struct HfsFile
{
    HfsFork  fork;
    uint64_t size;
    bool     compressed;
    uint64_t chunk_table;
    uint32_t chunk_count;
};

static bool hfs_fork_init(const HfsVolume* vol, const HfsForkData* data, HfsFork* fork, Error** errp)
{
    uint64_t capacity = 0;
    uint64_t start;
    uint64_t count;
    uint32_t i;

    fork->vol  = *vol;
    fork->size = be64_to_cpu(data->logical_size);
    for (i = 0; i < HFS_EXTENT_COUNT; i++) {
        start = be32_to_cpu(data->extents[i].start_block);
        count = be32_to_cpu(data->extents[i].block_count);
        if (start + count > vol->image_len / vol->block_size) {
            error_setg(errp, "HFS+ extent 0x%" PRIx64 "+0x%" PRIx64 " is out of bounds", start, count);
            return false;
        }

        fork->extents[i].start_block  = start;
        fork->extents[i].block_count  = count;
        capacity                     += count * (uint64_t)vol->block_size;
    }

    if (capacity < fork->size) {
        error_setg(errp, "HFS+ fork spills into the extents overflow file, which is unsupported");
        return false;
    }

    return true;
}

static bool hfs_fork_io(const HfsFork* fork, uint64_t off, uint64_t len, uint8_t* buf, bool write, Error** errp)
{
    const HfsVolume* vol     = &fork->vol;
    uint64_t         logical = 0;
    uint64_t         done    = 0;
    uint64_t         start;
    uint64_t         end;
    uint64_t         ext_len;
    uint64_t         phys;
    uint32_t         i;

    if (off > fork->size || len > fork->size - off) {
        error_setg(errp, "HFS+ fork access at 0x%" PRIx64 "+0x%" PRIx64 " is out of bounds", off, len);
        return false;
    }

    for (i = 0; i < HFS_EXTENT_COUNT; i++) {
        ext_len = fork->extents[i].block_count * (uint64_t)vol->block_size;
        start   = MAX(off, logical);
        end     = MIN(off + len, logical + ext_len);
        if (start < end) {
            phys = fork->extents[i].start_block * (uint64_t)vol->block_size + (start - logical);
            if (write) { memcpy(vol->image + phys, buf + (start - off), end - start); }
            else {
                memcpy(buf + (start - off), vol->image + phys, end - start);
            }
            done += end - start;
        }
        logical += ext_len;
    }

    if (done != len) {
        error_setg(errp, "HFS+ fork access at 0x%" PRIx64 "+0x%" PRIx64 " hits a hole", off, len);
        return false;
    }

    return true;
}

/// Calls `fn` for every leaf record of the B-tree in `fork`, in key order.
static int hfs_btree_walk(HfsFork* fork, HfsRecordFn fn, void* opaque, Error** errp)
{
    g_autofree uint8_t*      node = NULL;
    HfsBtreeHeader           bthdr;
    const HfsNodeDescriptor* desc;
    uint32_t                 node_size;
    uint32_t                 cur;
    uint32_t                 num;
    uint32_t                 i;
    uint16_t                 rec_off;
    uint16_t                 rec_end;
    int                      ret;

    // The B-tree header record lives right after node 0's descriptor.
    if (!hfs_fork_io(fork, sizeof(HfsNodeDescriptor), sizeof(bthdr), (uint8_t*)&bthdr, false, errp)) { return -1; }

    node_size = be16_to_cpu(bthdr.node_size);
    if (node_size < sizeof(HfsNodeDescriptor) || node_size > HFS_MAX_NODE_SIZE || (node_size & (node_size - 1)) != 0) {
        error_setg(errp, "HFS+ B-tree node size 0x%x is invalid", node_size);
        return -1;
    }

    node = g_malloc(node_size);
    desc = (const HfsNodeDescriptor*)node;
    cur  = be32_to_cpu(bthdr.first_leaf_node);
    while (cur != 0) {
        if (!hfs_fork_io(fork, (uint64_t)cur * node_size, node_size, node, false, errp)) { return -1; }

        if (desc->kind != -1) {
            error_setg(errp, "HFS+ B-tree node 0x%x is not a leaf", cur);
            return -1;
        }

        num = be16_to_cpu(desc->num_records);
        for (i = 0; i < num; i++) {
            rec_off = lduw_be_p(node + node_size - 2 * (i + 1));
            rec_end = lduw_be_p(node + node_size - 2 * (i + 2));
            if (rec_off >= rec_end || rec_end > node_size - 2 * (num + 1)) {
                error_setg(errp, "HFS+ B-tree node 0x%x record %u is corrupt", cur, i);
                return -1;
            }

            ret = fn(opaque, node + rec_off, rec_end - rec_off, errp);
            if (ret != 0) { return ret; }
        }

        cur = be32_to_cpu(desc->flink);
    }

    return 0;
}

/// Compares an HFS+ big-endian UTF-16 name against an ASCII component, case-insensitively.
static bool hfs_name_eq(const uint8_t* uni, uint16_t uni_len, const char* name, size_t name_len)
{
    uint16_t i;

    if (uni_len != name_len) { return false; }

    for (i = 0; i < uni_len; i++) {
        if (uni[i * 2] != 0) { return false; }
        if (g_ascii_tolower(uni[i * 2 + 1]) != g_ascii_tolower((uint8_t)name[i])) { return false; }
    }

    return true;
}

static int hfs_catalog_lookup_fn(void* opaque, const uint8_t* rec, uint16_t rec_len, Error** errp)
{
    HfsCatalogLookup*       lookup = opaque;
    uint16_t                key_len;
    uint32_t                parent;
    uint16_t                name_len;
    uint32_t                data_off;
    const HfsCatalogRecord* cat;

    if (rec_len < 8) { goto corrupt; }

    key_len  = lduw_be_p(rec);
    parent   = ldl_be_p(rec + 2);
    name_len = lduw_be_p(rec + 6);
    // keyLength counts the bytes after the field itself; records are even-aligned.
    data_off = ROUND_UP(2 + key_len, 2);
    if (8 + (uint32_t)name_len * 2 > 2 + key_len || data_off + sizeof(HfsCatalogRecord) > rec_len) { goto corrupt; }

    if (parent != lookup->parent || !hfs_name_eq(rec + 8, name_len, lookup->name, lookup->name_len)) { return 0; }

    cat = (const HfsCatalogRecord*)(rec + data_off);
    switch (be16_to_cpu(cat->record_type)) {
        case HFS_REC_FOLDER:
            lookup->is_file     = false;
            lookup->file.hdr.id = cat->id;
            lookup->found       = true;
            return 1;
        case HFS_REC_FILE:
            if (data_off + sizeof(HfsCatalogFile) > rec_len) { goto corrupt; }
            lookup->is_file = true;
            memcpy(&lookup->file, cat, sizeof(HfsCatalogFile));
            lookup->found = true;
            return 1;
        default: return 0;
    }

corrupt:
    error_setg(errp, "HFS+ catalog record is corrupt");
    return -1;
}

static int hfs_decmpfs_lookup_fn(void* opaque, const uint8_t* rec, uint16_t rec_len, Error** errp)
{
    HfsDecmpfsLookup*  lookup = opaque;
    uint16_t           key_len;
    uint32_t           file_id;
    uint16_t           name_len;
    uint32_t           data_off;
    const HfsAttrData* attr;
    const uint8_t*     value;

    // Attribute key: keyLength(2), pad(2), fileID(4), startBlock(4), nameLength(2), name.
    if (rec_len < 14) { goto corrupt; }

    key_len  = lduw_be_p(rec);
    file_id  = ldl_be_p(rec + 4);
    name_len = lduw_be_p(rec + 12);
    data_off = ROUND_UP(2 + key_len, 2);
    if (14 + (uint32_t)name_len * 2 > 2 + key_len || data_off > rec_len) { goto corrupt; }

    if (file_id != lookup->file_id) { return 0; }
    if (!hfs_name_eq(rec + 14, name_len, DECMPFS_XATTR, strlen(DECMPFS_XATTR))) { return 0; }

    if (data_off + sizeof(HfsAttrData) + sizeof(uint32_t) * 2 + sizeof(uint64_t) > rec_len) { goto corrupt; }

    attr = (const HfsAttrData*)(rec + data_off);
    if (be32_to_cpu(attr->record_type) != HFS_ATTR_INLINE_DATA) { goto corrupt; }

    value         = (const uint8_t*)attr + sizeof(HfsAttrData);
    lookup->magic = ldl_le_p(value);
    lookup->type  = ldl_le_p(value + 4);
    lookup->size  = ldq_le_p(value + 8);
    lookup->found = true;
    return 1;

corrupt:
    error_setg(errp, "HFS+ decmpfs attribute is corrupt");
    return -1;
}

static bool hfs_lookup_path(HfsFork* catalog, const char* path, HfsCatalogFile* out, Error** errp)
{
    g_auto(GStrv) components = g_strsplit(path, "/", -1);
    HfsCatalogLookup lookup  = {0};
    uint32_t         parent  = HFS_ROOT_FOLDER_ID;
    char**           component;

    for (component = components; *component != NULL; component++) {
        if (**component == '\0') { continue; }

        if (lookup.is_file) {
            error_setg(errp, "`%s`: not a directory", path);
            return false;
        }

        lookup.parent   = parent;
        lookup.name     = *component;
        lookup.name_len = strlen(*component);
        lookup.found    = false;
        if (hfs_btree_walk(catalog, hfs_catalog_lookup_fn, &lookup, errp) < 0) { return false; }

        if (!lookup.found) {
            error_setg(errp, "`%s`: no such file or directory", path);
            return false;
        }

        parent = be32_to_cpu(lookup.file.hdr.id);
    }

    if (!lookup.is_file) {
        error_setg(errp, "`%s`: not a file", path);
        return false;
    }

    *out = lookup.file;
    return true;
}

HfsFile* hfs_open_file(uint8_t* image, size_t image_len, const char* path, Error** errp)
{
    g_autoptr(HfsFile) file = NULL;
    HfsVolumeHeader  vh;
    HfsVolume        vol;
    HfsFork          catalog;
    HfsFork          attributes;
    HfsCatalogFile   cat;
    HfsDecmpfsLookup decmpfs = {0};
    uint16_t         sig;
    uint8_t          rsrc_hdr[16];
    uint64_t         chunk_table;

    if (image_len < HFS_VOLUME_HEADER_OFF + sizeof(vh)) {
        error_setg(errp, "HFS+ image is too small");
        return NULL;
    }

    memcpy(&vh, image + HFS_VOLUME_HEADER_OFF, sizeof(vh));
    sig = be16_to_cpu(vh.signature);
    if (sig != HFS_SIG_PLUS && sig != HFS_SIG_X) {
        error_setg(errp, "Not an HFS+ or HFSX volume (signature 0x%04x)", sig);
        return NULL;
    }

    vol.image      = image;
    vol.image_len  = image_len;
    vol.block_size = be32_to_cpu(vh.block_size);
    if (vol.block_size == 0 || (vol.block_size & (vol.block_size - 1)) != 0) {
        error_setg(errp, "HFS+ block size 0x%x is invalid", vol.block_size);
        return NULL;
    }

    if (!hfs_fork_init(&vol, &vh.catalog_file, &catalog, errp)) { return NULL; }

    if (!hfs_lookup_path(&catalog, path, &cat, errp)) { return NULL; }

    file = g_new0(HfsFile, 1);

    if (!(cat.owner_flags & HFS_UF_COMPRESSED)) {
        if (!hfs_fork_init(&vol, &cat.data_fork, &file->fork, errp)) { return NULL; }
        file->size = file->fork.size;
        return g_steal_pointer(&file);
    }

    if (!hfs_fork_init(&vol, &vh.attributes_file, &attributes, errp)) { return NULL; }

    decmpfs.file_id = be32_to_cpu(cat.hdr.id);
    if (hfs_btree_walk(&attributes, hfs_decmpfs_lookup_fn, &decmpfs, errp) < 0) { return NULL; }
    if (!decmpfs.found) {
        error_setg(errp, "`%s` is compressed but has no decmpfs attribute", path);
        return NULL;
    }
    if (decmpfs.magic != DECMPFS_MAGIC) {
        error_setg(errp, "`%s` has a bad decmpfs magic 0x%08x", path, decmpfs.magic);
        return NULL;
    }
    if (decmpfs.type != DECMPFS_TYPE_ZLIB_RSRC) {
        error_setg(errp, "`%s` uses unsupported decmpfs compression type %u", path, decmpfs.type);
        return NULL;
    }

    if (!hfs_fork_init(&vol, &cat.resource_fork, &file->fork, errp)) { return NULL; }

    // Resource fork header (big-endian): dataOffset, mapOffset, dataLength, mapLength.
    // The chunk table sits right after the leading 4-byte length at dataOffset.
    if (!hfs_fork_io(&file->fork, 0, sizeof(rsrc_hdr), rsrc_hdr, false, errp)) { return NULL; }
    chunk_table = ldl_be_p(rsrc_hdr) + 4;

    if (chunk_table + 4 > file->fork.size) {
        error_setg(errp, "`%s` resource fork is too small for a chunk table", path);
        return NULL;
    }

    file->compressed  = true;
    file->size        = decmpfs.size;
    file->chunk_table = chunk_table;
    file->chunk_count = (decmpfs.size + DECMPFS_CHUNK_SIZE - 1) / DECMPFS_CHUNK_SIZE;
    return g_steal_pointer(&file);
}

void hfs_file_free(HfsFile* file) { g_free(file); }

uint64_t hfs_file_size(const HfsFile* file) { return file->size; }

static bool hfs_chunk_entry(HfsFile* file, uint32_t idx, uint64_t* off, uint32_t* size, Error** errp)
{
    uint8_t entry[8];

    if (idx >= file->chunk_count) {
        error_setg(errp, "HFS+ decmpfs chunk %u is out of bounds", idx);
        return false;
    }

    if (!hfs_fork_io(&file->fork, file->chunk_table + 4 + (uint64_t)idx * 8, sizeof(entry), entry, false, errp)) {
        return false;
    }

    *off  = file->chunk_table + ldl_le_p(entry);
    *size = ldl_le_p(entry + 4);
    if (*off + *size > file->fork.size) {
        error_setg(errp, "HFS+ decmpfs chunk %u overruns the resource fork", idx);
        return false;
    }

    return true;
}

static bool hfs_chunk_read(HfsFile* file, uint32_t idx, uint8_t* out, uint64_t out_len, Error** errp)
{
    g_autofree uint8_t* comp = NULL;
    uint64_t            off;
    uint32_t            size;
    uLongf              dst_len;

    if (!hfs_chunk_entry(file, idx, &off, &size, errp)) { return false; }

    comp = g_malloc(size);
    if (!hfs_fork_io(&file->fork, off, size, comp, false, errp)) { return false; }

    if (size > 0 && comp[0] == DECMPFS_CHUNK_RAW) {
        if (size - 1 != out_len) {
            error_setg(errp, "HFS+ decmpfs raw chunk %u has the wrong size", idx);
            return false;
        }
        memcpy(out, comp + 1, out_len);
        return true;
    }

    dst_len = out_len;
    if (uncompress(out, &dst_len, comp, size) != Z_OK || dst_len != out_len) {
        error_setg(errp, "HFS+ decmpfs chunk %u failed to decompress", idx);
        return false;
    }

    return true;
}

static bool hfs_chunk_write(HfsFile* file, uint32_t idx, const uint8_t* in, uint64_t in_len, Error** errp)
{
    g_autofree uint8_t* comp = NULL;
    uint64_t            off;
    uint32_t            old_size;
    uLongf              comp_len;
    uint8_t             entry_size[4];

    if (!hfs_chunk_entry(file, idx, &off, &old_size, errp)) { return false; }

    comp_len = compressBound(in_len);
    comp     = g_malloc(comp_len);
    if (compress2(comp, &comp_len, in, in_len, Z_BEST_COMPRESSION) != Z_OK) {
        error_setg(errp, "HFS+ decmpfs chunk %u failed to compress", idx);
        return false;
    }

    if (comp_len > in_len) {
        comp[0] = DECMPFS_CHUNK_RAW;
        memcpy(comp + 1, in, in_len);
        comp_len = in_len + 1;
    }

    if (comp_len > old_size) {
        error_setg(errp, "HFS+ decmpfs chunk %u grew from 0x%x to 0x%lx bytes, cannot patch in place", idx, old_size,
                   comp_len);
        return false;
    }

    if (!hfs_fork_io(&file->fork, off, comp_len, comp, true, errp)) { return false; }

    stl_le_p(entry_size, comp_len);
    return hfs_fork_io(&file->fork, file->chunk_table + 4 + (uint64_t)idx * 8 + 4, sizeof(entry_size), entry_size, true,
                       errp);
}

static bool hfs_file_io(HfsFile* file, uint64_t off, uint64_t len, uint8_t* buf, bool write, Error** errp)
{
    g_autofree uint8_t* chunk = NULL;
    uint64_t            pos;
    uint64_t            end = off + len;
    uint32_t            idx;
    uint64_t            chunk_start;
    uint64_t            chunk_len;
    uint64_t            local;
    uint64_t            n;

    if (off > file->size || len > file->size - off) {
        error_setg(errp, "HFS+ file access at 0x%" PRIx64 "+0x%" PRIx64 " is out of bounds", off, len);
        return false;
    }

    if (!file->compressed) { return hfs_fork_io(&file->fork, off, len, buf, write, errp); }

    chunk = g_malloc(DECMPFS_CHUNK_SIZE);
    for (pos = off; pos < end; pos += n) {
        idx         = pos / DECMPFS_CHUNK_SIZE;
        chunk_start = (uint64_t)idx * DECMPFS_CHUNK_SIZE;
        chunk_len   = MIN(DECMPFS_CHUNK_SIZE, file->size - chunk_start);
        local       = pos - chunk_start;
        n           = MIN(end - pos, chunk_len - local);

        if (!hfs_chunk_read(file, idx, chunk, chunk_len, errp)) { return false; }

        if (write) {
            memcpy(chunk + local, buf + (pos - off), n);
            if (!hfs_chunk_write(file, idx, chunk, chunk_len, errp)) { return false; }
        }
        else {
            memcpy(buf + (pos - off), chunk + local, n);
        }
    }

    return true;
}

bool hfs_file_pread(HfsFile* file, uint64_t off, uint64_t len, void* buf, Error** errp)
{ return hfs_file_io(file, off, len, buf, false, errp); }

bool hfs_file_pwrite(HfsFile* file, uint64_t off, uint64_t len, const void* buf, Error** errp)
{ return hfs_file_io(file, off, len, (uint8_t*)buf, true, errp); }
