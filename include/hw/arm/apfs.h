/*
 * Minimal APFS file reader and in-place writer.
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

#include "qemu/osdep.h"

#define APFS_VOL_ROLE_SYSTEM  (0x0001)
#define APFS_VOL_ROLE_PREBOOT (0x0010)

typedef struct ApfsFile ApfsFile;

/// Open `path` in the volume with `role` in the APFS container on the GPT disk `blk`.
ApfsFile* apfs_open_file(BlockBackend* blk, uint16_t role, const char* path, Error** errp);
/// Open `path` in the volume of the APFS container that `image` holds, such as a cryptex; `image` must outlive it.
ApfsFile* apfs_open_image_file(ApfsFile* image, const char* path, Error** errp);
/// Open `path` in the volume of the APFS container `image`, which must outlive it.
ApfsFile* apfs_open_memory_file(uint8_t* image, size_t image_len, const char* path, Error** errp);
void      apfs_file_free(ApfsFile* file);
uint64_t  apfs_file_size(const ApfsFile* file);
bool      apfs_file_pread(ApfsFile* file, uint64_t off, uint64_t len, void* buf, Error** errp);
/// Overwrite existing bytes in place; the file cannot grow and no metadata changes.
bool apfs_file_pwrite(ApfsFile* file, uint64_t off, uint64_t len, const void* buf, Error** errp);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ApfsFile, apfs_file_free)

/// Read `path` from the volume with `role` in the APFS container on the GPT disk `blk`.
/// The returned buffer is NUL-terminated past `len`.
bool apfs_read_file(BlockBackend* blk, uint16_t role, const char* path, uint8_t** data, gsize* len, Error** errp);
