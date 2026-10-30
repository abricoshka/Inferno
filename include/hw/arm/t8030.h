/*
 * Apple T8030 SoC (iPhone 11).
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

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/arm/a13.h"
#include "hw/arm/iboot/iboot.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "qapi/qapi-types-usb.h"
#include "system/kvm.h"

#define TYPE_APPLE_T8030 MACHINE_TYPE_NAME("t8030")

#define APPLE_T8030(obj) OBJECT_CHECK(AppleT8030MachineState, (obj), TYPE_APPLE_T8030)

typedef struct
{
    MachineClass parent;
} AppleT8030MachineClass;

typedef struct
{
    MachineState parent;

    AppleA13State*  cpus[A13_MAX_CPU];
    AppleA13Cluster clusters[A13_MAX_CLUSTER];
    SysBusDevice*   aic;
    AppleBootInfo   boot_info;
    uint64_t        ecid;
    uint8_t         pmgr_reg[0x100000];
    MemoryRegion    amcc;
    uint8_t         amcc_reg[0x180000];
    bool            force_dfu;
    uint32_t        board_id;
    uint32_t        chip_revision;
    char*           usb_uplink_addr;
    USBUplinkType   usb_uplink_type;
    uint32_t        usb_uplink_compression_limit;
    char*           model_number;
    char*           region_info;
    char*           config_number;
    char*           serial_number;
    char*           mlb_serial_number;
    char*           regulatory_model;
    uint32_t        disp_width;
    uint32_t        disp_height;
} AppleT8030MachineState;
