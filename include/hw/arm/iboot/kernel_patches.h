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

#pragma once

#include "hw/arm/iboot/boot.h"
#include "hw/arm/iboot/patcher.h"
#include "qapi/qapi-types-machine-common.h"

void ck_patch_kernel(MachoHeader64* hdr, KernelPatchset patchset);
/// Accept code-signature slot hash mismatches in TXM, so patched pages on SPTM kernels aren't killed.
void ck_patch_txm(MachoHeader64* hdr);
/// Let the sepOS kernel boot with a timebase other than 24 MHz.
void ck_patch_sep_timebase(CKPatcherRange* range);
/// Patch `libimg4.dylib` in an HFS+ restore ramdisk in place; other ramdisks are left alone.
void ck_patch_ramdisk(uint8_t* image, uint64_t size);
