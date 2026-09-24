/*
 * Minimal read-only APFS reader.
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
#include "hw/arm/apfs.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "system/block-backend.h"

#define GPT_LBA_SIZE    (4096)
#define APFS_BLOCK_SIZE (4096)

#define APFS_OBJ_TYPE_MASK          (0xFFFF)
#define APFS_OBJ_TYPE_NX_SUPERBLOCK (0x1)

#define APFS_NX_MAX_FILE_SYSTEMS (100)

#define APFS_BTNODE_ROOT          (1 << 0)
#define APFS_BTNODE_LEAF          (1 << 1)
#define APFS_BTNODE_FIXED_KV_SIZE (1 << 2)
#define APFS_BTREE_INFO_SIZE      (40)
#define APFS_BTREE_MAX_LEVEL      (31)

#define APFS_OMAP_VAL_DELETED (1 << 0)

#define APFS_OBJ_ID_MASK      (0x0FFFFFFFFFFFFFFFULL)
#define APFS_OBJ_TYPE_SHIFT   (60)
#define APFS_TYPE_INODE       (3)
#define APFS_TYPE_FILE_EXTENT (8)
#define APFS_TYPE_DIR_REC     (9)

#define APFS_ROOT_DIR_INO (2)

#define APFS_DREC_LEN_MASK (0x3FF)

#define APFS_INODE_VAL_SIZE       (92)
#define APFS_INO_EXT_TYPE_DSTREAM (8)
#define APFS_FILE_EXTENT_LEN_MASK (0x00FFFFFFFFFFFFFFULL)

static const uint8_t gpt_apfs_type_guid[16] = {
    0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC,
};

typedef struct QEMU_PACKED
{
    char     signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc;
} GptHeader;

typedef struct QEMU_PACKED
{
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} GptEntry;

typedef struct QEMU_PACKED
{
    uint64_t cksum;
    uint64_t oid;
    uint64_t xid;
    uint32_t type;
    uint32_t subtype;
} ApfsObjPhys;

typedef struct QEMU_PACKED
{
    ApfsObjPhys o;
    uint32_t    magic;
    uint32_t    block_size;
    uint64_t    block_count;
    uint64_t    features;
    uint64_t    ro_compat_features;
    uint64_t    incompat_features;
    uint8_t     uuid[16];
    uint64_t    next_oid;
    uint64_t    next_xid;
    uint32_t    xp_desc_blocks;
    uint32_t    xp_data_blocks;
    uint64_t    xp_desc_base;
    uint64_t    xp_data_base;
    uint32_t    xp_desc_next;
    uint32_t    xp_data_next;
    uint32_t    xp_desc_index;
    uint32_t    xp_desc_len;
    uint32_t    xp_data_index;
    uint32_t    xp_data_len;
    uint64_t    spaceman_oid;
    uint64_t    omap_oid;
    uint64_t    reaper_oid;
    uint32_t    test_type;
    uint32_t    max_file_systems;
    uint64_t    fs_oid[APFS_NX_MAX_FILE_SYSTEMS];
} ApfsNxSuperblock;

typedef struct QEMU_PACKED
{
    ApfsObjPhys o;
    uint32_t    flags;
    uint32_t    snap_count;
    uint32_t    tree_type;
    uint32_t    snapshot_tree_type;
    uint64_t    tree_oid;
} ApfsOmapPhys;

typedef struct QEMU_PACKED
{
    ApfsObjPhys o;
    uint8_t     unused0[96];
    uint64_t    omap_oid;
    uint64_t    root_tree_oid;
    uint8_t     unused1[820];
    uint16_t    role;
} ApfsSuperblock;

typedef struct QEMU_PACKED
{
    ApfsObjPhys o;
    uint16_t    flags;
    uint16_t    level;
    uint32_t    nkeys;
    uint16_t    table_space_off;
    uint16_t    table_space_len;
    uint16_t    free_space_off;
    uint16_t    free_space_len;
    uint16_t    key_free_list_off;
    uint16_t    key_free_list_len;
    uint16_t    val_free_list_off;
    uint16_t    val_free_list_len;
} ApfsBtreeNodePhys;

typedef struct
{
    BlockBackend* blk;
    uint64_t      base;
    uint64_t      xid;
    uint64_t      omap_tree;
} ApfsContext;

typedef struct
{
    const uint8_t* key;
    uint32_t       key_len;
    const uint8_t* val;
    uint32_t       val_len;
} ApfsEntry;

/// Returns <0 on error, 0 to continue the walk and >0 to stop it.
typedef int (*ApfsRecordFn)(void* opaque, const ApfsEntry* entry, Error** errp);

typedef struct
{
    const char* name;
    size_t      name_len;
    bool        found;
    uint64_t    file_id;
} ApfsDirLookup;

typedef struct
{
    uint64_t private_id;
    uint64_t size;
} ApfsInode;

typedef struct
{
    ApfsContext* ctx;
    uint8_t*     data;
    uint64_t     size;
} ApfsExtentRead;

static uint64_t apfs_fletcher64(const uint8_t* block)
{
    uint64_t sum1 = 0;
    uint64_t sum2 = 0;
    uint64_t c1;
    uint64_t c2;
    uint32_t i;

    for (i = sizeof(uint64_t); i < APFS_BLOCK_SIZE; i += sizeof(uint32_t)) {
        sum1 = (sum1 + (uint32_t)ldl_le_p(block + i)) % UINT32_MAX;
        sum2 = (sum2 + sum1) % UINT32_MAX;
    }

    c1 = UINT32_MAX - ((sum1 + sum2) % UINT32_MAX);
    c2 = UINT32_MAX - ((sum1 + c1) % UINT32_MAX);
    return (c2 << 32) | c1;
}

static bool apfs_read(ApfsContext* ctx, uint64_t off, uint64_t len, void* buf, Error** errp)
{
    int ret = blk_pread(ctx->blk, ctx->base + off, len, buf, 0);

    if (ret < 0) {
        error_setg_errno(errp, -ret, "APFS read at 0x%" PRIx64 " failed", ctx->base + off);
        return false;
    }

    return true;
}

static bool apfs_read_block(ApfsContext* ctx, uint64_t paddr, uint8_t* buf, Error** errp)
{ return apfs_read(ctx, paddr * APFS_BLOCK_SIZE, APFS_BLOCK_SIZE, buf, errp); }

static bool apfs_node_entry(const uint8_t* node, uint32_t index, ApfsEntry* entry, Error** errp)
{
    const ApfsBtreeNodePhys* hdr            = (const ApfsBtreeNodePhys*)node;
    uint16_t                 flags          = le16_to_cpu(hdr->flags);
    uint32_t                 toc            = sizeof(*hdr) + le16_to_cpu(hdr->table_space_off);
    uint32_t                 keys           = toc + le16_to_cpu(hdr->table_space_len);
    uint32_t                 vals           = APFS_BLOCK_SIZE - ((flags & APFS_BTNODE_ROOT) ? APFS_BTREE_INFO_SIZE : 0);
    uint32_t                 toc_entry_size = (flags & APFS_BTNODE_FIXED_KV_SIZE) ? 4 : 8;
    const uint8_t*           toc_entry;
    uint32_t                 k_off;
    uint32_t                 k_len;
    uint32_t                 v_off;
    uint32_t                 v_len;

    if (keys > vals || toc + ((uint64_t)index + 1) * toc_entry_size > keys) { goto corrupt; }

    toc_entry = node + toc + index * toc_entry_size;
    if (flags & APFS_BTNODE_FIXED_KV_SIZE) {
        k_off = lduw_le_p(toc_entry);
        v_off = lduw_le_p(toc_entry + 2);
        k_len = 16;
        v_len = (flags & APFS_BTNODE_LEAF) ? 16 : sizeof(uint64_t);
    }
    else {
        k_off = lduw_le_p(toc_entry);
        k_len = lduw_le_p(toc_entry + 2);
        v_off = lduw_le_p(toc_entry + 4);
        v_len = lduw_le_p(toc_entry + 6);
    }

    if (k_len < sizeof(uint64_t) || keys + k_off + k_len > vals || v_off > vals || v_len > v_off) { goto corrupt; }

    entry->key     = node + keys + k_off;
    entry->key_len = k_len;
    entry->val     = node + vals - v_off;
    entry->val_len = v_len;
    return true;

corrupt:
    error_setg(errp, "APFS B-tree node 0x%" PRIx64 " is corrupt", le64_to_cpu(hdr->o.oid));
    return false;
}

static bool apfs_node_level_valid(const uint8_t* node, uint16_t max_level)
{
    const ApfsBtreeNodePhys* hdr   = (const ApfsBtreeNodePhys*)node;
    uint16_t                 level = le16_to_cpu(hdr->level);

    return level <= max_level && (level == 0) == !!(le16_to_cpu(hdr->flags) & APFS_BTNODE_LEAF);
}

static bool apfs_omap_lookup(ApfsContext* ctx, uint64_t oid, uint64_t* paddr, Error** errp)
{
    uint8_t                  node[APFS_BLOCK_SIZE];
    const ApfsBtreeNodePhys* hdr       = (const ApfsBtreeNodePhys*)node;
    uint64_t                 node_addr = ctx->omap_tree;
    uint16_t                 max_level = APFS_BTREE_MAX_LEVEL;
    ApfsEntry                entry;
    ApfsEntry                found;
    bool                     have;
    uint32_t                 nkeys;
    uint32_t                 i;
    uint64_t                 key_oid;

    for (;;) {
        if (!apfs_read_block(ctx, node_addr, node, errp)) { return false; }

        if (!apfs_node_level_valid(node, max_level)) {
            error_setg(errp, "APFS object map node 0x%" PRIx64 " is corrupt", node_addr);
            return false;
        }

        nkeys = le32_to_cpu(hdr->nkeys);
        have  = false;
        for (i = 0; i < nkeys; i++) {
            if (!apfs_node_entry(node, i, &entry, errp)) { return false; }

            key_oid = ldq_le_p(entry.key);
            if (key_oid > oid || (key_oid == oid && ldq_le_p(entry.key + 8) > ctx->xid)) { break; }

            found = entry;
            have  = true;
        }

        if (!have) { break; }

        if (le16_to_cpu(hdr->level) == 0) {
            if (ldq_le_p(found.key) != oid || (ldl_le_p(found.val) & APFS_OMAP_VAL_DELETED)) { break; }

            *paddr = ldq_le_p(found.val + 8);
            return true;
        }

        max_level = le16_to_cpu(hdr->level) - 1;
        node_addr = ldq_le_p(found.val);
    }

    error_setg(errp, "APFS object 0x%" PRIx64 " not found", oid);
    return false;
}

static uint64_t apfs_key_order(const uint8_t* key)
{
    uint64_t hdr = ldq_le_p(key);

    return ((hdr & APFS_OBJ_ID_MASK) << 4) | (hdr >> APFS_OBJ_TYPE_SHIFT);
}

/// Calls `fn` for every record of `obj_id` with `type`, in key order.
static int apfs_fs_walk(ApfsContext* ctx, uint64_t node_oid, uint16_t max_level, uint64_t obj_id, uint8_t type,
                        ApfsRecordFn fn, void* opaque, Error** errp)
{
    uint8_t                  node[APFS_BLOCK_SIZE];
    const ApfsBtreeNodePhys* hdr   = (const ApfsBtreeNodePhys*)node;
    uint64_t                 order = (obj_id << 4) | type;
    uint64_t                 paddr;
    uint64_t                 key_order;
    uint32_t                 nkeys;
    uint32_t                 i;
    ApfsEntry                entry;
    ApfsEntry                next;
    int                      ret;

    if (!apfs_omap_lookup(ctx, node_oid, &paddr, errp) || !apfs_read_block(ctx, paddr, node, errp)) { return -1; }

    if (!apfs_node_level_valid(node, max_level)) {
        error_setg(errp, "APFS file system node 0x%" PRIx64 " is corrupt", node_oid);
        return -1;
    }

    nkeys = le32_to_cpu(hdr->nkeys);
    for (i = 0; i < nkeys; i++) {
        if (!apfs_node_entry(node, i, &entry, errp)) { return -1; }

        key_order = apfs_key_order(entry.key);
        if (key_order > order) { return 1; }

        if (le16_to_cpu(hdr->level) == 0) {
            if (key_order == order) {
                ret = fn(opaque, &entry, errp);
                if (ret != 0) { return ret; }
            }
            continue;
        }

        if (i + 1 < nkeys) {
            if (!apfs_node_entry(node, i + 1, &next, errp)) { return -1; }
            if (apfs_key_order(next.key) < order) { continue; }
        }

        if (entry.val_len < sizeof(uint64_t)) {
            error_setg(errp, "APFS file system node 0x%" PRIx64 " is corrupt", node_oid);
            return -1;
        }

        ret = apfs_fs_walk(ctx, ldq_le_p(entry.val), le16_to_cpu(hdr->level) - 1, obj_id, type, fn, opaque, errp);
        if (ret != 0) { return ret; }
    }

    return 0;
}

static int apfs_dir_lookup_fn(void* opaque, const ApfsEntry* entry, Error** errp)
{
    ApfsDirLookup* lookup = opaque;
    uint32_t       name_len;

    if (entry->key_len < 12) { goto corrupt; }

    name_len = ldl_le_p(entry->key + 8) & APFS_DREC_LEN_MASK;
    if (12 + name_len > entry->key_len) { goto corrupt; }

    if (name_len != lookup->name_len + 1 || memcmp(entry->key + 12, lookup->name, lookup->name_len) != 0) { return 0; }

    if (entry->val_len < sizeof(uint64_t)) { goto corrupt; }

    lookup->file_id = ldq_le_p(entry->val);
    lookup->found   = true;
    return 1;

corrupt:
    error_setg(errp, "APFS directory record is corrupt");
    return -1;
}

static int apfs_inode_fn(void* opaque, const ApfsEntry* entry, Error** errp)
{
    ApfsInode*     inode = opaque;
    const uint8_t* xf;
    const uint8_t* xval;
    uint32_t       rest;
    uint32_t       count;
    uint32_t       xlen;
    uint32_t       i;

    if (entry->val_len < APFS_INODE_VAL_SIZE) { goto corrupt; }

    inode->private_id = ldq_le_p(entry->val + 8);

    xf   = entry->val + APFS_INODE_VAL_SIZE;
    rest = entry->val_len - APFS_INODE_VAL_SIZE;
    if (rest < 4) { return 1; }

    count = lduw_le_p(xf);
    if (4 + count * 4 > rest) { goto corrupt; }

    xval  = xf + 4 + count * 4;
    rest -= 4 + count * 4;
    for (i = 0; i < count; i++) {
        xlen = ROUND_UP(lduw_le_p(xf + 4 + i * 4 + 2), 8);
        if (xlen > rest) { goto corrupt; }

        if (xf[4 + i * 4] == APFS_INO_EXT_TYPE_DSTREAM) {
            if (xlen < sizeof(uint64_t)) { goto corrupt; }
            inode->size = ldq_le_p(xval);
            break;
        }

        xval += xlen;
        rest -= xlen;
    }

    return 1;

corrupt:
    error_setg(errp, "APFS inode record is corrupt");
    return -1;
}

static int apfs_extent_fn(void* opaque, const ApfsEntry* entry, Error** errp)
{
    ApfsExtentRead* extents = opaque;
    uint64_t        logical;
    uint64_t        len;
    uint64_t        paddr;

    if (entry->key_len < 16 || entry->val_len < 16) {
        error_setg(errp, "APFS file extent record is corrupt");
        return -1;
    }

    logical = ldq_le_p(entry->key + 8);
    len     = ldq_le_p(entry->val) & APFS_FILE_EXTENT_LEN_MASK;
    paddr   = ldq_le_p(entry->val + 8);

    if (logical >= extents->size) { return 0; }

    len = MIN(len, extents->size - logical);
    if (paddr != 0 && !apfs_read(extents->ctx, paddr * APFS_BLOCK_SIZE, len, extents->data + logical, errp)) {
        return -1;
    }

    return 0;
}

static bool apfs_find_container(ApfsContext* ctx, Error** errp)
{
    g_autofree uint8_t* entries = NULL;
    GptHeader           hdr;
    const GptEntry*     entry;
    uint32_t            num_entries;
    uint32_t            entry_size;
    uint32_t            i;

    if (!apfs_read(ctx, GPT_LBA_SIZE, sizeof(hdr), &hdr, errp)) { return false; }

    if (memcmp(hdr.signature, "EFI PART", sizeof(hdr.signature)) != 0) {
        error_setg(errp, "No GPT found");
        return false;
    }

    num_entries = le32_to_cpu(hdr.num_entries);
    entry_size  = le32_to_cpu(hdr.entry_size);
    if (entry_size < sizeof(GptEntry)) {
        error_setg(errp, "GPT header is corrupt");
        return false;
    }

    entries = g_malloc((size_t)num_entries * entry_size);
    if (!apfs_read(ctx, le64_to_cpu(hdr.entries_lba) * GPT_LBA_SIZE, (uint64_t)num_entries * entry_size, entries, errp))
    {
        return false;
    }

    for (i = 0; i < num_entries; i++) {
        entry = (const GptEntry*)(entries + (size_t)i * entry_size);
        if (memcmp(entry->type_guid, gpt_apfs_type_guid, sizeof(gpt_apfs_type_guid)) == 0) {
            ctx->base = le64_to_cpu(entry->first_lba) * GPT_LBA_SIZE;
            return true;
        }
    }

    error_setg(errp, "No APFS container found in the GPT");
    return false;
}

static bool apfs_load_checkpoint(ApfsContext* ctx, uint8_t* superblock, Error** errp)
{
    const ApfsNxSuperblock* copy = (const ApfsNxSuperblock*)superblock;
    uint64_t                desc_base;
    uint32_t                desc_blocks;
    uint32_t                i;
    uint64_t                xid;
    int64_t                 best = -1;

    if (!apfs_read_block(ctx, 0, superblock, errp)) { return false; }

    desc_base   = le64_to_cpu(copy->xp_desc_base);
    desc_blocks = le32_to_cpu(copy->xp_desc_blocks);
    for (i = 0; i < desc_blocks; i++) {
        if (!apfs_read_block(ctx, desc_base + i, superblock, errp)) { return false; }

        if ((le32_to_cpu(copy->o.type) & APFS_OBJ_TYPE_MASK) != APFS_OBJ_TYPE_NX_SUPERBLOCK
            || ldq_le_p(superblock) != apfs_fletcher64(superblock))
        {
            continue;
        }

        xid = le64_to_cpu(copy->o.xid);
        if (best < 0 || xid > ctx->xid) {
            best     = i;
            ctx->xid = xid;
        }
    }

    if (best < 0) {
        error_setg(errp, "No valid APFS checkpoint found");
        return false;
    }

    return apfs_read_block(ctx, desc_base + best, superblock, errp);
}

static bool apfs_load_omap(ApfsContext* ctx, uint64_t omap_oid, Error** errp)
{
    uint8_t block[APFS_BLOCK_SIZE];

    if (!apfs_read_block(ctx, omap_oid, block, errp)) { return false; }

    ctx->omap_tree = le64_to_cpu(((const ApfsOmapPhys*)block)->tree_oid);
    return true;
}

static bool apfs_mount_volume(ApfsContext* ctx, uint16_t role, uint64_t* root_tree, Error** errp)
{
    uint8_t                 nx_block[APFS_BLOCK_SIZE];
    uint8_t                 block[APFS_BLOCK_SIZE];
    const ApfsNxSuperblock* nx = (const ApfsNxSuperblock*)nx_block;
    const ApfsSuperblock*   fs = (const ApfsSuperblock*)block;
    uint32_t                i;
    uint64_t                oid;
    uint64_t                paddr;

    if (!apfs_load_checkpoint(ctx, nx_block, errp)) { return false; }

    if (!apfs_load_omap(ctx, le64_to_cpu(nx->omap_oid), errp)) { return false; }

    for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
        oid = le64_to_cpu(nx->fs_oid[i]);
        if (oid == 0) { continue; }

        if (!apfs_omap_lookup(ctx, oid, &paddr, errp) || !apfs_read_block(ctx, paddr, block, errp)) { return false; }

        if (le16_to_cpu(fs->role) == role) {
            *root_tree = le64_to_cpu(fs->root_tree_oid);
            return apfs_load_omap(ctx, le64_to_cpu(fs->omap_oid), errp);
        }
    }

    error_setg(errp, "No APFS volume with role 0x%x found", role);
    return false;
}

bool apfs_read_file(BlockBackend* blk, uint16_t role, const char* path, uint8_t** data, gsize* len, Error** errp)
{
    ApfsContext ctx            = {.blk = blk};
    g_auto(GStrv) components   = g_strsplit(path, "/", -1);
    g_autofree uint8_t* buffer = NULL;
    ApfsDirLookup       lookup = {0};
    ApfsInode           inode  = {0};
    ApfsExtentRead      extents;
    uint64_t            root_tree;
    uint64_t            ino = APFS_ROOT_DIR_INO;
    char**              component;

    if (!apfs_find_container(&ctx, errp) || !apfs_mount_volume(&ctx, role, &root_tree, errp)) { return false; }

    for (component = components; *component != NULL; component++) {
        lookup.name     = *component;
        lookup.name_len = strlen(*component);
        lookup.found    = false;
        if (apfs_fs_walk(&ctx, root_tree, APFS_BTREE_MAX_LEVEL, ino, APFS_TYPE_DIR_REC, apfs_dir_lookup_fn, &lookup,
                         errp)
            < 0)
        {
            return false;
        }

        if (!lookup.found) {
            error_setg(errp, "`%s`: no such file or directory", path);
            return false;
        }

        ino = lookup.file_id;
    }

    if (apfs_fs_walk(&ctx, root_tree, APFS_BTREE_MAX_LEVEL, ino, APFS_TYPE_INODE, apfs_inode_fn, &inode, errp) < 0) {
        return false;
    }

    buffer       = g_malloc0(inode.size + 1);
    extents.ctx  = &ctx;
    extents.data = buffer;
    extents.size = inode.size;
    if (apfs_fs_walk(&ctx, root_tree, APFS_BTREE_MAX_LEVEL, inode.private_id, APFS_TYPE_FILE_EXTENT, apfs_extent_fn,
                     &extents, errp)
        < 0)
    {
        return false;
    }

    *data = g_steal_pointer(&buffer);
    *len  = inode.size;
    return true;
}
