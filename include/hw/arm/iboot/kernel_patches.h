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

void ck_patch_kernel(MachoHeader64* hdr);
/// Accept code-signature slot hash mismatches in TXM, so patched pages on SPTM kernels aren't killed.
void ck_patch_txm(MachoHeader64* hdr);
/// Rewrite the GXF transitions, lower-EL exception entries, exception returns and PAC key/SCTLR accesses of an image a
/// hardware accelerator runs to `hvc`, and all IMPLEMENTATION DEFINED register accesses if the accelerator doesn't trap
/// them.
void ck_patch_virt(MachoHeader64* hdr);
/// `ck_patch_virt`, and also rewrite the GXF/SPRR/MMU register accesses and ASID invalidations of an image that runs on
/// a guarded monitor's shadow tables (SPTM, TXM and the kernel under them) to `hvc`.
void ck_patch_virt_monitor(MachoHeader64* hdr);
/// Let the sepOS kernel boot with a timebase other than 24 MHz.
void ck_patch_sep_timebase(CKPatcherRange* range);
/// Rewrite the IMPLEMENTATION DEFINED register accesses and IRQ vectors of the SEP's code at `va` for a hardware
/// accelerator that needs it.
void ck_patch_virt_unmanaged(CKPatcherRange* range, vaddr va);
/// Patch `libimg4.dylib` in an HFS+ restore ramdisk in place; other ramdisks are left alone.
void ck_patch_ramdisk(uint8_t* image, uint64_t size);
