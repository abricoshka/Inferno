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

#pragma once

#include "qemu/osdep.h"

typedef struct HfsFile HfsFile;

/// Open `path` in the HFS+ or HFSX volume `image`, which must outlive it.
HfsFile* hfs_open_file(uint8_t* image, size_t image_len, const char* path, Error** errp);
void     hfs_file_free(HfsFile* file);
uint64_t hfs_file_size(const HfsFile* file);
bool     hfs_file_pread(HfsFile* file, uint64_t off, uint64_t len, void* buf, Error** errp);
/// Overwrite existing bytes in place; the file cannot grow and no metadata changes.
/// Compressed chunks are recompressed and must still fit in their old space.
bool hfs_file_pwrite(HfsFile* file, uint64_t off, uint64_t len, const void* buf, Error** errp);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(HfsFile, hfs_file_free)
