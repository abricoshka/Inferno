/*
 * Apple Interrupt Controller.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2024-2026 Christian Inci (chris-pcguy).
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
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_APPLE_AIC "apple-aic"
OBJECT_DECLARE_SIMPLE_TYPE(AppleAICState, APPLE_AIC)

typedef struct AppleAICState AppleAICState;

typedef struct
{
    AppleAICState* aic;
    qemu_irq       irq;
    MemoryRegion   iomem;
    MemoryRegion   timebase_iomem;
    uint32_t       cpu_id;
    uint32_t       pending_ipi;
    uint32_t       deferred_ipi;
    uint32_t       ipi_mask;
    uint32_t       tmr_cfg;
    uint32_t       tmr_isr;
    uint32_t       tmr_state;
    uint64_t       tmr_deadline;
    QEMUTimer*     tmr;
} AppleAICCPU;

struct AppleAICState
{
    SysBusDevice parent_obj;
    QEMUTimer*   timer;
    uint32_t     num_eir;
    uint32_t     num_irq;
    uint32_t     num_cpu;
    uint32_t     global_cfg;
    uint32_t     pvt_stamp_cfg;
    uint32_t     shared_stamp[16];
    uint32_t*    eir_mask;
    uint32_t*    eir_dest;
    AppleAICCPU* cpus;
    uint32_t*    eir_state;
};
