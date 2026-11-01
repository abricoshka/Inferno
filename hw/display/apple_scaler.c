/*
 * Apple M2 Scaler and Color Space Converter.
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
#include "hw/display/apple_scaler.h"
#include "graphics/inferno_pixbuf.h"
#include "hw/irq.h"
#include "hw/registerfields.h"
#include "hw/resettable.h"
#include "qemu/lockable.h"
#include "qemu/thread.h"
#include "system/dma.h"
#include <libyuv.h>

#if 0
    #define SCALER_INFO(fmt, ...) fprintf(stderr, "%s: " fmt "\n", __func__, ##__VA_ARGS__)
#else
    #define SCALER_INFO(fmt, ...) \
        do { }                    \
        while (0);
#endif

// clang-format off
REG32(GLBL_VER, 0x0)
REG32(GLBL_CTRL, 0x4)
    REG_FIELD(GLBL_CTRL, RESET, 0, 1)
REG32(GLBL_STS, 0xC)
    REG_FIELD(GLBL_STS, RUNNING, 0, 1)
REG32(GLBL_IRQ_MASK, 0x1C)
REG32(GLBL_IRQSTS, 0x20)
REG32(GLBL_FRAMECNT, 0x24)
// End: 0x60

REG32(CTRL_COMMAND, 0x80)
    REG_FIELD(CTRL_COMMAND, RUN, 0, 1)
    REG_FIELD(CTRL_COMMAND, READ_ONLY, 1, 1)
REG32(CTRL_DBG, 0x9C)
REG32(CTRL_PIXEL_AVERAGING, 0xE4)
// End: 0x10C

// REG32(SRC_LUMA_BASE, 0x104)
// REG32(SRC_CHROMA_BASE, 0x108)
REG32(SRC_LUMA_PITCH, 0x10C)
REG32(SRC_CHROMA_PITCH, 0x110)
REG32(SRC_LUMA_PIXEL_OFFSET_IN_TILE_ARRAY, 0x114)
REG32(SRC_CHROMA_PIXEL_OFFSET_IN_TILE_ARRAY, 0x118)
REG32(SRC_RDMA_SIZE, 0x128)
REG32(SRC_FORMAT, 0x180)
REG32(SRC_LUMA_TEXTURE_SIZES, 0x184)
REG32(SRC_CHROMA_TEXTURE_SIZES, 0x188)
// REG32(SRC_LUMA_COMP_BASE, 0x194)
// REG32(SRC_CHROMA_COMP_BASE, 0x198)
REG32(SRC_LUMA_BASE, 0x194)
REG32(SRC_CHROMA_BASE, 0x198)
REG32(SRC_LUMA_COMP_INFO, 0x19C)
REG32(SRC_CHROMA_COMP_INFO, 0x1A0)
REG32(SRC_LUMA_COMP_HEADER_BASE, 0x1A4)
REG32(SRC_CHROMA_COMP_HEADER_BASE, 0x1A8)
REG32(SRC_LUMA_COMP_DATA_BASE, 0x1AC)
REG32(SRC_CHROMA_COMP_DATA_BASE, 0x1B0)
REG32(SRC_LUMA_STRIDE, 0x1B4)
REG32(SRC_CHROMA_STRIDE, 0x1B8)
REG32(SRC_LUMA_OFFSETS, 0x1BC)
REG32(SRC_CHROMA_OFFSETS, 0x1C0)
REG32(SRC_SWIZZLE, 0x1C4)
REG32(SRC_SIZE, 0x1C8)
// End: 0x1F0

// REG32(DST_LUMA_BASE, 0x204)
// REG32(DST_CHROMA_BASE, 0x208)
REG32(DST_LUMA_PITCH, 0x20C)
REG32(DST_CHROMA_PITCH, 0x210)
REG32(DST_LUMA_PIXEL_OFFSET_IN_TILE_ARRAY, 0x214)
REG32(DST_CHROMA_PIXEL_OFFSET_IN_TILE_ARRAY, 0x218)
REG32(DST_WDMA_SIZE, 0x228)
REG32(DST_FORMAT, 0x280)
REG32(DST_LUMA_TEXTURE_SIZES, 0x284)
REG32(DST_CHROMA_TEXTURE_SIZES, 0x288)
// REG32(DST_LUMA_COMP_BASE, 0x294)
// REG32(DST_CHROMA_COMP_BASE, 0x298)
REG32(DST_LUMA_BASE, 0x294)
REG32(DST_CHROMA_BASE, 0x298)
REG32(DST_LUMA_COMP_INFO, 0x29C)
REG32(DST_CHROMA_COMP_INFO, 0x2A0)
REG32(DST_LUMA_COMP_HEADER_BASE, 0x2A4)
REG32(DST_CHROMA_COMP_HEADER_BASE, 0x2A8)
REG32(DST_LUMA_COMP_DATA_BASE, 0x2AC)
REG32(DST_CHROMA_COMP_DATA_BASE, 0x2B0)
REG32(DST_LUMA_STRIDE, 0x2B4)
REG32(DST_CHROMA_STRIDE, 0x2B8)
REG32(DST_LUMA_OFFSETS, 0x2BC)
REG32(DST_CHROMA_OFFSETS, 0x2C0)
REG32(DST_SWIZZLE, 0x2C4)
REG32(DST_SIZE, 0x2C8)
// End: 0x2FC

/* packs two 14-bit pixel-space offsets, per M2ScalerSrcDestCfgControlMSR8::configureOffsets_gatedContext */
REG_FIELD(SRCDST_CL_PIXEL_OFFSET_IN_TILE_ARRAY, VERTICAL_LO, 0, 16)
REG_FIELD(SRCDST_CL_PIXEL_OFFSET_IN_TILE_ARRAY, HORIZONTAL, 16, 13)
REG_FIELD(SRCDST_CL_PIXEL_OFFSET_IN_TILE_ARRAY, VERTICAL_HI, 30, 2)
REG_FIELD(SRCDST_CL_OFFSET, HORIZONTAL, 0, 14)
REG_FIELD(SRCDST_CL_OFFSET, VERTICAL, 16, 14)
REG_FIELD(SRCDST_FORMAT, PACKED_RGB, 0, 1)
REG_FIELD(SRCDST_FORMAT, PREMULTIPLIED, 1, 1) /* PBGR/PBGH */
REG_FIELD(SRCDST_FORMAT, SUBSAMPLING, 2, 2) /* YUV */
REG_FIELD(SRCDST_FORMAT, GAMUT_MODE, 4, 3)
REG_FIELD(SRCDST_FORMAT, PLANE_LAYOUT, 8, 6) /* 0x06 8-bit 2-plane, 0x12 10-bit 2-plane, 0x13 wide 2-plane, ... */
REG_FIELD(SRCDST_FORMAT, CHROMA_COMPRESSED, 14, 1)
REG_FIELD(SRCDST_FORMAT, CHROMA_ADDRESSING_FORMAT, 16, 4)
REG_FIELD(SRCDST_FORMAT, SAMPLE_FORMAT, 20, 6) /* 1 = 8-bit, 2 = 10-bit, 3 = 12-bit, 5 = 16-bit */
REG_FIELD(SRCDST_FORMAT, LUMA_COMPRESSED, 26, 1)
REG_FIELD(SRCDST_FORMAT, LUMA_ADDRESSING_FORMAT, 28, 4)
REG_FIELD(SRCDST_SWIZZLE, COMPONENT_0, 0, 2)
REG_FIELD(SRCDST_SWIZZLE, COMPONENT_1, 8, 2)
REG_FIELD(SRCDST_SWIZZLE, COMPONENT_2, 16, 2)
REG_FIELD(SRCDST_SWIZZLE, COMPONENT_3, 24, 2)
REG_FIELD(SRCDST_SIZE, WIDTH, 0, 15)
REG_FIELD(SRCDST_SIZE, HEIGHT, 16, 15)

REG32(FLIP_ROTATE_CFG, 0x380)
    REG_FIELD(FLIP_ROTATE_CFG, ROTATE_90, 0, 1)
    REG_FIELD(FLIP_ROTATE_CFG, ROTATE_180, 1, 1)
    REG_FIELD(FLIP_ROTATE_CFG, FLIP_Y, 2, 1)
    REG_FIELD(FLIP_ROTATE_CFG, FLIP_X, 3, 1)
// End: 0x380

REG32(CSC_CFG_CHROMA_DOWNSAMPLING, 0x900)
// End: 0x900

REG32(BORDER_FILL_CFG, 0x3034)
REG32(BORDER_FILL_CFG_RED_Y, 0x3038)
REG32(BORDER_FILL_CFG_BLUE_CR, 0x303C)
REG32(BORDER_FILL_CFG_GREEN_CB, 0x3040)
REG32(BORDER_FILL_CFG_LUMA_OFFSETS, 0x3044)
    REG_FIELD(BORDER_FILL_CFG_LUMA_OFFSETS, X, 0, 15)
    REG_FIELD(BORDER_FILL_CFG_LUMA_OFFSETS, Y, 16, 15)
REG32(BORDER_FILL_CFG_CHROMA_OFFSETS, 0x3048)
    REG_FIELD(BORDER_FILL_CFG_CHROMA_OFFSETS, X, 0, 15)
    REG_FIELD(BORDER_FILL_CFG_CHROMA_OFFSETS, Y, 16, 15)
// End: 0x3048
// clang-format on

typedef enum SwizzleComponent
{
    SWIZZLE_COMPONENT_BLUE  = 0,
    SWIZZLE_COMPONENT_GREEN = 1,
    SWIZZLE_COMPONENT_RED   = 2,
    SWIZZLE_COMPONENT_ALPHA = 3,
} SwizzleComponent;

typedef enum AddressingFormat
{
    ADDRESSING_FORMAT_LINEAR            = 0,
    ADDRESSING_FORMAT_INDIRECT          = 1,
    ADDRESSING_FORMAT_TWIDDLED          = 2,
    ADDRESSING_FORMAT_TILED             = 3,
    ADDRESSING_FORMAT_REFERENCE         = 4,
    ADDRESSING_FORMAT_INTERCHANGE_TILED = 5,
} AddressingFormat;

typedef enum Subsampling
{
    SUBSAMPLING_444 = 0,
    SUBSAMPLING_422 = 1,
    SUBSAMPLING_420 = 3,
} Subsampling;

typedef enum SourceDest
{
    SOURCE,
    DEST,
    SOURCE_DEST_COUNT,
} SourceDest;

typedef enum LumaChroma
{
    CHROMA,
    LUMA,
    LUMA_CHROMA_COUNT,
} LumaChroma;

typedef struct AppleScalerSrcDstConfig
{
    uint32_t format;
    uint32_t base[LUMA_CHROMA_COUNT];
    uint32_t stride[LUMA_CHROMA_COUNT];
    uint32_t offsets[LUMA_CHROMA_COUNT];
    uint32_t swizzle;
    uint32_t size;
} AppleScalerSrcDstConfig;

struct AppleScalerState
{
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuMutex    lock;
    MemoryRegion regs_iomem;
    MemoryRegion apiodma_iomem;
    AddressSpace dma_as;
    qemu_irq     irqs[2];
    QEMUBH*      bh;

    AppleScalerSrcDstConfig srcdst[SOURCE_DEST_COUNT];
    uint32_t                flip_rotate_cfg;
    uint32_t                frame_count;
    uint32_t                irq_sts;
    bool                    running;
};

typedef enum AppleScalerFormat
{
    APPLE_SCALER_FORMAT_BGRA                 = 0x00,
    APPLE_SCALER_FORMAT_RGBA                 = 0x01,
    APPLE_SCALER_FORMAT_L565                 = 0x02,
    APPLE_SCALER_FORMAT_1555_5551            = 0x03,
    APPLE_SCALER_FORMAT_YUV_420              = 0x04, /* 420{f,v} */
    APPLE_SCALER_FORMAT_YUV_422              = 0x05, /* 422{f,v} */
    APPLE_SCALER_FORMAT_YUV_444              = 0x06, /* 444{f,v} */
    APPLE_SCALER_FORMAT_YUVS_YUVF            = 0x07,
    APPLE_SCALER_FORMAT_L10R                 = 0x08,
    APPLE_SCALER_FORMAT_PBGR                 = 0x09,
    APPLE_SCALER_FORMAT_W40A                 = 0x0A,
    APPLE_SCALER_FORMAT_PBGH                 = 0x0B,
    APPLE_SCALER_FORMAT_XF4P_X44P            = 0x0C,
    APPLE_SCALER_FORMAT_X420_XF20            = 0x0D,
    APPLE_SCALER_FORMAT_X422_XF22            = 0x0E,
    APPLE_SCALER_FORMAT_XF2P                 = 0x0F,
    APPLE_SCALER_FORMAT_X444_XF44_XW44       = 0x10,
    APPLE_SCALER_FORMAT_L008                 = 0x11,
    APPLE_SCALER_FORMAT_L010                 = 0x12,
    APPLE_SCALER_FORMAT_L012                 = 0x13,
    APPLE_SCALER_FORMAT_L016                 = 0x14,
    APPLE_SCALER_FORMAT_B3A8                 = 0x15,
    APPLE_SCALER_FORMAT_PW20_P420            = 0x16,
    APPLE_SCALER_FORMAT_PF22_P422            = 0x17,
    APPLE_SCALER_FORMAT_PF44_P444            = 0x18, /* name inferred from its 420/422 siblings */
    APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE = 0x19,
    APPLE_SCALER_FORMAT_T422_Y422_FAMILY     = 0x1A,
    APPLE_SCALER_FORMAT_Y444_FAMILY          = 0x1B,
    APPLE_SCALER_FORMAT_S4F4_S4V4            = 0x1E,
    APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V       = 0x1F,
    APPLE_SCALER_FORMAT_PTW2_YP2F_YP2V       = 0x20,
    APPLE_SCALER_FORMAT_Y4F4_TW44_FAMILY     = 0x21,
    APPLE_SCALER_FORMAT_RGHA_FP16            = 0x28,
    APPLE_SCALER_FORMAT_COUNT,
    APPLE_SCALER_FORMAT_UNKNOWN = -1,
} AppleScalerFormat;

static G_GNUC_UNUSED const char* apple_scaler_stringify_format(AppleScalerFormat format)
{
    switch (format) {
        case APPLE_SCALER_FORMAT_BGRA                : return "BGRA";
        case APPLE_SCALER_FORMAT_RGBA                : return "RGBA";
        case APPLE_SCALER_FORMAT_L565                : return "L565";
        case APPLE_SCALER_FORMAT_1555_5551           : return "1555_5551";
        case APPLE_SCALER_FORMAT_YUV_420             : return "YUV_420";
        case APPLE_SCALER_FORMAT_YUV_422             : return "YUV_422";
        case APPLE_SCALER_FORMAT_YUV_444             : return "YUV_444";
        case APPLE_SCALER_FORMAT_YUVS_YUVF           : return "YUVS_YUVF";
        case APPLE_SCALER_FORMAT_L10R                : return "L10R";
        case APPLE_SCALER_FORMAT_PBGR                : return "PBGR";
        case APPLE_SCALER_FORMAT_W40A                : return "W40A";
        case APPLE_SCALER_FORMAT_PBGH                : return "PBGH";
        case APPLE_SCALER_FORMAT_XF4P_X44P           : return "XF4P_X44P";
        case APPLE_SCALER_FORMAT_X420_XF20           : return "X420_XF20";
        case APPLE_SCALER_FORMAT_X422_XF22           : return "X422_XF22";
        case APPLE_SCALER_FORMAT_XF2P                : return "XF2P";
        case APPLE_SCALER_FORMAT_X444_XF44_XW44      : return "X444_XF44_XW44";
        case APPLE_SCALER_FORMAT_L008                : return "L008";
        case APPLE_SCALER_FORMAT_L010                : return "L010";
        case APPLE_SCALER_FORMAT_L012                : return "L012";
        case APPLE_SCALER_FORMAT_L016                : return "L016";
        case APPLE_SCALER_FORMAT_B3A8                : return "B3A8";
        case APPLE_SCALER_FORMAT_PW20_P420           : return "PW20_P420";
        case APPLE_SCALER_FORMAT_PF22_P422           : return "PF22_P422";
        case APPLE_SCALER_FORMAT_PF44_P444           : return "PF44_P444";
        case APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE: return "PTW0_420_2PLANE_WIDE";
        case APPLE_SCALER_FORMAT_T422_Y422_FAMILY    : return "T422_Y422_FAMILY";
        case APPLE_SCALER_FORMAT_Y444_FAMILY         : return "Y444_FAMILY";
        case APPLE_SCALER_FORMAT_S4F4_S4V4           : return "S4F4_S4V4";
        case APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V      : return "PTW0_YP0F_YP0V";
        case APPLE_SCALER_FORMAT_PTW2_YP2F_YP2V      : return "PTW2_YP2F_YP2V";
        case APPLE_SCALER_FORMAT_Y4F4_TW44_FAMILY    : return "Y4F4_TW44_FAMILY";
        case APPLE_SCALER_FORMAT_RGHA_FP16           : return "RGHA_FP16";
        default                                      : return "UNKNOWN";
    }
}

static InfernoDMASurfaceFormat apple_scaler_surface_format(AppleScalerFormat format)
{
    switch (format) {
        case APPLE_SCALER_FORMAT_BGRA          : return INFERNO_DMA_SURFACE_FORMAT_ARGB;
        case APPLE_SCALER_FORMAT_RGBA          : return INFERNO_DMA_SURFACE_FORMAT_ABGR;
        case APPLE_SCALER_FORMAT_L10R          : return INFERNO_DMA_SURFACE_FORMAT_AR30;
        case APPLE_SCALER_FORMAT_YUV_420       : return INFERNO_DMA_SURFACE_FORMAT_NV12;
        case APPLE_SCALER_FORMAT_YUV_422       : return INFERNO_DMA_SURFACE_FORMAT_NV16;
        case APPLE_SCALER_FORMAT_YUV_444       : return INFERNO_DMA_SURFACE_FORMAT_NV24;
        case APPLE_SCALER_FORMAT_YUVS_YUVF     : return INFERNO_DMA_SURFACE_FORMAT_YUY2;
        case APPLE_SCALER_FORMAT_X420_XF20     : return INFERNO_DMA_SURFACE_FORMAT_P010;
        case APPLE_SCALER_FORMAT_X422_XF22     : return INFERNO_DMA_SURFACE_FORMAT_P210;
        case APPLE_SCALER_FORMAT_X444_XF44_XW44: return INFERNO_DMA_SURFACE_FORMAT_P410;
        case APPLE_SCALER_FORMAT_L008          :
        case APPLE_SCALER_FORMAT_L010          :
        case APPLE_SCALER_FORMAT_L012          :
        case APPLE_SCALER_FORMAT_L016          : return INFERNO_DMA_SURFACE_FORMAT_I400;
        default                                : return INFERNO_DMA_SURFACE_FORMAT_NONE;
    }
}

static uint32_t apple_scaler_format_depth(AppleScalerFormat format)
{
    switch (format) {
        case APPLE_SCALER_FORMAT_L008: return 8;
        case APPLE_SCALER_FORMAT_L010: return 10;
        case APPLE_SCALER_FORMAT_L012: return 12;
        case APPLE_SCALER_FORMAT_L016: return 16;
        default                      : return 0;
    }
}

static AppleScalerFormat apple_scaler_subsampled_format(uint32_t hw_subsampling, AppleScalerFormat first)
{
    qemu_build_assert(APPLE_SCALER_FORMAT_YUV_422 == APPLE_SCALER_FORMAT_YUV_420 + 1);
    qemu_build_assert(APPLE_SCALER_FORMAT_YUV_444 == APPLE_SCALER_FORMAT_YUV_420 + 2);
    qemu_build_assert(APPLE_SCALER_FORMAT_X422_XF22 == APPLE_SCALER_FORMAT_X420_XF20 + 1);
    qemu_build_assert(APPLE_SCALER_FORMAT_T422_Y422_FAMILY == APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE + 1);
    qemu_build_assert(APPLE_SCALER_FORMAT_Y444_FAMILY == APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE + 2);
    qemu_build_assert(APPLE_SCALER_FORMAT_PF22_P422 == APPLE_SCALER_FORMAT_PW20_P420 + 1);
    qemu_build_assert(APPLE_SCALER_FORMAT_PF44_P444 == APPLE_SCALER_FORMAT_PW20_P420 + 2);
    qemu_build_assert(APPLE_SCALER_FORMAT_PTW2_YP2F_YP2V == APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V + 1);
    qemu_build_assert(APPLE_SCALER_FORMAT_Y4F4_TW44_FAMILY == APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V + 2);

    switch (hw_subsampling) {
        case SUBSAMPLING_420: return first;
        case SUBSAMPLING_422: return first + 1;
        case SUBSAMPLING_444: return first + 2;
        default             : return APPLE_SCALER_FORMAT_UNKNOWN;
    }
}

/* NOTE: MSR8-specific */
static AppleScalerFormat apple_scaler_convert_hw_format(uint32_t hw_format, uint32_t hw_swizzle)
{
    uint32_t hw_subsampling;
    uint32_t hw_sample;
    bool     hw_planar;
    bool     hw_premult;

    if (REG_FIELD_EX32(hw_format, SRCDST_FORMAT, CHROMA_COMPRESSED)
        || REG_FIELD_EX32(hw_format, SRCDST_FORMAT, LUMA_COMPRESSED)
        || REG_FIELD_EX32(hw_format, SRCDST_FORMAT, CHROMA_ADDRESSING_FORMAT) != ADDRESSING_FORMAT_LINEAR
        || REG_FIELD_EX32(hw_format, SRCDST_FORMAT, LUMA_ADDRESSING_FORMAT) != ADDRESSING_FORMAT_LINEAR)
    {
        return APPLE_SCALER_FORMAT_UNKNOWN;
    }

    hw_subsampling = REG_FIELD_EX32(hw_format, SRCDST_FORMAT, SUBSAMPLING);
    hw_premult     = REG_FIELD_EX32(hw_format, SRCDST_FORMAT, PREMULTIPLIED);
    hw_planar      = REG_FIELD_EX32(hw_format, SRCDST_FORMAT, PLANE_LAYOUT) != 0;
    hw_sample      = REG_FIELD_EX32(hw_format, SRCDST_FORMAT, SAMPLE_FORMAT);

    switch (hw_sample) {
        case 0x01: /* 8-bit */
            return hw_planar ? apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_YUV_420) :
                               APPLE_SCALER_FORMAT_L008;
        case 0x02: /* 10-bit */
            if (!hw_planar) { return APPLE_SCALER_FORMAT_L010; }
            return hw_subsampling == SUBSAMPLING_444 ?
                       APPLE_SCALER_FORMAT_X444_XF44_XW44 :
                       apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_X420_XF20);
        case 0x03: /* 12-bit */
            return hw_planar ?
                       apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE) :
                       APPLE_SCALER_FORMAT_L012;
        case 0x05: return hw_planar ? APPLE_SCALER_FORMAT_UNKNOWN : APPLE_SCALER_FORMAT_L016; /* 16-bit */
        case 0x08: return APPLE_SCALER_FORMAT_L565;
        case 0x0B: return APPLE_SCALER_FORMAT_1555_5551;
        case 0x10: return apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_PW20_P420);
        case 0x18: return hw_planar ? APPLE_SCALER_FORMAT_B3A8 : APPLE_SCALER_FORMAT_XF4P_X44P;
        case 0x19: return APPLE_SCALER_FORMAT_L10R;
        case 0x1E:
            if (hw_subsampling == SUBSAMPLING_422) { return APPLE_SCALER_FORMAT_YUVS_YUVF; }
            if (hw_premult) { return APPLE_SCALER_FORMAT_PBGR; }
            /*
             * M2ScalerCSCColorConversionControlMSR8::getSwizzleRegisterFields_gatedContext
             * There is also GRBA, but it has no libyuv equivalent.
             */
            if (REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_1) != SWIZZLE_COMPONENT_GREEN
                || REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_3) != SWIZZLE_COMPONENT_ALPHA)
            {
                return APPLE_SCALER_FORMAT_UNKNOWN;
            }
            if (REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_0) == SWIZZLE_COMPONENT_BLUE
                && REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_2) == SWIZZLE_COMPONENT_RED)
            {
                return APPLE_SCALER_FORMAT_BGRA;
            }
            if (REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_0) == SWIZZLE_COMPONENT_RED
                && REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_2) == SWIZZLE_COMPONENT_BLUE)
            {
                return APPLE_SCALER_FORMAT_RGBA;
            }
            return APPLE_SCALER_FORMAT_UNKNOWN;
        case 0x20: return apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V);
        case 0x23:
            if (hw_subsampling == SUBSAMPLING_422) { return APPLE_SCALER_FORMAT_XF2P; }
            return hw_premult ? APPLE_SCALER_FORMAT_PBGH : APPLE_SCALER_FORMAT_W40A;
        case 0x24: return APPLE_SCALER_FORMAT_RGHA_FP16;
        default  : return APPLE_SCALER_FORMAT_UNKNOWN;
    }
}

static dma_addr_t apple_scaler_offset_bytes(uint32_t offsets_reg, uint32_t stride, uint32_t bytes_per_sample)
{
    dma_addr_t h = REG_FIELD_EX32(offsets_reg, SRCDST_CL_OFFSET, HORIZONTAL);
    dma_addr_t v = REG_FIELD_EX32(offsets_reg, SRCDST_CL_OFFSET, VERTICAL);
    return v * stride + h * bytes_per_sample;
}

typedef struct
{
    AppleScalerFormat format;
    InfernoDMASurface surface;
} AppleScalerSurface;

static bool apple_scaler_surface_init(AppleScalerSurface* surface, AppleScalerState* scaler, SourceDest srcdst)
{
    const AppleScalerSrcDstConfig* cfg = &scaler->srcdst[srcdst];
    InfernoDMASurface*             out = &surface->surface;

    surface->format = apple_scaler_convert_hw_format(cfg->format, cfg->swizzle);
    out->format     = apple_scaler_surface_format(surface->format);
    out->depth      = apple_scaler_format_depth(surface->format);
    out->width      = REG_FIELD_EX32(cfg->size, SRCDST_SIZE, WIDTH);
    out->height     = REG_FIELD_EX32(cfg->size, SRCDST_SIZE, HEIGHT);
    out->stride[0]  = cfg->stride[LUMA];
    out->stride[1]  = cfg->stride[CHROMA];

    SCALER_INFO("%s w/h %ux%u stride 0x%X 0x%X format %s @ cl 0x%X 0x%X offsets %d %d",
                srcdst == SOURCE ? "src" : "dst", out->width, out->height, cfg->stride[CHROMA], cfg->stride[LUMA],
                apple_scaler_stringify_format(surface->format), cfg->base[CHROMA], cfg->base[LUMA],
                cfg->offsets[CHROMA], cfg->offsets[LUMA]);

    if (out->format == INFERNO_DMA_SURFACE_FORMAT_NONE || out->width == 0 || out->height == 0) { return false; }

    out->base[0] = cfg->base[LUMA]
                   + apple_scaler_offset_bytes(cfg->offsets[LUMA], cfg->stride[LUMA],
                                               inferno_dma_surface_bytes_per_pixel(out->format, out->depth, 0));
    out->base[1] = cfg->base[CHROMA]
                   + apple_scaler_offset_bytes(cfg->offsets[CHROMA], cfg->stride[CHROMA],
                                               inferno_dma_surface_bytes_per_pixel(out->format, out->depth, 1));
    return true;
}

#if 0
static void apple_scaler_export_file(SourceDest srcdst, uint32_t width, uint32_t height, AppleScalerFormat format,
                                     LumaChroma lc, const void* contents, dma_addr_t size)
{
    char fn[128];
    snprintf(fn, sizeof(fn), "/Users/visual/Downloads/Scaler/%s_%lld_%c_%ux%u_%s", srcdst == SOURCE ? "src" : "dst",
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), lc == CHROMA ? 'c' : 'l', width, height,
             apple_scaler_stringify_format(format));
    g_file_set_contents(fn, contents, size, NULL);
}

static void apple_scaler_export_source(AppleScalerState* scaler, const AppleScalerSurface* surface)
{
    const AppleScalerSrcDstConfig* cfg = &scaler->srcdst[SOURCE];
    LumaChroma                     lc;

    for (lc = CHROMA; lc < LUMA_CHROMA_COUNT; lc++) {
        dma_addr_t size =
            (dma_addr_t)cfg->stride[lc] * (lc == LUMA ? surface->surface.height : surface->surface.height / 2);
        void*      buf;

        if (size == 0) { continue; }

        buf = g_malloc(size);
        if (dma_memory_read(&scaler->dma_as, cfg->base[lc], buf, size, MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
            apple_scaler_export_file(SOURCE, surface->surface.width, surface->surface.height, surface->format, lc, buf,
                                     size);
        }
        g_free(buf);
    }
}
#else
    #define apple_scaler_export_source(scaler, surface) (void)0
#endif

static void apple_scaler_update_irqs(AppleScalerState* scaler)
{ qemu_set_irq(scaler->irqs[0], qatomic_read(&scaler->irq_sts) != 0); }

static void apple_scaler_signal_frame_done(AppleScalerState* scaler)
{
    qatomic_fetch_inc(&scaler->frame_count);
    /*
     * AppleM2ScalerCSCHalMSR8::scalerErrorInterruptStatus(): &0x3e0 compression/decompression, &0x1e018 read/write,
     * &0x1000 AXI, and &0x2 MSR_CTRL_DBGSTS (0x9C).
     */
    qatomic_set(&scaler->irq_sts, 1);
    qatomic_set(&scaler->running, false);
    apple_scaler_update_irqs(scaler);
}

typedef struct
{
    enum RotationMode rotation;
    bool              flip_x;
    bool              flip_y;
} AppleScalerTransform;

static AppleScalerTransform apple_scaler_transform(const AppleScalerState* scaler)
{
    uint32_t             cfg        = scaler->flip_rotate_cfg;
    bool                 rotate_90  = REG_FIELD_EX32(cfg, FLIP_ROTATE_CFG, ROTATE_90);
    bool                 rotate_180 = REG_FIELD_EX32(cfg, FLIP_ROTATE_CFG, ROTATE_180);
    AppleScalerTransform transform;

    transform.flip_x = REG_FIELD_EX32(cfg, FLIP_ROTATE_CFG, FLIP_X);
    transform.flip_y = REG_FIELD_EX32(cfg, FLIP_ROTATE_CFG, FLIP_Y);

    if (transform.flip_x && transform.flip_y) {
        transform.flip_x = transform.flip_y = false;
        rotate_180                          = !rotate_180;
    }

    transform.rotation = rotate_90 ? (rotate_180 ? kRotate270 : kRotate90) : (rotate_180 ? kRotate180 : kRotate0);
    return transform;
}

static bool apple_scaler_transform_is_identity(const AppleScalerTransform* transform)
{ return transform->rotation == kRotate0 && !transform->flip_x && !transform->flip_y; }

static void apple_scaler_process(AppleScalerState* scaler, const AppleScalerSurface* src, const AppleScalerSurface* dst)
{
    AppleScalerTransform transform = apple_scaler_transform(scaler);
    InfernoPixbufKind    dst_kind  = inferno_dma_surface_pixbuf_kind(dst->surface.format);
    InfernoPixbuf        image, tmp;
    int                  ret;

    if (apple_scaler_transform_is_identity(&transform) && src->format == dst->format
        && src->surface.width == dst->surface.width && src->surface.height == dst->surface.height)
    {
        inferno_dma_surface_copy(&scaler->dma_as, &src->surface, &dst->surface);
        return;
    }

    if (!inferno_dma_surface_load(&image, &scaler->dma_as, &src->surface)) {
        SCALER_INFO("conversion from %s failed", apple_scaler_stringify_format(src->format));
        return;
    }

    if (transform.rotation != kRotate0) {
        inferno_pixbuf_rotate(&tmp, &image, transform.rotation);
        inferno_pixbuf_destroy(&image);
        image = tmp;
    }

    if (transform.flip_x || transform.flip_y) {
        inferno_pixbuf_mirror(&tmp, &image, transform.flip_x);
        inferno_pixbuf_destroy(&image);
        image = tmp;
    }

    if (image.width != dst->surface.width || image.height != dst->surface.height) {
        inferno_pixbuf_scale(&tmp, &image, dst->surface.width, dst->surface.height);
        inferno_pixbuf_destroy(&image);
        image = tmp;
    }

    if (image.kind != dst_kind) {
        ret = inferno_pixbuf_convert(&tmp, &image, dst_kind, INFERNO_PIXBUF_MATRIX_BT601);
        inferno_pixbuf_destroy(&image);
        if (ret) {
            SCALER_INFO("conversion to the destination layout failed: %d", ret);
            inferno_pixbuf_destroy(&tmp);
            return;
        }
        image = tmp;
    }

    if (!inferno_dma_surface_store(&image, &scaler->dma_as, &dst->surface)) {
        SCALER_INFO("conversion to %s failed", apple_scaler_stringify_format(dst->format));
    }
    inferno_pixbuf_destroy(&image);
}

static void apple_scaler_bh(void* opaque)
{
    AppleScalerState*  scaler = opaque;
    AppleScalerSurface src, dst;
    bool               src_ok, dst_ok;

    QEMU_LOCK_GUARD(&scaler->lock);

    src_ok = apple_scaler_surface_init(&src, scaler, SOURCE);
    dst_ok = apple_scaler_surface_init(&dst, scaler, DEST);

    if (src_ok && dst_ok) { apple_scaler_process(scaler, &src, &dst); }
    else {
        SCALER_INFO("unsupported format pair %s -> %s", apple_scaler_stringify_format(src.format),
                    apple_scaler_stringify_format(dst.format));
        apple_scaler_export_source(scaler, &src);
    }

    apple_scaler_signal_frame_done(scaler);
}

static void apple_scaler_reset_enter(Object* obj, ResetType type)
{
    AppleScalerState* scaler = APPLE_SCALER(obj);

    QEMU_LOCK_GUARD(&scaler->lock);

    qemu_bh_cancel(scaler->bh);
    memset(&scaler->srcdst, 0, sizeof(scaler->srcdst));
    scaler->flip_rotate_cfg = 0;
    qatomic_set(&scaler->frame_count, 0);
    qatomic_set(&scaler->irq_sts, 0);
    qatomic_set(&scaler->running, false);
}

static void apple_scaler_reset_hold(Object* obj, ResetType type)
{
    AppleScalerState* scaler = APPLE_SCALER(obj);

    QEMU_LOCK_GUARD(&scaler->lock);

    apple_scaler_update_irqs(scaler);
}

static void apple_scaler_reset(AppleScalerState* scaler, ResetType type)
{
    apple_scaler_reset_enter(OBJECT(scaler), type);
    apple_scaler_reset_hold(OBJECT(scaler), type);
}

static uint32_t* apple_scaler_reg_ptr(AppleScalerState* scaler, hwaddr index)
{
    switch (index) {
        case R_SRC_FORMAT        : return &scaler->srcdst[SOURCE].format;
        case R_SRC_LUMA_BASE     : return &scaler->srcdst[SOURCE].base[LUMA];
        case R_SRC_CHROMA_BASE   : return &scaler->srcdst[SOURCE].base[CHROMA];
        case R_SRC_LUMA_STRIDE   : return &scaler->srcdst[SOURCE].stride[LUMA];
        case R_SRC_CHROMA_STRIDE : return &scaler->srcdst[SOURCE].stride[CHROMA];
        case R_SRC_SWIZZLE       : return &scaler->srcdst[SOURCE].swizzle;
        case R_SRC_SIZE          : return &scaler->srcdst[SOURCE].size;
        case R_SRC_CHROMA_OFFSETS: return &scaler->srcdst[SOURCE].offsets[CHROMA];
        case R_SRC_LUMA_OFFSETS  : return &scaler->srcdst[SOURCE].offsets[LUMA];
        case R_DST_FORMAT        : return &scaler->srcdst[DEST].format;
        case R_DST_LUMA_BASE     : return &scaler->srcdst[DEST].base[LUMA];
        case R_DST_CHROMA_BASE   : return &scaler->srcdst[DEST].base[CHROMA];
        case R_DST_LUMA_STRIDE   : return &scaler->srcdst[DEST].stride[LUMA];
        case R_DST_CHROMA_STRIDE : return &scaler->srcdst[DEST].stride[CHROMA];
        case R_DST_CHROMA_OFFSETS: return &scaler->srcdst[DEST].offsets[CHROMA];
        case R_DST_LUMA_OFFSETS  : return &scaler->srcdst[DEST].offsets[LUMA];
        case R_DST_SWIZZLE       : return &scaler->srcdst[DEST].swizzle;
        case R_DST_SIZE          : return &scaler->srcdst[DEST].size;
        case R_FLIP_ROTATE_CFG   : return &scaler->flip_rotate_cfg;
        default                  : return NULL;
    }
}

static void apple_scaler_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleScalerState* scaler = opaque;
    uint32_t*         reg;

    // SCALER_INFO("0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, addr, data);

    addr >>= 2;

    reg = apple_scaler_reg_ptr(scaler, addr);
    if (reg != NULL) {
        if (!qatomic_read(&scaler->running)) { *reg = (uint32_t)data; }
        return;
    }

    switch (addr) {
        case R_GLBL_IRQSTS:
            qatomic_and(&scaler->irq_sts, ~(uint32_t)data);
            qemu_mutex_lock(&scaler->lock);
            apple_scaler_update_irqs(scaler);
            qemu_mutex_unlock(&scaler->lock);
            break;
        case R_GLBL_CTRL:
            if (REG_FIELD_EX32(data, GLBL_CTRL, RESET)) { apple_scaler_reset(scaler, RESET_TYPE_COLD); }
            break;
        case R_CTRL_COMMAND:
            if (REG_FIELD_EX32(data, CTRL_COMMAND, RUN) && !qatomic_cmpxchg(&scaler->running, false, true)) {
                qemu_bh_schedule(scaler->bh);
            }
            break;
        default: break;
    }
}

static uint64_t apple_scaler_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleScalerState* scaler = opaque;
    const uint32_t*   reg;
    uint32_t          ret;

    addr >>= 2;

    reg = apple_scaler_reg_ptr(scaler, addr);
    if (reg != NULL) { return *reg; }

    switch (addr) {
        case R_GLBL_VER:
            // SOC 0x8 -> 0x90082/0x9009B/0x900A7
            ret = 0x9009B;
            break;
        case R_GLBL_STS     : ret = qatomic_read(&scaler->running) ? R_GLBL_STS_RUNNING_MASK : 0; break;
        case R_GLBL_IRQSTS  : ret = qatomic_read(&scaler->irq_sts); break;
        case R_GLBL_FRAMECNT: ret = qatomic_read(&scaler->frame_count); break;
        default             : ret = 0; break;
    }

    // SCALER_INFO("0x" HWADDR_FMT_plx " -> 0x%X", addr << 2, ret);

    return ret;
}

static const MemoryRegionOps apple_scaler_reg_ops = {
    .write                 = apple_scaler_reg_write,
    .read                  = apple_scaler_reg_read,
    .endianness            = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_scaler_apiodma_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    // AppleScalerState *scaler = opaque;

    // SCALER_INFO("0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, addr, data);

    switch (addr) {
        default: {
            break;
        }
    }
}

static uint64_t apple_scaler_apiodma_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    // AppleScalerState *scaler = opaque;
    uint64_t ret;

    switch (addr >> 2) {
        default: ret = 0; break;
    }

    // SCALER_INFO("0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx, addr, ret);

    return ret;
}

static const MemoryRegionOps apple_scaler_apiodma_reg_ops = {
    .write                 = apple_scaler_apiodma_reg_write,
    .read                  = apple_scaler_apiodma_reg_read,
    .endianness            = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_scaler_init(Object* obj)
{
    SysBusDevice*     sbd;
    AppleScalerState* scaler;
    int               i;

    sbd    = SYS_BUS_DEVICE(obj);
    scaler = APPLE_SCALER(sbd);

    qemu_mutex_init(&scaler->lock);

    memory_region_init_io(&scaler->regs_iomem, OBJECT(scaler), &apple_scaler_reg_ops, scaler, TYPE_APPLE_SCALER ".regs",
                          0x20000);
    memory_region_enable_lockless_io(&scaler->regs_iomem);
    memory_region_init_io(&scaler->apiodma_iomem, OBJECT(scaler), &apple_scaler_apiodma_reg_ops, scaler,
                          TYPE_APPLE_SCALER ".regs-apiodma", 0x4000);
    memory_region_enable_lockless_io(&scaler->apiodma_iomem);
    sysbus_init_mmio(sbd, &scaler->regs_iomem);
    sysbus_init_mmio(sbd, &scaler->apiodma_iomem);

    for (i = 0; i < 2; i++) { sysbus_init_irq(sbd, &scaler->irqs[i]); }

    scaler->bh = aio_bh_new(qemu_get_aio_context(), apple_scaler_bh, scaler);
}

static void apple_scaler_realize(DeviceState* dev, Error** errp)
{
    address_space_init(&APPLE_SCALER(dev)->dma_as, MEMORY_REGION(object_property_get_link(OBJECT(dev), "dma-mr", errp)),
                       "scaler0.dma");
}

static void apple_scaler_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_scaler_reset_enter;
    rc->phases.hold  = apple_scaler_reset_hold;

    dc->realize = apple_scaler_realize;
    dc->desc    = "Apple M2 Scaler and Color Space Converter";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(AppleScalerState, apple_scaler, APPLE_SCALER, SYS_BUS_DEVICE)
