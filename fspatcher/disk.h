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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct BlockBackend BlockBackend;
    typedef struct ApfsFile     ApfsFile;

    /// On failure, these return NULL or false and set `*err`, which is freed with `disk_error_free`.
    BlockBackend* disk_open(const char* filename, char** err);
    void          disk_close(BlockBackend* blk);
    /// Open `path` in the System volume of the disk.
    ApfsFile* disk_file_open(BlockBackend* blk, const char* path, char** err);
    /// Open the OS cryptex image in the Preboot volume of the disk.
    ApfsFile* disk_cryptex_open(BlockBackend* blk, char** err);
    /// Open `path` in the image `image`, which must outlive the file.
    ApfsFile* disk_image_file_open(ApfsFile* image, const char* path, char** err);
    void      disk_file_close(ApfsFile* file);
    uint64_t  disk_file_size(const ApfsFile* file);
    bool      disk_file_pread(ApfsFile* file, uint64_t off, uint64_t len, void* buf, char** err);
    bool      disk_file_pwrite(ApfsFile* file, uint64_t off, uint64_t len, const void* buf, char** err);
    void      disk_error_free(char* err);

#ifdef __cplusplus
}
#endif
