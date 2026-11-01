/*
 * Simulated Apple iBoot.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
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

// #define ENABLE_BASEBAND
#define ENABLE_DATA_ENCRYPTION

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/arm/dart.h"
#include "hw/arm/dt.h"
#include "hw/arm/iboot/boot.h"
#include "hw/arm/iboot/dfu.h"
#include "hw/boards.h"
#include "hw/display/apple_displaypipe_v4.h"
#include "qapi/qapi-types-machine-common.h"

/// The `region-id-<n>` of a carveout in `/chosen/carveout-memory-map`. XNU takes 55 to be iBoot's profiler region.
typedef enum
{
    APPLE_CARVEOUT_IBOOT_HANDOFF = 0x01,
    APPLE_CARVEOUT_NVME_SCRATCH  = 0x02,
    APPLE_CARVEOUT_SEPFW         = 0x04,
    APPLE_CARVEOUT_AP_TRUSTZONE  = 0x06,
    APPLE_CARVEOUT_UNKNOWN_07    = 0x07,
    APPLE_CARVEOUT_UNKNOWN_0A    = 0x0A,
    APPLE_CARVEOUT_PRAM          = 0x0C,
    APPLE_CARVEOUT_VRAM          = 0x0E,
    APPLE_CARVEOUT_UNKNOWN_50    = 0x50,
    APPLE_CARVEOUT_IOP_SIO       = 0x60,
    APPLE_CARVEOUT_IOP_ANS       = 0x61,
    APPLE_CARVEOUT_DISP_DART     = 0x62,
} AppleCarveoutID;

typedef struct
{
    AppleCarveoutID id;
    hwaddr          size;
} AppleCarveout;

typedef enum
{
    kAppleBootModeAuto = 0,
    kAppleBootModeEnterRecovery,
    kAppleBootModeExitRecovery,
    kAppleBootModeDFU,
} AppleBootMode;

typedef struct AppleIBootSplash AppleIBootSplash;

typedef struct
{
    /* Set by the machine before `apple_iboot_init`. */
    hwaddr dram_base;
    hwaddr dram_size;
    /// Carved out downwards from the end of DRAM in this order. Includes the VRAM and PRAM.
    const AppleCarveout* carveouts;
    size_t               num_carveouts;

    /* Set by the machine before its first reset. */
    hwaddr srom_base;
    /// The SEP ROM is not loaded if 0.
    hwaddr seprom_base;
    hwaddr sepfw_size;
    /// The SEP's TZ0; there is none if the size is 0.
    hwaddr tz0_size;
    hwaddr tz0_align;
    /// Offset of the framebuffer in the VRAM.
    hwaddr   fb_offset;
    uint32_t disp_width;
    uint32_t disp_height;
    /// Prepended to the kernel command line.
    const char* boot_args;
    /// The AP's CPUs are CPUs 0 to `num_cpus - 1`.
    uint32_t num_cpus;
    /// Rewrite the kernel, SPTM and TXM for CPUs that `apple_virt_manage_cpu` manages.
    bool           patch_virt;
    AppleDFUState* dfu;
    /// The display iBoot leaves scanning out the framebuffer, whose DMA goes through `disp_sid` of `disp_dart`.
    AppleDARTState*          disp_dart;
    uint32_t                 disp_sid;
    AppleDisplayPipeV4State* disp;
    void                     (*update_device_tree)(MachineState* machine, AppleDTNode* root);
    /// Lock the read-only region of the kernel, if `lock_ro`, and the TZ0.
    void (*lock_regions)(MachineState* machine, hwaddr ro_lower, hwaddr ro_upper, bool lock_ro, hwaddr tz0_base,
                         hwaddr tz0_size);

    /* Machine properties. */
    char*          securerom_filename;
    char*          seprom_filename;
    char*          trustcache_filename;
    bool           kaslr_off;
    AppleBootMode  boot_mode;
    KernelPatchset kernel_patchset;

    /// Set by `apple_iboot_reset`: the build version of the kernel it boots, or 0.
    uint32_t build_version;

    /* Private. */
    MachineState*     machine;
    GArray*           regions;
    guint             num_static_regions;
    hwaddr            static_end;
    hwaddr            carveout_end;
    gchar*            securerom;
    gsize             securerom_size;
    uint64_t          boot_nonce_generator;
    bool              non_cold_boot;
    bool              had_autoboot;
    AppleVideoArgs    video_args;
    hwaddr            entry;
    hwaddr            entry_arg;
    hwaddr            rvbar;
    AppleIBootSplash* splash;
} AppleBootInfo;

/// Carve out the machine's regions of DRAM.
void apple_iboot_init(AppleBootInfo* info, MachineState* machine);

/// Boot the machine after its reset, through the SecureROM if the machine has one.
void apple_iboot_reset(AppleBootInfo* info);

hwaddr apple_iboot_carveout_base(const AppleBootInfo* info, AppleCarveoutID id);

/// Give `cpu` the state iBoot hands off with.
void apple_iboot_handoff_cpu(CPUState* cpu);
