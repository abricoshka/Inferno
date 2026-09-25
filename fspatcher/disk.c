/*
 * Disk image access.
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

#include "disk.h"
#include "block/block.h"
#include "crypto/init.h"
#include "hw/arm/apfs.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/block-backend.h"

static char* disk_take_error(Error* local_err)
{
    char* err = g_strdup(error_get_pretty(local_err));

    error_free(local_err);
    return err;
}

BlockBackend* disk_open(const char* filename, char** err)
{
    Error*        local_err = NULL;
    BlockBackend* blk;

    qemu_init_main_loop(&error_fatal);
    qcrypto_init(&error_fatal);
    module_call_init(MODULE_INIT_QOM);
    bdrv_init();

    blk = blk_new_open(filename, NULL, NULL, BDRV_O_RDWR, &local_err);
    if (blk == NULL) { *err = disk_take_error(local_err); }

    return blk;
}

void disk_close(BlockBackend* blk) { blk_unref(blk); }

ApfsFile* disk_file_open(BlockBackend* blk, const char* path, char** err)
{
    Error*    local_err = NULL;
    ApfsFile* file      = apfs_open_file(blk, APFS_VOL_ROLE_SYSTEM, path, &local_err);

    if (file == NULL) { *err = disk_take_error(local_err); }

    return file;
}

ApfsFile* disk_cryptex_open(BlockBackend* blk, char** err)
{
    Error*    local_err = NULL;
    ApfsFile* file      = apfs_open_file(blk, APFS_VOL_ROLE_PREBOOT, "cryptex1/current/os.dmg", &local_err);

    if (file == NULL) { *err = disk_take_error(local_err); }

    return file;
}

ApfsFile* disk_image_file_open(ApfsFile* image, const char* path, char** err)
{
    Error*    local_err = NULL;
    ApfsFile* file      = apfs_open_image_file(image, path, &local_err);

    if (file == NULL) { *err = disk_take_error(local_err); }

    return file;
}

void disk_file_close(ApfsFile* file) { apfs_file_free(file); }

uint64_t disk_file_size(const ApfsFile* file) { return apfs_file_size(file); }

bool disk_file_pread(ApfsFile* file, uint64_t off, uint64_t len, void* buf, char** err)
{
    Error* local_err = NULL;

    if (!apfs_file_pread(file, off, len, buf, &local_err)) {
        *err = disk_take_error(local_err);
        return false;
    }

    return true;
}

bool disk_file_pwrite(ApfsFile* file, uint64_t off, uint64_t len, const void* buf, char** err)
{
    Error* local_err = NULL;

    if (!apfs_file_pwrite(file, off, len, buf, &local_err)) {
        *err = disk_take_error(local_err);
        return false;
    }

    return true;
}

void disk_error_free(char* err) { g_free(err); }
