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

#pragma once

#include "qemu/osdep.h"

#define APFS_VOL_ROLE_PREBOOT (0x0010)

/// Read `path` from the volume with `role` in the APFS container on the GPT disk `blk`.
/// The returned buffer is NUL-terminated past `len`.
bool apfs_read_file(BlockBackend* blk, uint16_t role, const char* path, uint8_t** data, gsize* len, Error** errp);
