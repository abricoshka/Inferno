/*
 * ChefKiss Patcher (PenguinWizardryC).
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

#pragma once

#include "qemu/osdep.h"
#include "exec/vaddr.h"

#define MOV_W0_WZR_BYTES 0xE0, 0x03, 0x1F, 0x2A

typedef struct
{
    /// Physical or virtual address.
    vaddr addr;
    vaddr length;
    /// Guaranteed to be an accessible host pointer.
    uint8_t*    ptr;
    const char* name;
} CKPatcherRange;

CKPatcherRange* ck_patcher_range_from_ptr(const char* name, uint8_t* ptr, vaddr size);

/// Precondition: `insn` must be masked.
uint8_t* ck_patcher_find_next_insn(uint8_t* buffer, uint32_t num, uint32_t insn, uint32_t mask, uint32_t skip);
/// See `ck_patcher_find_next_insn`.
uint8_t* ck_patcher_find_prev_insn(uint8_t* buffer, uint32_t num, uint32_t insn, uint32_t mask, uint32_t skip);

/// Callback function prototype. `ctx` may be null.
typedef bool (*CKPatcherCallback)(void* ctx, uint8_t* buffer);

/// Precondition: bytes in `find` must be masked.
/// If `align` is set to a non-zero value, the searching will be aligned
/// to its value amount of bytes, otherwise it will align the search by
/// a single byte.
/// The search stops after `callback` succeeded `count` times; 0 means
/// every match.
bool ck_patcher_find_callback_ctx(CKPatcherRange* range, const char* name, const uint8_t* pattern, const uint8_t* mask,
                                  size_t len, size_t align, uint32_t count, void* ctx, CKPatcherCallback callback);
/// See `ck_patcher_find_callback_ctx`.
bool ck_patcher_find_callback(CKPatcherRange* range, const char* name, const uint8_t* pattern, const uint8_t* mask,
                              size_t len, size_t align, uint32_t count, CKPatcherCallback callback);
/// See `ck_patcher_find_callback`.
/// `replace_off` is the byte offset in the matched pattern
/// which the `replacement` will be applied on.
bool ck_patcher_find_replace(CKPatcherRange* range, const char* name, const uint8_t* pattern, const uint8_t* mask,
                             size_t len, size_t align, uint32_t count, const uint8_t* replacement,
                             const uint8_t* replacement_mask, size_t replace_off, size_t replace_len);

/// Thumb-2 `MOVW Rd, #imm16` (T3) at `buffer`.
bool ck_patcher_thumb_movw(const uint8_t* buffer, uint16_t* imm);
/// Thumb-2 `STR.W Rt, [Rn, #imm12]` (T3) at `buffer`.
bool ck_patcher_thumb_str_imm12(const uint8_t* buffer, uint16_t* imm);
/// Thumb-2 `B.W` (T4) or `BL` (T1) at `buffer`, which sits at `pc`.
bool ck_patcher_thumb_branch(const uint8_t* buffer, uint32_t pc, uint32_t* target);
