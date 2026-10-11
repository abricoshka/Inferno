/*
 * Apple Display Pipe V2 Controller.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/display/apple_displaypipe_v2.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "ui/console.h"
#include "framebuffer.h"

typedef struct
{
    uint32_t vftg_ctl;
    uint32_t const_colour;
} DisplayBackEndState;

struct AppleDisplayPipeV2State
{
    /*< private >*/
    SysBusDevice parent_obj;

    uint32_t            width;
    uint32_t            height;
    MemoryRegion        backend_regs;
    MemoryRegion        vram;
    uint64_t            vram_size;
    MemoryRegion*       dma_mr;
    AddressSpace        dma_as;
    MemoryRegionSection vram_section;
    qemu_irq            irqs[9];

    DisplayBackEndState dbe_state;
    QemuConsole*        console;
};

#define REG_SPDS_VERSION (0x1014)

#define REG_DBE_VFTG_CTRL                  (0x8)
#define REG_DBE_SCREEN_SIZE                (0xC)
#define DBE_VFTG_CTRL_VFTG_ENABLE          BIT(31)
#define DBE_VFTG_CTRL_VFTG_STATUS          BIT(30)
#define DBE_VFTG_CTRL_UPDATE_ENABLE_TIMING BIT(15)
#define DBE_VFTG_CTRL_UPDATE_REQ_TIMING    BIT(14)
#define REG_DBE_FRONT_PORCH                (0x10)
#define REG_DBE_SYNC_PULSE                 (0x14)
#define REG_DBE_BACK_PORCH                 (0x18)
#define REG_DBE_CONST_COLOUR               (0x34)

static void frontend_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleDisplayPipeV2State* s = opaque;

    switch (addr) {
        default:
            qemu_log_mask(LOG_UNIMP, "disp0: /frontend/ 0x" HWADDR_FMT_plx " <- 0x%X\n", addr, (uint32_t)data);
            break;
    }
}

static uint64_t frontend_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleDisplayPipeV2State* s = opaque;

    switch (addr) {
        case REG_SPDS_VERSION: qemu_log_mask(LOG_GUEST_ERROR, "disp0: REG_SPDS_VERSION -> 0x13\n"); return 0x13;
        default: qemu_log_mask(LOG_UNIMP, "disp0: /frontend/ 0x" HWADDR_FMT_plx " -> 0x0\n", addr); return 0;
    }
}

static const MemoryRegionOps frontend_reg_ops = {
    .write                 = frontend_write,
    .read                  = frontend_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void backend_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleDisplayPipeV2State* s = opaque;

    switch (addr) {
        case REG_DBE_VFTG_CTRL:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ REG_DBE_VFTG_CTRL <- 0x%X\n", (uint32_t)data);
            if (data & DBE_VFTG_CTRL_VFTG_ENABLE) { data |= DBE_VFTG_CTRL_VFTG_STATUS; }
            s->dbe_state.vftg_ctl = (uint32_t)data;
            break;
        case REG_DBE_SCREEN_SIZE:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ attempted to set screen size, this is "
                                           "NOT supported!\n");
            break;
        case REG_DBE_CONST_COLOUR:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ REG_DBE_CONST_COLOUR <- 0x%X\n", (uint32_t)data);
            s->dbe_state.const_colour = (uint32_t)data;
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "disp0: /backend/ 0x" HWADDR_FMT_plx " <- 0x%X\n", addr, (uint32_t)data);
            break;
    }
}

static uint64_t backend_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleDisplayPipeV2State* s = opaque;

    switch (addr) {
        case REG_DBE_VFTG_CTRL:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ REG_DBE_VFTG_CTRL -> 0x%X\n", s->dbe_state.vftg_ctl);
            return s->dbe_state.vftg_ctl;
        case REG_DBE_SCREEN_SIZE:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ REG_DBE_SCREEN_SIZE -> 0x%X\n",
                          s->width | (s->height << 16));
            return s->width | (s->height << 16);
        case REG_DBE_FRONT_PORCH:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ REG_DBE_FRONT_PORCH -> 0x%X\n", 102 | (536 << 16));
            return 102 | (536 << 16);
        case REG_DBE_SYNC_PULSE:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ REG_DBE_SYNC_PULSE -> 0x%X\n", 32 | (3 << 16));
            return 32 | (3 << 16);
        case REG_DBE_BACK_PORCH:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ REG_DBE_BACK_PORCH -> 0x%X\n", 4 | (4 << 16));
            return 4 | (4 << 16);
        case REG_DBE_CONST_COLOUR:
            qemu_log_mask(LOG_GUEST_ERROR, "disp0: /backend/ REG_DBE_CONST_COLOUR -> 0x%X\n",
                          s->dbe_state.const_colour);
            return s->dbe_state.const_colour;
        default: qemu_log_mask(LOG_UNIMP, "disp0: /backend/ 0x" HWADDR_FMT_plx " -> 0x0\n", addr); return 0;
    }
}

static const MemoryRegionOps backend_reg_ops = {
    .write                 = backend_write,
    .read                  = backend_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void dummy_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    switch (addr) {
        default:
            qemu_log_mask(LOG_UNIMP, "disp0: /dummy/ @ 0x" HWADDR_FMT_plx " <- 0x%X\n", addr, (uint32_t)data);
            break;
    }
}

static uint64_t dummy_read(void* opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
        default: qemu_log_mask(LOG_UNIMP, "disp0: /dummy/ @ 0x" HWADDR_FMT_plx " -> 0x0\n", addr); return 0;
    }
}

static const MemoryRegionOps dummy_reg_ops = {
    .write                 = dummy_write,
    .read                  = dummy_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void adp_v2_draw_row(void* opaque, uint8_t* dest, const uint8_t* src, int width, int dest_pitch)
{
    while (width--) {
        uint32_t colour  = ldl_le_p(src);
        src             += sizeof(colour);
        memcpy(dest, &colour, sizeof(colour));
        dest += sizeof(colour);
    }
}

static void adp_v2_gfx_update(void* opaque)
{
    AppleDisplayPipeV2State* s       = opaque;
    DisplaySurface*          surface = qemu_console_surface(s->console);

    int stride = s->width * sizeof(uint32_t);

    int first = 0, last = 0;

    if (!s->vram_section.mr) { framebuffer_update_memory_section(&s->vram_section, &s->vram, 0, s->height, stride); }
    framebuffer_update_display(surface, &s->vram_section, s->width, s->height, stride, stride, 0, 0, adp_v2_draw_row, s,
                               &first, &last);
    if (first >= 0) { dpy_gfx_update(s->console, 0, first, s->width, last - first + 1); }
}

static const GraphicHwOps adp_v2_ops = {
    .gfx_update = adp_v2_gfx_update,
};

static void adp_v2_realize(DeviceState* dev, Error** errp)
{
    AppleDisplayPipeV2State* s = APPLE_DISPLAY_PIPE_V2(dev);

    s->dma_mr = MEMORY_REGION(object_property_get_link(OBJECT(dev), "dma-mr", &error_abort));
    address_space_init(&s->dma_as, s->dma_mr, "disp0.dma");

    if (!memory_region_init_ram(&s->vram, OBJECT(s), "vram", s->vram_size, errp)) { return; }

    s->dbe_state          = (DisplayBackEndState){0};
    s->dbe_state.vftg_ctl = DBE_VFTG_CTRL_VFTG_ENABLE | DBE_VFTG_CTRL_VFTG_STATUS | DBE_VFTG_CTRL_UPDATE_ENABLE_TIMING
                            | DBE_VFTG_CTRL_UPDATE_REQ_TIMING;
    s->console            = graphic_console_init(dev, 0, &adp_v2_ops, s);
    qemu_console_resize(s->console, s->width, s->height);
}

static void adp_v2_init(Object* obj)
{
    SysBusDevice*            sbd = SYS_BUS_DEVICE(obj);
    AppleDisplayPipeV2State* s   = APPLE_DISPLAY_PIPE_V2(obj);
    MemoryRegion*            mr  = g_new0(MemoryRegion, 5);

    memory_region_init_io(mr, obj, &frontend_reg_ops, s, "adp.frontend", 0x100000);
    memory_region_init_io(&s->backend_regs, obj, &backend_reg_ops, s, "adp.backend", 0x8000);
    memory_region_init_io(mr + 1, obj, &dummy_reg_ops, s, "adp.aap", 0x8000);
    memory_region_init_io(mr + 2, obj, &dummy_reg_ops, s, "adp.pixel-bl", 0x8000);
    memory_region_init_io(mr + 3, obj, &dummy_reg_ops, s, "adp.dither", 0x8000);
    memory_region_init_io(mr + 4, obj, &dummy_reg_ops, s, "adp.prc", 0x1C);

    sysbus_init_mmio(sbd, mr);
    sysbus_init_mmio(sbd, &s->backend_regs);
    sysbus_init_mmio(sbd, mr + 1);
    sysbus_init_mmio(sbd, mr + 2);
    sysbus_init_mmio(sbd, mr + 3);
    sysbus_init_mmio(sbd, mr + 4);

    for (size_t i = 0; i < ARRAY_SIZE(s->irqs); i++) { sysbus_init_irq(sbd, &s->irqs[i]); }
}

static const Property adp_v2_props[] = {
    // iPhone 4/4S
    DEFINE_PROP_UINT32("width", AppleDisplayPipeV2State, width, 640),
    DEFINE_PROP_UINT32("height", AppleDisplayPipeV2State, height, 960),
    DEFINE_PROP_UINT64("vram-size", AppleDisplayPipeV2State, vram_size, 0),
};

static void adp_v2_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(klass);

    dc->realize = adp_v2_realize;
    device_class_set_props(dc, adp_v2_props);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(AppleDisplayPipeV2State, adp_v2, APPLE_DISPLAY_PIPE_V2, SYS_BUS_DEVICE)

void adp_v2_update_vram_mapping(AppleDisplayPipeV2State* s, MemoryRegion* mr, hwaddr base)
{
    if (memory_region_is_mapped(&s->vram)) { memory_region_del_subregion(mr, &s->vram); }
    memory_region_add_subregion_overlap(mr, base, &s->vram, 1);
}
