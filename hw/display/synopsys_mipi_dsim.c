/*
 * Synopsys MIPI DSIM.
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

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/display/synopsys_mipi_dsim.h"
#include "hw/registerfields.h"
#include "system/memory.h"

// clang-format off
REG32(CORE_VERSION, 0x0)
REG32(CORE_PWR_UP, 0x4)
    REG_FIELD(CORE_PWR_UP, SHUTDOWNZ, 0, 1)
REG32(CORE_CMD_PKT_STATUS, 0x74)
    REG_FIELD(CORE_CMD_PKT_STATUS, GEN_CMD_EMPTY, 0, 1)
REG32(GENERAL_CTRL, 0x80004)
    REG_FIELD(GENERAL_CTRL, PHYLOCK_HW_LOCK, 4, 1)
REG32(TOP_PLL_CTRL, 0x80034)
// clang-format on

struct SynopsysMIPIDSIMState
{
    /*< private >*/
    SysBusDevice parent_obj;
    MemoryRegion iomems[2];
    qemu_irq     irqs[2];

    /*< public >*/
    uint32_t power_up;
};

static void synopsys_mipi_dsim_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    SynopsysMIPIDSIMState* s = opaque;

    switch (addr >> 2) {
        case R_CORE_PWR_UP: s->power_up = data; break;
        default           : break;
    }
}

static uint64_t synopsys_mipi_dsim_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    SynopsysMIPIDSIMState* s = opaque;

    switch (addr >> 2) {
        case R_CORE_VERSION       : return 0x3133302A;
        case R_CORE_PWR_UP        : return s->power_up;
        case R_CORE_CMD_PKT_STATUS: return REG_FIELD_DP32(0, CORE_CMD_PKT_STATUS, GEN_CMD_EMPTY, 1);
        case R_GENERAL_CTRL       : return REG_FIELD_DP32(0, GENERAL_CTRL, PHYLOCK_HW_LOCK, 1);
        case R_TOP_PLL_CTRL       : return 0;
        default                   : return UINT32_MAX;
    }
}

static const MemoryRegionOps synopsys_mipi_dsim_reg_ops = {
    .write                 = synopsys_mipi_dsim_reg_write,
    .read                  = synopsys_mipi_dsim_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void synopsys_mipi_dsim_swmpr_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    switch (addr >> 2) {
        default: break;
    }
}

static uint64_t synopsys_mipi_dsim_swmpr_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    switch (addr >> 2) {
        default: return UINT32_MAX;
    }
}

static const MemoryRegionOps synopsys_mipi_dsim_swmpr_reg_ops = {
    .write                 = synopsys_mipi_dsim_swmpr_reg_write,
    .read                  = synopsys_mipi_dsim_swmpr_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void synopsys_mipi_dsim_reset_enter(Object* obj, ResetType type)
{
    SynopsysMIPIDSIMState* mipi = SYNOPSYS_MIPI_DSIM(obj);

    // Default to display powered on
    mipi->power_up = REG_FIELD_DP32(0, CORE_PWR_UP, SHUTDOWNZ, 1);
}

static void synopsys_mipi_dsim_init(Object* obj)
{
    SynopsysMIPIDSIMState* mipi = SYNOPSYS_MIPI_DSIM(obj);
    SysBusDevice*          sbd  = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&mipi->iomems[0], obj, &synopsys_mipi_dsim_reg_ops, mipi, TYPE_SYNOPSYS_MIPI_DSIM ".regs",
                          0xC0000);
    sysbus_init_mmio(sbd, &mipi->iomems[0]);
    memory_region_init_io(&mipi->iomems[1], obj, &synopsys_mipi_dsim_swmpr_reg_ops, mipi,
                          TYPE_SYNOPSYS_MIPI_DSIM ".swmpr_regs", 0x10000);
    sysbus_init_mmio(sbd, &mipi->iomems[1]);

    sysbus_init_irq(sbd, &mipi->irqs[0]);
    sysbus_init_irq(sbd, &mipi->irqs[1]);
}

static void synopsys_mipi_dsim_class_init(ObjectClass* oc, const void* data)
{
    ResettableClass* rc;
    DeviceClass*     dc;

    rc = RESETTABLE_CLASS(oc);
    dc = DEVICE_CLASS(oc);

    rc->phases.enter = synopsys_mipi_dsim_reset_enter;

    dc->desc           = "Synopsys MIPI DSIM";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(SynopsysMIPIDSIMState, synopsys_mipi_dsim, SYNOPSYS_MIPI_DSIM, SYS_BUS_DEVICE)
