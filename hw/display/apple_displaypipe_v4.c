/*
 * Apple Display Pipe V4 Controller.
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

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "block/aio.h"
#include "hw/display/apple_apt_m3.h"
#include "hw/display/apple_displaypipe_v4.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "system/dma.h"
#include "ui/console.h"
#include "pixman.h"

#if 0
    #define ADP_INFO(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#else
    #define ADP_INFO(fmt, ...) \
        do { }                 \
        while (0);
#endif

/**
 * Block Bases (DisplayTarget5)
 * 0x00000  |  APT M3 Instruction Memory
 * 0x08000  |  APT M3 Data Memory
 * 0x10000  |  APT M3 CPU
 * 0x40000  |  Control
 * 0x48000  |  Vertical Frame Timing Generator
 * 0x50000  |  Generic Pixel Pipe 0
 * 0x58000  |  Generic Pixel Pipe 1
 * 0x60000  |  Blend Unit
 * 0x70000  |  White Point Correction
 * 0x7C000  |  Pixel Response Correction
 * 0x80000  |  Dither
 * 0x82000  |  Dither: Enchanced ST Dither 0
 * 0x83000  |  Dither: Enchanced ST Dither 1
 * 0x84000  |  Content-Dependent Frame Duration
 * 0x88000  |  SPLR (Sub-Pixel Layout R?)
 * 0x90000  |  Burn-In Compensation Sampler
 * 0x98000  |  Sub-Pixel Uniformity Correction
 * 0xA0000  |  PDC (Panel Dither Correction?)
 * 0xB0000  |  PCC (Pixel Color Correction?)
 * 0xD0000  |  PCC M3 Instruction Memory
 * 0xD8000  |  PCC M3 Data Memory
 * 0xE0000  |  PCC M3 CPU
 * 0xF0000  |  DBM (Dynamic Backlight Modulation?)
 */

/**
 * Interrupt Indices
 * 0 | Maybe VBlank
 * 1 | APT M3 (Video Mode Mailbox)
 * 2 | Maybe GP0
 * 3 | Maybe GP1
 * 4 | ?
 * 5 | ?
 * 6 | ?
 * 7 | ?
 * 8 | PCC M3 (PCC Mailbox)
 * 9 | ?
 */

#define ADP_V4_PANEL_HZ (60)

/* Active size, front porch, sync pulse, back porch */
#define ADP_V4_VFTG_TIMING_COUNT (4)

/* 2 GenPipes, 2 Layers per GenPipe */
#define ADP_V4_GP_COUNT    (2)
#define ADP_V4_LAYER_COUNT (2)

#define ADP_V4_M3_IMEM_SIZE       (0x8000)
#define ADP_V4_M3_DMEM_BASE       (0x8000)
#define ADP_V4_M3_DMEM_SIZE       (0x8000)
#define ADP_V4_M3_OUTCMDQ_SIZE    (0x80)
#define ADP_V4_M3_OUTCMD_ACTIVEQ  (1)
#define ADP_V4_M3_ACTIVEQ_SIZE    (0x20)
#define ADP_V4_M3_ACTIVEQ_STRIDE  (ADP_V4_M3_ACTIVEQ_SIZE * sizeof(uint32_t))
#define ADP_V4_M3_MAILBOX_DEPTH   (4)
#define ADP_V4_M3_WPC_PENDING_OFF (0x88)
#define ADP_V4_M3_WPC_SLOTS       (2)

#define ADP_V4_TSQ_DEPTH          (8)
#define ADP_V4_TIMEBASE_HZ        (24000000)
#define ADP_V4_APIODMA_BASE_COUNT (4)

typedef struct
{
    uint32_t config_control;
    uint32_t pixel_format;
    uint16_t dest_width;
    uint16_t dest_height;
    uint32_t data_start;
    uint32_t data_end;
    uint32_t stride;
    uint16_t src_width;
    uint16_t src_height;
    void*    buf;
    uint32_t buf_len;
    uint32_t buf_capacity;
    /// Cached image, to not remake it every single run.
    pixman_image_t* image;
} ADPV4GenPipeState;

typedef struct
{
    QemuMutex         lock;
    uint8_t           index;
    ADPV4GenPipeState state;
} ADPV4GenPipe;

typedef struct
{
    uint32_t layer_config[ADP_V4_LAYER_COUNT];
} ADPV4BlendUnitState;

typedef struct
{
    uint64_t time;
    uint32_t cmd_addr;
    uint32_t cmd_ctrl;
    uint32_t config;
} ADPV4TSQEntry;

struct AppleDisplayPipeV4State
{
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion        up_regs;
    uint32_t            width;
    uint32_t            height;
    MemoryRegion*       dma_mr;
    AddressSpace        dma_as;
    AddressSpace        piodma_as;
    qemu_irq            irqs[9];
    uint32_t            int_status;
    uint32_t            int_enable;
    uint32_t            normal_mode_status;
    uint32_t            commit_pending;
    uint32_t            present_pending;
    bool                direct_scanout;
    bool                boot_fb_valid;
    uint32_t            boot_fb_iova;
    uint32_t            vftg_control;
    uint32_t            vftg_timings[ADP_V4_VFTG_TIMING_COUNT];
    QEMUTimer*          vsync_timer;
    uint64_t            next_vsync_ns;
    ADPV4GenPipe        genpipe[ADP_V4_GP_COUNT];
    ADPV4BlendUnitState blend_unit;
    QemuConsole*        console;
    QEMUBH*             update_disp_image_bh;
    QemuMutex           m3_lock;
    uint32_t            m3_state;
    uint32_t            m3_int_enable;
    uint32_t            m3_inbox_ctrl;
    uint32_t            m3_outbox_ctrl;
    uint32_t            m3_outbox[ADP_V4_M3_MAILBOX_DEPTH];
    uint32_t            m3_outbox_head;
    uint32_t            m3_outbox_count;
    uint32_t            m3_outcmdq_put;
    uint32_t            m3_activeq_put;
    bool                m3_replaying;
    bool                m3_activeq_deferred;
    uint32_t            m3_activeq_deferred_flags;
    uint32_t            m3_dmem[ADP_V4_M3_DMEM_SIZE / sizeof(uint32_t)];
    uint8_t             m3_imem[ADP_V4_M3_IMEM_SIZE];
    hwaddr              m3_outcmdq;
    hwaddr              m3_activeq;
    uint32_t            apiodma_base[ADP_V4_APIODMA_BASE_COUNT];
    bool                timebase_enabled;
    uint64_t            timebase_load;
    uint64_t            timebase_value;
    int64_t             timebase_ns;
    ADPV4TSQEntry       tsq_entry;
    ADPV4TSQEntry       tsq[ADP_V4_TSQ_DEPTH];
    uint32_t            tsq_head;
    uint32_t            tsq_count;
};

// clang-format off
// APT M3
REG32(APT_M3_VERSION, 0x10000)
#define APT_M3_VERSION_EXPECTED (0x307)
REG32(APT_M3_STATE, 0x10008)
#define APT_M3_STATE_RUN (1)
REG32(APT_M3_INT_ENABLE, 0x10028)
REG32(APT_M3_INT_STATUS, 0x1002C)
    REG_FIELD(APT_M3_INT, INBOX_EMPTY, 0, 1)
    REG_FIELD(APT_M3_INT, OUTBOX_NOT_EMPTY, 3, 1)
REG32(APT_M3_INBOX_CTRL, 0x10030)
REG32(APT_M3_INBOX_WRITE, 0x10034)
REG32(APT_M3_OUTBOX_CTRL, 0x1003C)
REG32(APT_M3_OUTBOX_READ, 0x10044)
    REG_FIELD(APT_M3_MAILBOX_CTRL, ENABLE, 0, 1)
    REG_FIELD(APT_M3_MAILBOX_CTRL, FULL, 16, 1)
    REG_FIELD(APT_M3_MAILBOX_CTRL, EMPTY, 17, 1)
    REG_FIELD(APT_M3_OUTCMD, TYPE, 0, 3)
    REG_FIELD(APT_M3_OUTCMD, INDEX, 8, 5)
    REG_FIELD(APT_M3_ACTIVEQ, TAG, 0, 4)
    REG_FIELD(APT_M3_ACTIVEQ, REPLAY_ACTIVE, 4, 1)
    REG_FIELD(APT_M3_ACTIVEQ, REPLAY_EXITED, 5, 1)
    REG_FIELD(APT_M3_ACTIVEQ, APIODMA_DONE, 7, 1)

// APIODMA
REG32(APIODMA_CFG, 0x40000)
REG32(APIODMA_CFG_MEM_SRC, 0x40010)
REG32(APIODMA_CFG_MEM_DAT, 0x40014)
REG32(APIODMA_CFG_DMA_CFG_PIO_RD, 0x4001C)
REG32(APIODMA_CFG_DMA_CFG_PIO_WR, 0x40020)
REG32(APIODMA_BASE_ADDR_0, 0x40024)
REG32(APIODMA_BASE_ADDR_3, 0x40030)
    REG_FIELD(APIODMA_BASE_ADDR, PAGE, 14, 18)
    REG_FIELD(APIODMA_PACKET, INCREMENT, 0, 1)
    REG_FIELD(APIODMA_PACKET, OFFSET, 2, 16)
    REG_FIELD(APIODMA_PACKET, COUNT, 18, 12)
    REG_FIELD(APIODMA_PACKET, BASE, 30, 2)
#define APIODMA_PACKET_PAGE_SHIFT (18)

// timestamp queue
REG32(TSQ_TIMEBASE_CONTROL, 0x44004)
    REG_FIELD(TSQ_TIMEBASE_CONTROL, ENABLE, 0, 1)
REG32(TSQ_TIMEBASE_LOAD_LO, 0x44008)
REG32(TSQ_TIMEBASE_LOAD_HI, 0x4400C)
REG32(TSQ_TIMEBASE_LOAD, 0x4401C)
REG32(TSQ_TIMEBASE_LO, 0x44020)
REG32(TSQ_TIMEBASE_HI, 0x44024)
REG32(TSQ_ENTRY_TIME_LO, 0x44814)
REG32(TSQ_ENTRY_TIME_HI, 0x44818)
REG32(TSQ_ENTRY_CMD_ADDR, 0x4481C)
REG32(TSQ_ENTRY_CMD_CTRL, 0x44824)
    REG_FIELD(TSQ_ENTRY_CMD_CTRL, LENGTH, 8, 22)
REG32(TSQ_ENTRY_CONFIG, 0x44828)
    REG_FIELD(TSQ_ENTRY_CONFIG, TAG, 16, 4)
    REG_FIELD(TSQ_ENTRY_CONFIG, REPLAY, 20, 1)
    REG_FIELD(TSQ_ENTRY_CONFIG, SUBMIT, 30, 1)

// pipe control
REG32(CONTROL_SHADOW_FIFO_STATUS, 0x45004)
    REG_FIELD(CONTROL_SHADOW_FIFO_STATUS, EMPTY, 0, 1)
    REG_FIELD(CONTROL_SHADOW_FIFO_STATUS, PENDING, 4, 3)
REG32(CONTROL_SHADOW_FIFO_POWER_CONTROL, 0x45010)
REG32(CONTROL_NORMAL_MODE_STATUS, 0x45808)
    REG_FIELD(CONTROL_NORMAL_MODE_STATUS, AUTO, 0, 1)
REG32(CONTROL_MODE_CHANGE_REQUEST, 0x4580C)
REG32(CONTROL_INT_STATUS, 0x45818)
    REG_FIELD(CONTROL_INT, MODE_CHANGED, 0, 1)
    REG_FIELD(CONTROL_INT, UNDERRUN, 3, 1)
    REG_FIELD(CONTROL_INT, SWAP_COMPLETE, 10, 1)
    REG_FIELD(CONTROL_INT, SUB_FRAME_OVERFLOW, 11, 1)
    REG_FIELD(CONTROL_INT, M3, 13, 1)
    REG_FIELD(CONTROL_INT, PCC, 17, 1)
    REG_FIELD(CONTROL_INT, CDFD, 19, 1)
    REG_FIELD(CONTROL_INT, FRAME_PROCESSED, 20, 1)
    REG_FIELD(CONTROL_INT, AXI_READ_ERR, 30, 1)
    REG_FIELD(CONTROL_INT, AXI_WRITE_ERR, 31, 1)
REG32(CONTROL_INT_ENABLE, 0x4581C)
REG32(CONTROL_HILO, 0x45830)
REG32(CONTROL_DISP_FIFO_POWER_CONTROL, 0x45834)
REG32(CONTROL_POWER_GATE_TIMER_GP0, 0x4583C)
REG32(CONTROL_POWER_GATE_TIMER_GP1, 0x45840)
REG32(CONTROL_POWER_GATE_TIMER_PPP, 0x45844)
REG32(CONTROL_POWER_GATE_TIMER_HILO, 0x45848)

// pipe config
REG32(CONTROL_PIPE_CONFIG_SPARE_0, 0x46000)
REG32(CONTROL_VERSION, 0x46020)
#define CONTROL_VERSION_A0 (0x70044)
#define CONTROL_VERSION_A1 (0x70045)
REG32(CONTROL_UPDATE, 0x4602C)
    REG_FIELD(CONTROL_UPDATE, TAG, 8, 4)
    REG_FIELD(CONTROL_UPDATE, COMMIT, 12, 1)
REG32(CONTROL_FRAME_SIZE, 0x4603C)
REG32(CONTROL_CONFIG, 0x46040)
REG32(CONTROL_OUT_FIFO_CLK_GATE, 0x46074)
REG32(CONTROL_OUT_FIFO_DEPTH, 0x46084)
REG32(CONTROL_COMPRESSION_CFG, 0x460E0)
REG32(CONTROL_BACKPRESSURE, 0x46120)
REG32(CONTROL_POWER_GATE_CTRL, 0x46158)
REG32(CONTROL_BIS_UPDATE_INTERVAL, 0x46198)
REG32(CONTROL_MIN_BANDWIDTH_RATE, 0x461C0)
REG32(CONTROL_BANDWIDTH_RATE_SCALE_FACTOR, 0x461C4)
REG32(CONTROL_PIO_DMA_BANDWIDTH_RATE, 0x461C8)
REG32(CONTROL_REPLAY_DMA_BANDWIDTH_RATE, 0x461CC)
REG32(CONTROL_GATE_CONTROL, 0x461D0)
REG32(CONTROL_READ_LINK_GATE_METRIC, 0x461D4)
REG32(CONTROL_READ_LTR_CONFIG, 0x461D8)
REG32(CONTROL_LTR_TIMER, 0x461DC)
REG32(CONTROL_WR_LTR_CONFIG, 0x461E0)
REG32(CONTROL_RD_PIXEL_LTR_CONV_FACTOR, 0x461E4)
REG32(CONTROL_WR_PIXEL_LTR_CONV_FACTOR, 0x461E8)
REG32(CONTROL_LTR_OFFSET, 0x461EC)
REG32(CONTROL_EVENT_ORDERING, 0x46254)
REG32(CONTROL_MIN_TLTR, 0x46258)
REG32(CONTROL_START_CLTR, 0x4625C)
REG32(CONTROL_OPE_OFFSET0, 0x46268)
REG32(CONTROL_OPE_OFFSET1, 0x4626C)
REG32(CONTROL_GP_AGGR_OFFSET0, 0x4627C)
REG32(CONTROL_GP_AGGR_OFFSET1, 0x46280)
REG32(CONTROL_BIC_AGGR_OFFSET, 0x46284)
REG32(CONTROL_SPUC_AGGR_OFFSET, 0x46288)
REG32(CONTROL_PDC_RD_AGGR_OFFSET, 0x4628C)
REG32(CONTROL_PREFETCH_BWR_CONFIG, 0x46290)
REG32(CONTROL_PREFETCH_LTR_CONFIG, 0x46294)
REG32(CONTROL_PREFETCH_LTR_PEG_CONFIG, 0x46298)
REG32(CONTROL_COLUMN_REPEAT, 0x462D0)
REG32(CONTROL_LINK_GATE_LB_CONFIG, 0x462D4)
REG32(CONTROL_REPLAY_READ_HEADER_AF_CONFIG, 0x462F0)
REG32(CONTROL_REPLAY_READ_TILE_AF_CONFIG, 0x462F4)
REG32(CONTROL_REPLAY_WRITE_HEADER_AF_CONFIG, 0x462F8)
REG32(CONTROL_REPLAY_WRITE_TILE_AF_CONFIG, 0x462FC)
REG32(CONTROL_DART_AF_CONFIG, 0x46300)
REG32(CONTROL_PIODMA_READ_AF_CONFIG, 0x46304)
REG32(CONTROL_PIODMA_WRITE_AF_CONFIG, 0x46308)
REG32(CONTROL_WRITE_LINK_GATE_METRIC, 0x4630C)
REG32(CONTROL_BIS_WR_AGGR_CONFIGURATION, 0x46314)
REG32(CONTROL_REPLAY_WR_AGGR_CONFIGURATION, 0x46318)
REG32(CONTROL_PDC_WR_AGGR_CONFIGURATION, 0x4631C)
REG32(CONTROL_WR_LINK_GATE_CONFIG, 0x46320)
REG32(CONTROL_WR_LTR_OFFSET_RAMP_CONFIG, 0x46324)

// vertical frame timing generator
REG32(VFTG_DEBUG, 0x48000)
REG32(VFTG_CONTROL, 0x48024)
    REG_FIELD(VFTG_CONTROL, UPDATE, 14, 2)
    REG_FIELD(VFTG_CONTROL, RUNNING, 30, 1)
    REG_FIELD(VFTG_CONTROL, ENABLE, 31, 1)
REG32(VFTG_ACTIVE_SIZE, 0x4802C)
REG32(VFTG_FRONT_PORCH, 0x48030)
REG32(VFTG_SYNC_PULSE, 0x48034)
REG32(VFTG_BACK_PORCH, 0x48038)
REG32(VFTG_VBLANK_POSITION, 0x48044)
REG32(VFTG_AUTO_IDLE_SUBFRAME_COUNT, 0x48050)
// 0x48200-0x48280: IFP_CFG
// 0x48300-0x48380: BSYNC1
// 0x48400-0x48480: EB_BSYNC1
// 0x48500-0x48580: BSYNC0
// 0x48600-0x48680: EB_BSYNC0
REG32(VFTG_EB_OFFSET, 0x48700)
REG32(VFTG_IDLE_SUB_FRAME_CONFIG, 0x48708)
REG32(VFTG_IDLE_SUB_FRAME_TOP_CONFIG, 0x4870C)
REG32(VFTG_N1_PARAM, 0x48780)
REG32(VFTG_N1_PARAM_VBLANK, 0x48784)
REG32(VFTG_N5_A_PARAM, 0x48788)
REG32(VFTG_N5_A_PARAM_VBLANK, 0x4878C)
REG32(VFTG_N5_B_PARAM, 0x48790)
REG32(VFTG_N5_B_PARAM_VBLANK, 0x48794)
REG32(VFTG_N6_PARAM, 0x48798)
REG32(VFTG_N7_PARAM, 0x4879C)
REG32(VFTG_N7_PARAM_VBLANK, 0x487A0)
REG32(VFTG_LINK_UP_TIME_CONFIG, 0x487A4)
REG32(VFTG_AUX_WAKE_CONFIG, 0x487A8)
REG32(VFTG_AUX_DISABLE_TIME, 0x487AC)
REG32(VFTG_POL_OFFSET, 0x487C4)
REG32(VFTG_SWIZZLE_CONFIG_EVEN_ROWS, 0x487D0)
REG32(VFTG_SWIZZLE_CONFIG_EVEN_ROWS_LAST_PAIR, 0x487D4)
REG32(VFTG_SWIZZLE_CONFIG_ODD_ROWS, 0x487D8)
REG32(VFTG_SWIZZLE_CONFIG_ODD_ROWS_LAST_PAIR, 0x487DC)
REG32(VFTG_CROP_CONTROL, 0x4881C)
REG32(VFTG_MIPI_MODE, 0x48828)

#define GP_BLOCK_BASE (0x50000)
#define GP_BLOCK_SIZE (0x8000)
REG32(GP_CONFIG_CONTROL, 0x4)
    REG_FIELD(GP_CONFIG_CONTROL, NO_SCALE, 0, 1)
    REG_FIELD(GP_CONFIG_CONTROL, SCALE_CROPPED, 4, 1)
    REG_FIELD(GP_CONFIG_CONTROL, TWO_PLANE, 8, 1)
    REG_FIELD(GP_CONFIG_CONTROL, MULTI_PLANE, 12, 1)
    REG_FIELD(GP_CONFIG_CONTROL, GAMMA_SPACE, 16, 2)
    REG_FIELD(GP_CONFIG_CONTROL, HDR, 24, 1)
    REG_FIELD(GP_CONFIG_CONTROL, WIDE_GAMUT, 30, 1)
    REG_FIELD(GP_CONFIG_CONTROL, ENABLED, 31, 1)
// 0x8, 0xC = ??
// 0x10 = HDR_CONFIG?
REG32(GP_AF_CONFIG_0, 0x14)
REG32(GP_AF_CONFIG_1, 0x18)
REG32(GP_PIXEL_FORMAT, 0x1C)
#define GP_PIXEL_FORMAT_BGRA ((BIT32(4) << 22) | BIT32(24) | (3 << 13))
#define GP_PIXEL_FORMAT_ARGB ((BIT32(4) << 22) | BIT32(24))
#define GP_PIXEL_FORMAT_COMPRESSED BIT32(30)
// 0x20 = ??
REG32(GP_AGX_CONFIG, 0x24)
REG32(GP_LAYER_0_HTPC_CONFIG, 0x28)
REG32(GP_LAYER_1_HTPC_CONFIG, 0x2C)
REG32(GP_LAYER_0_DATA_START, 0x30)
REG32(GP_LAYER_1_DATA_START, 0x34)
REG32(GP_LAYER_0_DATA_END, 0x40)
REG32(GP_LAYER_1_DATA_END, 0x44)
REG32(GP_LAYER_0_HEADER_BASE, 0x48)
REG32(GP_LAYER_1_HEADER_BASE, 0x4C)
REG32(GP_LAYER_0_HEADER_END, 0x58)
REG32(GP_LAYER_1_HEADER_END, 0x5C)
REG32(GP_LAYER_0_STRIDE, 0x60)
REG32(GP_LAYER_1_STRIDE, 0x64)
REG32(GP_LAYER_0_POSITION, 0x68)
REG32(GP_LAYER_1_POSITION, 0x6C)
REG32(GP_LAYER_0_DIMENSIONS, 0x70)
REG32(GP_LAYER_1_DIMENSIONS, 0x74)
REG32(GP_SRC_RECT, 0x78)
REG32(GP_DEST_POSITION, 0x7C)
REG32(GP_DEST_DIMENSIONS, 0x80)
REG32(GP_SRC_ACTIVE_REGION_0_POSITION, 0x98)
REG32(GP_SRC_ACTIVE_REGION_1_POSITION, 0x9C)
REG32(GP_SRC_ACTIVE_REGION_2_POSITION, 0xA0)
REG32(GP_SRC_ACTIVE_REGION_3_POSITION, 0xA4)
REG32(GP_SRC_ACTIVE_REGION_0_DIMENSIONS, 0xA8)
REG32(GP_SRC_ACTIVE_REGION_1_DIMENSIONS, 0xAC)
REG32(GP_SRC_ACTIVE_REGION_2_DIMENSIONS, 0xB0)
REG32(GP_SRC_ACTIVE_REGION_3_DIMENSIONS, 0xB4)
REG32(GP_CRC_DATA, 0x160)
REG32(GP_DMA_BANDWIDTH_RATE, 0x170)
REG32(GP_STATUS, 0x184)
#define GP_STATUS_DECOMPRESSION_FAIL BIT32(0)

#define GP_BLOCK_BASE_FOR(i) (GP_BLOCK_BASE + ((i) * GP_BLOCK_SIZE))
#define GP_BLOCK_END_FOR(i) (GP_BLOCK_BASE_FOR(i) + (GP_BLOCK_SIZE - 1))

#define BLEND_BLOCK_BASE (0x60000)
#define BLEND_BLOCK_SIZE (0x8000)
REG32(BLEND_CONFIG, 0x4)
REG32(BLEND_BG, 0x8)
REG32(BLEND_LAYER_0_BG, 0xC)
REG32(BLEND_LAYER_1_BG, 0x10)
REG32(BLEND_LAYER_0_CONFIG, 0x14)
REG32(BLEND_LAYER_1_CONFIG, 0x18)
#define BLEND_LAYER_CONFIG_PIPE(v) ((v) & 0xF)
#define BLEND_LAYER_CONFIG_MODE(v) (((v) >> 4) & 0xF)
#define BLEND_MODE_NONE (0)
#define BLEND_MODE_ALPHA (1)
#define BLEND_MODE_PREMULT (2)
#define BLEND_MODE_BYPASS (3)
REG32(BLEND_DEGAMMA_TABLE_R, 0x1C)
REG32(BLEND_DEGAMMA_TABLE_G, 0x1024)
REG32(BLEND_DEGAMMA_TABLE_B, 0x202C)
// REG32(BLEND_??, 0x3034)
REG32(BLEND_PIXCAP_CONFIG, 0x303C)
// clang-format on

static void adp_v4_update_irqs(AppleDisplayPipeV4State* genpipe)
{ qemu_set_irq(genpipe->irqs[0], (qatomic_read(&genpipe->int_enable) & qatomic_read(&genpipe->int_status)) != 0); }

static pixman_format_code_t adp_v4_gp_fmt_to_pixman(ADPV4GenPipe* genpipe)
{
    if ((genpipe->state.pixel_format & GP_PIXEL_FORMAT_BGRA) == GP_PIXEL_FORMAT_BGRA) {
        ADP_INFO("gp%d: pixel format is BGRA (0x%X).", genpipe->index, genpipe->state.pixel_format);
        return PIXMAN_b8g8r8a8;
    }
    if ((genpipe->state.pixel_format & GP_PIXEL_FORMAT_ARGB) == GP_PIXEL_FORMAT_ARGB) {
        ADP_INFO("gp%d: pixel format is ARGB (0x%X).", genpipe->index, genpipe->state.pixel_format);
        return PIXMAN_a8r8g8b8;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "gp%d: pixel format is unknown (0x%X).\n", genpipe->index,
                  genpipe->state.pixel_format);
    return 0;
}

static void adp_v4_gp_read(ADPV4GenPipe* genpipe, AddressSpace* dma_as)
{
    uint32_t len;

    genpipe->state.buf_len = 0;

    // TODO: Decompress the data and display it properly.
    if (genpipe->state.pixel_format & GP_PIXEL_FORMAT_COMPRESSED) {
        qemu_log_mask(LOG_GUEST_ERROR, "gp%d: dropping frame as it's compressed.\n", genpipe->index);
        return;
    }

    ADP_INFO("gp%d: width and height is %dx%d.", genpipe->index, genpipe->state.src_width, genpipe->state.src_height);
    ADP_INFO("gp%d: stride is %d.", genpipe->index, genpipe->state.stride);

    len = genpipe->state.src_height * genpipe->state.stride;
    if (genpipe->state.buf_capacity < len) {
        g_free(genpipe->state.buf);
        genpipe->state.buf          = g_malloc(len);
        genpipe->state.buf_capacity = len;
    }

    if (dma_memory_read(dma_as, genpipe->state.data_start, genpipe->state.buf, len, MEMTXATTRS_UNSPECIFIED) == MEMTX_OK)
    {
        genpipe->state.buf_len = len;
    }
    else {
        qemu_log_mask(LOG_GUEST_ERROR, "gp%d: failed to read from DMA.\n", genpipe->index);
    }
}

static void adp_v4_gp_image_changed_check(ADPV4GenPipe* genpipe, uint32_t old, uint32_t new)
{
    if (old == new) { return; }

    qemu_pixman_image_unref(genpipe->state.image);
    genpipe->state.image = NULL;
}

#define ADP_V4_GP_REG_WRITE_SET_WITH_CHECK_VAL(_field, _val)                \
    do {                                                                    \
        typeof(_val) val = _val;                                            \
        adp_v4_gp_image_changed_check(genpipe, genpipe->state._field, val); \
        genpipe->state._field = val;                                        \
    }                                                                       \
    while (0)
#define ADP_V4_GP_REG_WRITE_SET_WITH_CHECK(_field) ADP_V4_GP_REG_WRITE_SET_WITH_CHECK_VAL(_field, (uint32_t)data);

static void adp_v4_gp_reg_write(ADPV4GenPipe* genpipe, hwaddr addr, uint64_t data)
{
    switch (addr >> 2) {
        case R_GP_CONFIG_CONTROL: {
            ADP_INFO("gp%d: control <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            genpipe->state.config_control = (uint32_t)data;
            break;
        }
        case R_GP_PIXEL_FORMAT: {
            ADP_INFO("gp%d: pixel format <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            ADP_V4_GP_REG_WRITE_SET_WITH_CHECK(pixel_format);
            break;
        }
        case R_GP_LAYER_0_DATA_START: {
            ADP_INFO("gp%d: layer 0 data start <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            genpipe->state.data_start = (uint32_t)data;
            break;
        }
        case R_GP_LAYER_0_DATA_END: {
            ADP_INFO("gp%d: layer 0 data end <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            genpipe->state.data_end = (uint32_t)data;
            break;
        }
        case R_GP_LAYER_0_STRIDE: {
            ADP_INFO("gp%d: layer 0 stride <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            ADP_V4_GP_REG_WRITE_SET_WITH_CHECK(stride);
            break;
        }
        case R_GP_LAYER_0_DIMENSIONS: {
            ADP_V4_GP_REG_WRITE_SET_WITH_CHECK_VAL(src_height, data & 0xFFFF);
            ADP_V4_GP_REG_WRITE_SET_WITH_CHECK_VAL(src_width, (data >> 16) & 0xFFFF);
            ADP_INFO("gp%d: layer 0 dimensions <- 0x" HWADDR_FMT_plx " (%dx%d)", genpipe->index, data,
                     genpipe->state.src_width, genpipe->state.src_height);
            break;
        }
        case R_GP_DEST_DIMENSIONS: {
            genpipe->state.dest_height = data & 0xFFFF;
            genpipe->state.dest_width  = (data >> 16) & 0xFFFF;
            ADP_INFO("gp%d: dest dimensions <- 0x" HWADDR_FMT_plx " (%dx%d)", genpipe->index, data,
                     genpipe->state.dest_width, genpipe->state.dest_height);
            break;
        }
        default: {
            ADP_INFO("gp%d: unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, genpipe->index, addr, data);
            break;
        }
    }
}

static uint32_t adp_v4_gp_reg_read(ADPV4GenPipe* genpipe, hwaddr addr)
{
    switch (addr >> 2) {
        case R_GP_CONFIG_CONTROL: {
            ADP_INFO("gp%d: control -> 0x%X", genpipe->index, genpipe->state.config_control);
            return genpipe->state.config_control;
        }
        case R_GP_PIXEL_FORMAT: {
            ADP_INFO("gp%d: pixel format -> 0x%X", genpipe->index, genpipe->state.pixel_format);
            return genpipe->state.pixel_format;
        }
        case R_GP_LAYER_0_DATA_START: {
            ADP_INFO("gp%d: layer 0 data start -> 0x%X", genpipe->index, genpipe->state.data_start);
            return genpipe->state.data_start;
        }
        case R_GP_LAYER_0_DATA_END: {
            ADP_INFO("gp%d: layer 0 data end -> 0x%X", genpipe->index, genpipe->state.data_end);
            return genpipe->state.data_end;
        }
        case R_GP_LAYER_0_STRIDE: {
            ADP_INFO("gp%d: layer 0 stride -> 0x%X", genpipe->index, genpipe->state.stride);
            return genpipe->state.stride;
        }
        case R_GP_LAYER_0_DIMENSIONS: {
            ADP_INFO("gp%d: layer 0 dimensions -> 0x%X (%dx%d)", genpipe->index,
                     (genpipe->state.src_width << 16) | genpipe->state.src_height, genpipe->state.src_width,
                     genpipe->state.src_height);
            return ((uint32_t)genpipe->state.src_width << 16) | genpipe->state.src_height;
        }
        case R_GP_DEST_DIMENSIONS: {
            ADP_INFO("gp%d: dest dimensions -> 0x%X (%dx%d)", genpipe->index,
                     (genpipe->state.dest_width << 16) | genpipe->state.dest_height, genpipe->state.dest_width,
                     genpipe->state.dest_height);
            return ((uint32_t)genpipe->state.dest_width << 16) | genpipe->state.dest_height;
        }
        default: {
            ADP_INFO("gp%d: unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx, genpipe->index, addr, (hwaddr)0);
            return 0;
        }
    }
}

static void adp_v4_gp_reset(ADPV4GenPipe* genpipe, uint8_t index)
{
    qemu_pixman_image_unref(genpipe->state.image);
    g_free(genpipe->state.buf);
    genpipe->state = (ADPV4GenPipeState){0};
    genpipe->index = index;
}

static void adp_v4_blend_reg_write(ADPV4BlendUnitState* blend, uint64_t addr, uint64_t data)
{
    switch (addr >> 2) {
        case R_BLEND_LAYER_0_CONFIG: {
            ADP_INFO("blend: layer 0 config <- 0x" HWADDR_FMT_plx, data);
            blend->layer_config[0] = (uint32_t)data;
            break;
        }
        case R_BLEND_LAYER_1_CONFIG: {
            blend->layer_config[1] = (uint32_t)data;
            ADP_INFO("blend: layer 1 config <- 0x" HWADDR_FMT_plx, data);
            break;
        }
        default: {
            ADP_INFO("blend: unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, addr, data);
            break;
        }
    }
}

static uint64_t adp_v4_blend_reg_read(ADPV4BlendUnitState* blend, uint64_t addr)
{
    switch (addr >> 2) {
        case R_BLEND_LAYER_0_CONFIG: {
            ADP_INFO("blend: layer 0 config -> 0x%X", blend->layer_config[0]);
            return blend->layer_config[0];
        }
        case R_BLEND_LAYER_1_CONFIG: {
            ADP_INFO("blend: layer 1 config -> 0x%X", blend->layer_config[1]);
            return blend->layer_config[1];
        }
        default: {
            ADP_INFO("blend: unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx, addr, (hwaddr)0);
            return 0;
        }
    }
}

static void adp_v4_blend_reset(ADPV4BlendUnitState* blend) { *blend = (ADPV4BlendUnitState){0}; }

static uint64_t adp_v4_timebase_now(AppleDisplayPipeV4State* adp)
{
    if (!adp->timebase_enabled) { return adp->timebase_value; }

    return adp->timebase_value
           + muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - adp->timebase_ns, ADP_V4_TIMEBASE_HZ,
                      NANOSECONDS_PER_SECOND);
}

static uint32_t adp_v4_m3_int_status(AppleDisplayPipeV4State* adp)
{
    uint32_t status = R_APT_M3_INT_INBOX_EMPTY_MASK;

    if (adp->m3_outbox_count != 0) { status |= R_APT_M3_INT_OUTBOX_NOT_EMPTY_MASK; }

    return status;
}

static void adp_v4_m3_update_irq(AppleDisplayPipeV4State* adp)
{ qemu_set_irq(adp->irqs[1], (adp->m3_int_enable & adp_v4_m3_int_status(adp)) != 0); }

static uint32_t* adp_v4_m3_dmem(AppleDisplayPipeV4State* adp, hwaddr addr)
{ return &adp->m3_dmem[(addr - ADP_V4_M3_DMEM_BASE) / sizeof(uint32_t)]; }

static void adp_v4_m3_find_layout(AppleDisplayPipeV4State* adp)
{
    uint32_t outcmdq;
    uint32_t flags;

    if (!apple_apt_m3_find_queues(adp->m3_imem, sizeof(adp->m3_imem), &outcmdq, &flags)
        || flags < ADP_V4_M3_ACTIVEQ_STRIDE * 2 || flags >= ADP_V4_M3_DMEM_SIZE)
    {
        adp->m3_outcmdq = 0;
        adp->m3_activeq = 0;
        qemu_log_mask(LOG_UNIMP, "apt m3: could not find the queue layout in the firmware.\n");
        return;
    }

    adp->m3_outcmdq = ADP_V4_M3_DMEM_BASE + outcmdq;
    adp->m3_activeq = ADP_V4_M3_DMEM_BASE + flags - ADP_V4_M3_ACTIVEQ_STRIDE * 2;
    ADP_INFO("apt m3: OutCmdQ @ 0x%" HWADDR_PRIX ", ActiveQ @ 0x%" HWADDR_PRIX, adp->m3_outcmdq, adp->m3_activeq);
}

static void adp_v4_m3_post_activeq(AppleDisplayPipeV4State* adp, uint64_t time, uint32_t flags)
{
    uint32_t slot = adp->m3_activeq_put;
    uint32_t cmd  = adp->m3_outcmdq_put;

    if (adp->m3_activeq == 0) {
        qemu_log_mask(LOG_UNIMP, "apt m3: queue layout unknown, dropping ActiveQ entry.\n");
        return;
    }

    if (adp->m3_outbox_count == ADP_V4_M3_MAILBOX_DEPTH) {
        qemu_log_mask(LOG_GUEST_ERROR, "apt m3: outbox full, dropping ActiveQ entry.\n");
        return;
    }

    adp->m3_activeq_put = (slot + 1) % ADP_V4_M3_ACTIVEQ_SIZE;
    adp->m3_outcmdq_put = (cmd + 1) % ADP_V4_M3_OUTCMDQ_SIZE;

    *adp_v4_m3_dmem(adp, adp->m3_activeq + slot * sizeof(uint32_t))                                = time >> 32;
    *adp_v4_m3_dmem(adp, adp->m3_activeq + ADP_V4_M3_ACTIVEQ_STRIDE + slot * sizeof(uint32_t))     = (uint32_t)time;
    *adp_v4_m3_dmem(adp, adp->m3_activeq + ADP_V4_M3_ACTIVEQ_STRIDE * 2 + slot * sizeof(uint32_t)) = flags;
    *adp_v4_m3_dmem(adp, adp->m3_outcmdq + cmd * sizeof(uint32_t)) =
        REG_FIELD_DP32(REG_FIELD_DP32(0, APT_M3_OUTCMD, TYPE, ADP_V4_M3_OUTCMD_ACTIVEQ), APT_M3_OUTCMD, INDEX, slot);

    adp->m3_outbox[(adp->m3_outbox_head + adp->m3_outbox_count) % ADP_V4_M3_MAILBOX_DEPTH]  = cmd;
    adp->m3_outbox_count                                                                   += 1;

    adp_v4_m3_update_irq(adp);
}

static bool adp_v4_m3_reg_write(AppleDisplayPipeV4State* adp, hwaddr addr, uint32_t data)
{
    QEMU_LOCK_GUARD(&adp->m3_lock);

    if (addr < ADP_V4_M3_IMEM_SIZE) {
        stl_le_p(adp->m3_imem + addr, data);
        return true;
    }

    if (addr >= ADP_V4_M3_DMEM_BASE && addr < ADP_V4_M3_DMEM_BASE + ADP_V4_M3_DMEM_SIZE) {
        *adp_v4_m3_dmem(adp, addr) = data;
        return true;
    }

    switch (addr >> 2) {
        case R_APT_M3_STATE: {
            ADP_INFO("apt m3: state <- 0x%X", data);
            adp->m3_state = data;
            if (data == APT_M3_STATE_RUN) { adp_v4_m3_find_layout(adp); }
            return true;
        }
        case R_APT_M3_INT_ENABLE: {
            adp->m3_int_enable = data;
            adp_v4_m3_update_irq(adp);
            return true;
        }
        case R_APT_M3_INT_STATUS :
        case R_APT_M3_INBOX_WRITE: {
            return true;
        }
        case R_APT_M3_INBOX_CTRL: {
            adp->m3_inbox_ctrl = data & R_APT_M3_MAILBOX_CTRL_ENABLE_MASK;
            return true;
        }
        case R_APT_M3_OUTBOX_CTRL: {
            adp->m3_outbox_ctrl = data & R_APT_M3_MAILBOX_CTRL_ENABLE_MASK;
            return true;
        }
        case R_APIODMA_BASE_ADDR_0 ... R_APIODMA_BASE_ADDR_3: {
            adp->apiodma_base[(addr >> 2) - R_APIODMA_BASE_ADDR_0] = data;
            return true;
        }
        case R_TSQ_TIMEBASE_CONTROL: {
            bool enable = REG_FIELD_EX32(data, TSQ_TIMEBASE_CONTROL, ENABLE);

            if (enable == adp->timebase_enabled) { return true; }
            adp->timebase_value   = adp_v4_timebase_now(adp);
            adp->timebase_ns      = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            adp->timebase_enabled = enable;
            return true;
        }
        case R_TSQ_TIMEBASE_LOAD: {
            if (data & 1) {
                adp->timebase_value = adp->timebase_load;
                adp->timebase_ns    = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            }
            return true;
        }
        case R_TSQ_TIMEBASE_LOAD_LO:
        case R_TSQ_TIMEBASE_LO     : {
            adp->timebase_load = deposit64(adp->timebase_load, 0, 32, data);
            return true;
        }
        case R_TSQ_TIMEBASE_LOAD_HI:
        case R_TSQ_TIMEBASE_HI     : {
            adp->timebase_load = deposit64(adp->timebase_load, 32, 32, data);
            return true;
        }
        case R_TSQ_ENTRY_TIME_LO: {
            adp->tsq_entry.time = deposit64(adp->tsq_entry.time, 0, 32, data);
            return true;
        }
        case R_TSQ_ENTRY_TIME_HI: {
            adp->tsq_entry.time = deposit64(adp->tsq_entry.time, 32, 32, data);
            return true;
        }
        case R_TSQ_ENTRY_CMD_ADDR: {
            adp->tsq_entry.cmd_addr = data;
            return true;
        }
        case R_TSQ_ENTRY_CMD_CTRL: {
            adp->tsq_entry.cmd_ctrl = data;
            return true;
        }
        case R_TSQ_ENTRY_CONFIG: {
            adp->tsq_entry.config = data;
            if (!REG_FIELD_EX32(data, TSQ_ENTRY_CONFIG, SUBMIT)) { return true; }

            ADP_INFO("tsq: submit @ 0x%" PRIX64 ": cmd 0x%X ctrl 0x%X config 0x%X", adp->tsq_entry.time,
                     adp->tsq_entry.cmd_addr, adp->tsq_entry.cmd_ctrl, data);
            if (adp->tsq_count == ADP_V4_TSQ_DEPTH) {
                qemu_log_mask(LOG_GUEST_ERROR, "tsq: queue full, dropping entry.\n");
                return true;
            }
            adp->tsq[(adp->tsq_head + adp->tsq_count) % ADP_V4_TSQ_DEPTH]  = adp->tsq_entry;
            adp->tsq_count                                                += 1;
            return true;
        }
        default: {
            return false;
        }
    }
}

static bool adp_v4_m3_reg_read(AppleDisplayPipeV4State* adp, hwaddr addr, uint32_t* data)
{
    QEMU_LOCK_GUARD(&adp->m3_lock);

    if (addr < ADP_V4_M3_IMEM_SIZE) {
        *data = ldl_le_p(adp->m3_imem + addr);
        return true;
    }

    if (addr >= ADP_V4_M3_DMEM_BASE && addr < ADP_V4_M3_DMEM_BASE + ADP_V4_M3_DMEM_SIZE) {
        *data = *adp_v4_m3_dmem(adp, addr);
        return true;
    }

    switch (addr >> 2) {
        case R_APT_M3_VERSION: {
            *data = APT_M3_VERSION_EXPECTED;
            return true;
        }
        case R_APT_M3_STATE: {
            *data = adp->m3_state;
            return true;
        }
        case R_APT_M3_INT_ENABLE: {
            *data = adp->m3_int_enable;
            return true;
        }
        case R_APT_M3_INT_STATUS: {
            *data = adp_v4_m3_int_status(adp);
            return true;
        }
        case R_APT_M3_INBOX_CTRL: {
            *data = adp->m3_inbox_ctrl | R_APT_M3_MAILBOX_CTRL_EMPTY_MASK;
            return true;
        }
        case R_APT_M3_OUTBOX_CTRL: {
            *data = adp->m3_outbox_ctrl;
            if (adp->m3_outbox_count == 0) { *data |= R_APT_M3_MAILBOX_CTRL_EMPTY_MASK; }
            if (adp->m3_outbox_count == ADP_V4_M3_MAILBOX_DEPTH) { *data |= R_APT_M3_MAILBOX_CTRL_FULL_MASK; }
            return true;
        }
        case R_APT_M3_OUTBOX_READ: {
            if (adp->m3_outbox_count == 0) {
                qemu_log_mask(LOG_GUEST_ERROR, "apt m3: outbox read while empty.\n");
                *data = 0;
                return true;
            }
            *data                 = adp->m3_outbox[adp->m3_outbox_head];
            adp->m3_outbox_head   = (adp->m3_outbox_head + 1) % ADP_V4_M3_MAILBOX_DEPTH;
            adp->m3_outbox_count -= 1;
            adp_v4_m3_update_irq(adp);
            return true;
        }
        case R_APIODMA_BASE_ADDR_0 ... R_APIODMA_BASE_ADDR_3: {
            *data = adp->apiodma_base[(addr >> 2) - R_APIODMA_BASE_ADDR_0];
            return true;
        }
        case R_TSQ_TIMEBASE_CONTROL: {
            *data = REG_FIELD_DP32(0, TSQ_TIMEBASE_CONTROL, ENABLE, adp->timebase_enabled);
            return true;
        }
        case R_TSQ_TIMEBASE_LO: {
            *data = adp_v4_timebase_now(adp);
            return true;
        }
        case R_TSQ_TIMEBASE_HI: {
            *data = adp_v4_timebase_now(adp) >> 32;
            return true;
        }
        default: {
            return false;
        }
    }
}

static void adp_v4_vsync_start(AppleDisplayPipeV4State* adp)
{
    uint64_t period = NANOSECONDS_PER_SECOND / ADP_V4_PANEL_HZ;

    adp->next_vsync_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period;
    timer_mod_ns(adp->vsync_timer, adp->next_vsync_ns);
}

static void adp_v4_vftg_set_control(AppleDisplayPipeV4State* adp, uint32_t value)
{
    uint32_t old;

    value = REG_FIELD_DP32(value, VFTG_CONTROL, UPDATE, 0);
    value = REG_FIELD_DP32(value, VFTG_CONTROL, RUNNING, REG_FIELD_EX32(value, VFTG_CONTROL, ENABLE));
    old   = qatomic_xchg(&adp->vftg_control, value);

    if (REG_FIELD_EX32(value, VFTG_CONTROL, ENABLE) == REG_FIELD_EX32(old, VFTG_CONTROL, ENABLE)) { return; }

    if (REG_FIELD_EX32(value, VFTG_CONTROL, ENABLE)) { adp_v4_vsync_start(adp); }
}

static void adp_v4_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleDisplayPipeV4State* adp = opaque;

    if (addr >= 0x200000) {    // some weird shadow shit
        addr -= 0x200000;
    }

    if (addr >= GP_BLOCK_BASE_FOR(0) && addr < (BLEND_BLOCK_BASE + BLEND_BLOCK_SIZE)
        && qatomic_read(&adp->commit_pending))
    {
        qemu_log_mask(LOG_GUEST_ERROR, "disp: write @ 0x" HWADDR_FMT_plx " while a commit is pending.\n", addr);
        return;
    }

    if (addr >= GP_BLOCK_BASE_FOR(0) && addr < GP_BLOCK_END_FOR(0)) {
        return adp_v4_gp_reg_write(&adp->genpipe[0], addr - GP_BLOCK_BASE_FOR(0), data);
    }

    if (addr >= GP_BLOCK_BASE_FOR(1) && addr < GP_BLOCK_END_FOR(1)) {
        return adp_v4_gp_reg_write(&adp->genpipe[1], addr - GP_BLOCK_BASE_FOR(1), data);
    }

    if (addr >= BLEND_BLOCK_BASE && addr < (BLEND_BLOCK_BASE + BLEND_BLOCK_SIZE)) {
        return adp_v4_blend_reg_write(&adp->blend_unit, addr - BLEND_BLOCK_BASE, data);
    }

    if (adp_v4_m3_reg_write(adp, addr, (uint32_t)data)) { return; }

    switch (addr >> 2) {
        case R_CONTROL_INT_STATUS: {
            ADP_INFO("disp: int status <- 0x%X", (uint32_t)data);
            qatomic_and(&adp->int_status, ~(uint32_t)data);
            adp_v4_update_irqs(adp);
            break;
        }
        case R_CONTROL_INT_ENABLE: {
            ADP_INFO("disp: int enable <- 0x%X", (uint32_t)data);
            qatomic_set(&adp->int_enable, (uint32_t)data);
            adp_v4_update_irqs(adp);
            break;
        }
        case R_CONTROL_MODE_CHANGE_REQUEST: {
            ADP_INFO("disp: mode change request <- 0x%X", (uint32_t)data);
            if (data & 1) {
                qatomic_xor(&adp->normal_mode_status, R_CONTROL_NORMAL_MODE_STATUS_AUTO_MASK);
                qatomic_or(&adp->int_status, R_CONTROL_INT_MODE_CHANGED_MASK);
                adp_v4_update_irqs(adp);
            }
            break;
        }
        case R_VFTG_CONTROL: {
            ADP_INFO("vftg: control <- 0x%X", (uint32_t)data);
            adp_v4_vftg_set_control(adp, (uint32_t)data);
            break;
        }
        case R_VFTG_ACTIVE_SIZE ... R_VFTG_BACK_PORCH: {
            ADP_INFO("vftg: timing @ 0x" HWADDR_FMT_plx " <- 0x%X", addr, (uint32_t)data);
            qatomic_set(&adp->vftg_timings[(addr >> 2) - R_VFTG_ACTIVE_SIZE], (uint32_t)data);
            break;
        }
        case R_CONTROL_UPDATE: {
            ADP_INFO("disp: update <- 0x%X", (uint32_t)data);
            if (REG_FIELD_EX32((uint32_t)data, CONTROL_UPDATE, COMMIT)) {
                if (qatomic_xchg(&adp->commit_pending, 1) != 0) {
                    qemu_log_mask(LOG_GUEST_ERROR, "disp: commit while one is still pending.\n");
                }
                qemu_bh_schedule(adp->update_disp_image_bh);
            }
            break;
        }
        default: {
            ADP_INFO("disp: unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, addr, data);
            break;
        }
    }
}

static uint64_t adp_v4_reg_read(void* const opaque, hwaddr addr, unsigned size)
{
    AppleDisplayPipeV4State* adp = opaque;
    uint32_t                 val;

    if (addr >= 0x200000) {    // ditto
        addr -= 0x200000;
    }

    if (addr >= GP_BLOCK_BASE_FOR(0) && addr < GP_BLOCK_END_FOR(0)) {
        return adp_v4_gp_reg_read(&adp->genpipe[0], addr - GP_BLOCK_BASE_FOR(0));
    }

    if (addr >= GP_BLOCK_BASE_FOR(1) && addr < GP_BLOCK_END_FOR(1)) {
        return adp_v4_gp_reg_read(&adp->genpipe[1], addr - GP_BLOCK_BASE_FOR(1));
    }

    if (addr >= BLEND_BLOCK_BASE && addr < (BLEND_BLOCK_BASE + BLEND_BLOCK_SIZE)) {
        return adp_v4_blend_reg_read(&adp->blend_unit, addr - BLEND_BLOCK_BASE);
    }

    if (adp_v4_m3_reg_read(adp, addr, &val)) { return val; }

    switch (addr >> 2) {
        case R_CONTROL_VERSION: {
            ADP_INFO("disp: version -> 0x%X", CONTROL_VERSION_A1);
            return CONTROL_VERSION_A1;
        }
        case R_CONTROL_FRAME_SIZE: {
            ADP_INFO("disp: frame size -> 0x%X", (adp->width << 16) | adp->height);
            return (adp->width << 16) | adp->height;
        }
        case R_CONTROL_SHADOW_FIFO_STATUS: {
            if (qatomic_read(&adp->commit_pending)) {
                return REG_FIELD_DP32(0, CONTROL_SHADOW_FIFO_STATUS, PENDING, 1);
            }
            return REG_FIELD_DP32(0, CONTROL_SHADOW_FIFO_STATUS, EMPTY, 1);
        }
        case R_CONTROL_NORMAL_MODE_STATUS: {
            ADP_INFO("disp: normal mode status -> 0x%X", qatomic_read(&adp->normal_mode_status));
            return qatomic_read(&adp->normal_mode_status);
        }
        case R_CONTROL_INT_STATUS: {
            ADP_INFO("disp: int status -> 0x%X", qatomic_read(&adp->int_status));
            return qatomic_read(&adp->int_status);
        }
        case R_CONTROL_INT_ENABLE: {
            ADP_INFO("disp: int enable -> 0x%X", qatomic_read(&adp->int_enable));
            return qatomic_read(&adp->int_enable);
        }
        case R_VFTG_CONTROL: {
            ADP_INFO("vftg: control -> 0x%X", qatomic_read(&adp->vftg_control));
            return qatomic_read(&adp->vftg_control);
        }
        case R_VFTG_ACTIVE_SIZE ... R_VFTG_BACK_PORCH: {
            ADP_INFO("vftg: timing @ 0x" HWADDR_FMT_plx " -> 0x%X", addr,
                     qatomic_read(&adp->vftg_timings[(addr >> 2) - R_VFTG_ACTIVE_SIZE]));
            return qatomic_read(&adp->vftg_timings[(addr >> 2) - R_VFTG_ACTIVE_SIZE]);
        }
        default: {
            ADP_INFO("disp: unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx, addr, (hwaddr)0);
            return 0;
        }
    }
}

static const MemoryRegionOps adp_v4_reg_ops = {
    .write                 = adp_v4_reg_write,
    .read                  = adp_v4_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void adp_v4_invalidate(void* opaque) { }

static const GraphicHwOps adp_v4_ops = {
    .invalidate = adp_v4_invalidate,
};

/* The RAM behind [@iova, @iova + @len) if it is one contiguous run, or NULL. */
static void* adp_v4_dma_contiguous_ptr(AppleDisplayPipeV4State* adp, hwaddr iova, hwaddr len)
{
    MemoryRegion* first_mr = NULL;
    MemoryRegion* mr;
    hwaddr        first = 0;
    hwaddr        next  = 0;
    hwaddr        off   = 0;
    hwaddr        xlat;
    hwaddr        plen;

    RCU_READ_LOCK_GUARD();

    while (off < len) {
        plen = len - off;
        mr   = address_space_translate(&adp->dma_as, iova + off, &xlat, &plen, false, MEMTXATTRS_UNSPECIFIED);

        if (!memory_region_is_ram(mr) || plen == 0) { return NULL; }
        if (first_mr == NULL) {
            first_mr = mr;
            first    = xlat;
        }
        else if (mr != first_mr || xlat != next) {
            return NULL;
        }

        next  = xlat + plen;
        off  += plen;
    }

    return memory_region_get_ram_ptr(first_mr) + first;
}

/*
 * Show @genpipe's source 1:1 when it alone covers the panel unscaled: in place
 * if it is one contiguous run of RAM, otherwise by reading it straight into a
 * console surface of the same format and stride.
 */
static bool adp_v4_gp_scanout(AppleDisplayPipeV4State* adp, ADPV4GenPipe* genpipe)
{
    DisplaySurface* surface;
    pixman_image_t* image;
    hwaddr          len;
    void*           ptr;

    if (genpipe->state.pixel_format & GP_PIXEL_FORMAT_COMPRESSED) { return false; }
    if (adp_v4_gp_fmt_to_pixman(genpipe) != PIXMAN_a8r8g8b8) { return false; }
    if (genpipe->state.src_width != adp->width || genpipe->state.src_height != adp->height
        || genpipe->state.dest_width != adp->width || genpipe->state.dest_height != adp->height
        || genpipe->state.stride < adp->width * sizeof(uint32_t) || genpipe->state.stride % sizeof(uint32_t) != 0)
    {
        return false;
    }

    len = (hwaddr)genpipe->state.stride * genpipe->state.src_height;

    ptr = adp_v4_dma_contiguous_ptr(adp, genpipe->state.data_start, len);
    if (ptr != NULL) {
        image = pixman_image_create_bits(PIXMAN_a8r8g8b8, adp->width, adp->height, ptr, genpipe->state.stride);
        dpy_gfx_replace_surface(adp->console, qemu_create_displaysurface_pixman(image));
        qemu_pixman_image_unref(image);
        adp->direct_scanout = true;
        return true;
    }

    surface = qemu_console_surface(adp->console);
    if (adp->direct_scanout || surface_format(surface) != PIXMAN_a8r8g8b8
        || surface_stride(surface) != genpipe->state.stride)
    {
        surface =
            qemu_create_displaysurface_from(adp->width, adp->height, PIXMAN_a8r8g8b8, genpipe->state.stride, NULL);
        dpy_gfx_replace_surface(adp->console, surface);
        adp->direct_scanout = false;
    }

    if (dma_memory_read(&adp->dma_as, genpipe->state.data_start, surface_data(surface), len, MEMTXATTRS_UNSPECIFIED)
        != MEMTX_OK)
    {
        qemu_log_mask(LOG_GUEST_ERROR, "gp%d: failed to read from DMA.\n", genpipe->index);
    }

    return true;
}

// `display-timing-info`
// h_active, h_back_porch, h_front_porch, h_sync_pulse, v_active, v_back_porch,
// v_front_porch, v_sync_pulse
// FIXME: Unhardcode.
static const uint32_t adp_v4_timing_info[] = {828, 144, 1, 1, 1792, 1, 1, 1};

static void adp_v4_boot_display(AppleDisplayPipeV4State* adp)
{
    ADPV4GenPipe* genpipe = &adp->genpipe[0];
    uint32_t      size    = (adp->width << 16) | adp->height;

    // IOMFB's `get_dfb_info` reads the framebuffer back from GP0.
    adp_v4_gp_reg_write(genpipe, A_GP_PIXEL_FORMAT, GP_PIXEL_FORMAT_ARGB);
    adp_v4_gp_reg_write(genpipe, A_GP_LAYER_0_DATA_START, adp->boot_fb_iova);
    adp_v4_gp_reg_write(genpipe, A_GP_LAYER_0_STRIDE, adp->width * sizeof(uint32_t));
    adp_v4_gp_reg_write(genpipe, A_GP_LAYER_0_DIMENSIONS, size);
    adp_v4_gp_reg_write(genpipe, A_GP_DEST_DIMENSIONS, size);
    adp_v4_gp_reg_write(genpipe, A_GP_CONFIG_CONTROL, R_GP_CONFIG_CONTROL_ENABLED_MASK);

    qemu_mutex_lock(&genpipe->lock);
    adp_v4_gp_scanout(adp, genpipe);
    qemu_mutex_unlock(&genpipe->lock);

    adp_v4_reg_write(adp, A_VFTG_ACTIVE_SIZE, (adp_v4_timing_info[4] << 16) | adp_v4_timing_info[0], 4);
    adp_v4_reg_write(adp, A_VFTG_FRONT_PORCH, (adp_v4_timing_info[6] << 16) | adp_v4_timing_info[2], 4);
    adp_v4_reg_write(adp, A_VFTG_SYNC_PULSE, (adp_v4_timing_info[7] << 16) | adp_v4_timing_info[3], 4);
    adp_v4_reg_write(adp, A_VFTG_BACK_PORCH, (adp_v4_timing_info[5] << 16) | adp_v4_timing_info[1], 4);
    adp_v4_reg_write(adp, A_VFTG_CONTROL, R_VFTG_CONTROL_ENABLE_MASK, 4);
}

static void adp_v4_reset_hold(Object* obj, ResetType type)
{
    AppleDisplayPipeV4State* adp = APPLE_DISPLAY_PIPE_V4(obj);
    size_t                   i;

    qatomic_set(&adp->int_status, 0);
    qatomic_set(&adp->int_enable, 0);
    qatomic_set(&adp->normal_mode_status, R_CONTROL_NORMAL_MODE_STATUS_AUTO_MASK);
    WITH_QEMU_LOCK_GUARD(&adp->m3_lock)
    {
        adp->m3_state            = 0;
        adp->m3_int_enable       = 0;
        adp->m3_inbox_ctrl       = 0;
        adp->m3_outbox_ctrl      = 0;
        adp->m3_outbox_head      = 0;
        adp->m3_outbox_count     = 0;
        adp->m3_outcmdq_put      = 0;
        adp->m3_activeq_put      = 0;
        adp->m3_replaying        = false;
        adp->m3_activeq_deferred = false;
        memset(adp->m3_dmem, 0, sizeof(adp->m3_dmem));
        memset(adp->m3_imem, 0, sizeof(adp->m3_imem));
        adp->m3_outcmdq = 0;
        adp->m3_activeq = 0;
        memset(adp->apiodma_base, 0, sizeof(adp->apiodma_base));
        adp->timebase_enabled = false;
        adp->timebase_load    = 0;
        adp->timebase_value   = 0;
        adp->tsq_entry        = (ADPV4TSQEntry){0};
        adp->tsq_head         = 0;
        adp->tsq_count        = 0;
        adp_v4_m3_update_irq(adp);
    }
    qatomic_set(&adp->commit_pending, 0);
    qatomic_set(&adp->present_pending, 0);
    qatomic_set(&adp->vftg_control, 0);
    for (i = 0; i < ARRAY_SIZE(adp->vftg_timings); ++i) { qatomic_set(&adp->vftg_timings[i], 0); }
    timer_del(adp->vsync_timer);

    adp_v4_update_irqs(adp);

    adp->direct_scanout = true;

    for (i = 0; i < ARRAY_SIZE(adp->genpipe); ++i) { adp_v4_gp_reset(&adp->genpipe[i], i); }
    adp_v4_blend_reset(&adp->blend_unit);

    if (adp->boot_fb_valid) { adp_v4_boot_display(adp); }
}

// TODO: handle source/dest position, etc.
static bool adp_v4_gp_draw(ADPV4GenPipe* genpipe, AddressSpace* dma_as, pixman_image_t* disp_image)
{
    pixman_format_code_t fmt;
    pixman_image_t*      image;
    pixman_transform_t   transform;

    if (REG_FIELD_EX32(genpipe->state.config_control, GP_CONFIG_CONTROL, ENABLED) == 0) { return false; }

    qemu_mutex_lock(&genpipe->lock);
    adp_v4_gp_read(genpipe, dma_as);
    qemu_mutex_unlock(&genpipe->lock);

    if (genpipe->state.buf_len != 0) {
        image = genpipe->state.image;
        if (image == NULL) {
            fmt                  = adp_v4_gp_fmt_to_pixman(genpipe);
            genpipe->state.image = image =
                pixman_image_create_bits(fmt, genpipe->state.src_width, genpipe->state.src_height,
                                         (uint32_t*)genpipe->state.buf, genpipe->state.stride);
        }

        if (genpipe->state.src_width != genpipe->state.dest_width
            || genpipe->state.src_height != genpipe->state.dest_height)
        {
            pixman_transform_init_scale(
                &transform, pixman_double_to_fixed((double)genpipe->state.src_width / genpipe->state.dest_width),
                pixman_double_to_fixed((double)genpipe->state.src_height / genpipe->state.dest_height));
            pixman_image_set_filter(image, PIXMAN_FILTER_BILINEAR, NULL, 0);
            pixman_image_set_transform(image, &transform);
        }
        else {
            pixman_image_set_transform(image, NULL);
        }

        pixman_image_composite(PIXMAN_OP_SRC, image, NULL, disp_image, 0, 0, 0, 0, 0, 0, genpipe->state.dest_width,
                               genpipe->state.dest_height);

        return true;
    }

    return false;
}

static ADPV4GenPipe* adp_v4_single_enabled_gp(AppleDisplayPipeV4State* adp)
{
    ADPV4GenPipe* found = NULL;
    int           i;

    for (i = 0; i < ADP_V4_GP_COUNT; ++i) {
        if (REG_FIELD_EX32(adp->genpipe[i].state.config_control, GP_CONFIG_CONTROL, ENABLED) == 0) { continue; }
        if (found != NULL) { return NULL; }
        found = &adp->genpipe[i];
    }

    return found;
}

static void adp_v4_update_disp_bh(void* opaque)
{
    AppleDisplayPipeV4State* adp = opaque;
    ADPV4GenPipe*            genpipe;
    pixman_image_t*          disp_image;
    bool                     drawn = false;
    int                      i;

    genpipe = adp_v4_single_enabled_gp(adp);
    if (genpipe != NULL) {
        qemu_mutex_lock(&genpipe->lock);
        drawn = adp_v4_gp_scanout(adp, genpipe);
        qemu_mutex_unlock(&genpipe->lock);
    }

    if (!drawn) {
        if (adp->direct_scanout) {
            dpy_gfx_replace_surface(adp->console, qemu_create_displaysurface(adp->width, adp->height));
            adp->direct_scanout = false;
        }

        disp_image = qemu_console_surface(adp->console)->image;
        memset(pixman_image_get_data(disp_image), 0,
               (size_t)pixman_image_get_stride(disp_image) * pixman_image_get_height(disp_image));

        for (i = 0; i < ADP_V4_GP_COUNT; ++i) { adp_v4_gp_draw(&adp->genpipe[i], &adp->dma_as, disp_image); }
    }

    qatomic_set(&adp->present_pending, 1);

    qatomic_set(&adp->commit_pending, 0);

    WITH_QEMU_LOCK_GUARD(&adp->m3_lock)
    {
        if (adp->m3_activeq_deferred) {
            adp->m3_activeq_deferred = false;
            adp_v4_m3_post_activeq(adp, adp_v4_timebase_now(adp), adp->m3_activeq_deferred_flags);
        }
    }

    qatomic_or(&adp->int_status, R_CONTROL_INT_FRAME_PROCESSED_MASK);
    adp_v4_update_irqs(adp);
}

static void adp_v4_init(Object* obj)
{
    SysBusDevice*            sbd;
    AppleDisplayPipeV4State* adp;
    int                      i;

    sbd = SYS_BUS_DEVICE(obj);
    adp = APPLE_DISPLAY_PIPE_V4(sbd);

    adp->update_disp_image_bh = aio_bh_new(qemu_get_aio_context(), adp_v4_update_disp_bh, adp);

    memory_region_init_io(&adp->up_regs, OBJECT(sbd), &adp_v4_reg_ops, sbd, TYPE_APPLE_DISPLAY_PIPE_V4 ".regs",
                          0x2FC000);
    memory_region_enable_lockless_io(&adp->up_regs);
    sysbus_init_mmio(sbd, &adp->up_regs);

    qemu_mutex_init(&adp->genpipe[0].lock);
    qemu_mutex_init(&adp->genpipe[1].lock);
    qemu_mutex_init(&adp->m3_lock);

    for (i = 0; i < ARRAY_SIZE(adp->irqs); i++) { sysbus_init_irq(sbd, &adp->irqs[i]); }
}

static void adp_v4_apiodma_replay(AppleDisplayPipeV4State* adp, const ADPV4TSQEntry* entry)
{
    uint32_t             len = REG_FIELD_EX32(entry->cmd_ctrl, TSQ_ENTRY_CMD_CTRL, LENGTH);
    g_autofree uint32_t* buf = g_new(uint32_t, len);
    uint32_t             hdr;
    uint32_t             count;
    uint32_t             base;
    uint32_t             i = 0;
    hwaddr               addr;

    if (dma_memory_read(&adp->piodma_as, entry->cmd_addr, buf, len * sizeof(uint32_t), MEMTXATTRS_UNSPECIFIED)
        != MEMTX_OK)
    {
        qemu_log_mask(LOG_GUEST_ERROR, "apiodma: failed to read command buffer @ 0x%X.\n", entry->cmd_addr);
        return;
    }

    while (i < len) {
        hdr = le32_to_cpu(buf[i++]);
        if ((hdr & 3) == 3) {
            qemu_log_mask(LOG_UNIMP, "apiodma: unsupported packet 0x%X.\n", hdr);
            return;
        }

        count = REG_FIELD_EX32(hdr, APIODMA_PACKET, COUNT) + 1;
        WITH_QEMU_LOCK_GUARD(&adp->m3_lock) { base = adp->apiodma_base[REG_FIELD_EX32(hdr, APIODMA_PACKET, BASE)]; }
        addr = ((hwaddr)REG_FIELD_EX32(base, APIODMA_BASE_ADDR, PAGE) << APIODMA_PACKET_PAGE_SHIFT)
               | (hdr & R_APIODMA_PACKET_OFFSET_MASK);

        for (; count != 0 && i < len; --count) {
            adp_v4_reg_write(adp, addr, le32_to_cpu(buf[i++]), sizeof(uint32_t));
            if (REG_FIELD_EX32(hdr, APIODMA_PACKET, INCREMENT)) { addr += sizeof(uint32_t); }
        }
    }
}

static void adp_v4_tsq_run(AppleDisplayPipeV4State* adp)
{
    ADPV4TSQEntry entry;
    uint32_t      flags;

    WITH_QEMU_LOCK_GUARD(&adp->m3_lock)
    {
        if (adp->m3_outcmdq != 0) {
            for (int i = 0; i < ADP_V4_M3_WPC_SLOTS; i++) {
                *adp_v4_m3_dmem(adp, adp->m3_outcmdq - ADP_V4_M3_WPC_PENDING_OFF + i * sizeof(uint32_t)) = 0;
            }
        }

        if (adp->tsq_count == 0) { return; }

        entry = adp->tsq[adp->tsq_head];
        if (entry.time > adp_v4_timebase_now(adp)) { return; }

        adp->tsq_head   = (adp->tsq_head + 1) % ADP_V4_TSQ_DEPTH;
        adp->tsq_count -= 1;
    }

    adp_v4_apiodma_replay(adp, &entry);

    flags = REG_FIELD_DP32(0, APT_M3_ACTIVEQ, TAG, REG_FIELD_EX32(entry.config, TSQ_ENTRY_CONFIG, TAG));

    QEMU_LOCK_GUARD(&adp->m3_lock);
    if (REG_FIELD_EX32(entry.config, TSQ_ENTRY_CONFIG, REPLAY)) {
        flags             = REG_FIELD_DP32(flags, APT_M3_ACTIVEQ, REPLAY_ACTIVE, 1);
        flags             = REG_FIELD_DP32(flags, APT_M3_ACTIVEQ, APIODMA_DONE, 1);
        adp->m3_replaying = true;
    }
    else if (adp->m3_replaying) {
        flags             = REG_FIELD_DP32(flags, APT_M3_ACTIVEQ, REPLAY_EXITED, 1);
        adp->m3_replaying = false;
    }
    if (qatomic_read(&adp->commit_pending)) {
        adp->m3_activeq_deferred       = true;
        adp->m3_activeq_deferred_flags = flags;
        return;
    }
    adp_v4_m3_post_activeq(adp, adp_v4_timebase_now(adp), flags);
}

static void adp_v4_vsync_tick(void* opaque)
{
    AppleDisplayPipeV4State* adp    = opaque;
    uint64_t                 now    = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t                 period = NANOSECONDS_PER_SECOND / ADP_V4_PANEL_HZ;

    if (adp->direct_scanout || qatomic_xchg(&adp->present_pending, 0)) { dpy_gfx_update_full(adp->console); }

    if (!REG_FIELD_EX32(qatomic_read(&adp->vftg_control), VFTG_CONTROL, ENABLE)) { return; }

    qatomic_or(&adp->int_status, R_CONTROL_INT_SWAP_COMPLETE_MASK);
    adp_v4_update_irqs(adp);

    adp_v4_tsq_run(adp);

    adp->next_vsync_ns += period;
    if (adp->next_vsync_ns <= now) { adp->next_vsync_ns = now + period; }

    timer_mod_ns(adp->vsync_timer, adp->next_vsync_ns);
}

static void adp_v4_realize(DeviceState* dev, Error** errp)
{
    AppleDisplayPipeV4State* adp = APPLE_DISPLAY_PIPE_V4(dev);

    address_space_init(&adp->dma_as, MEMORY_REGION(object_property_get_link(OBJECT(dev), "dma-mr", errp)), "disp0.dma");
    address_space_init(&adp->piodma_as, MEMORY_REGION(object_property_get_link(OBJECT(dev), "piodma-dma-mr", errp)),
                       "disp0.piodma");
    adp->console     = graphic_console_init(dev, 0, &adp_v4_ops, adp);
    adp->vsync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, adp_v4_vsync_tick, adp);
}

static const Property adp_v4_props[] = {
    DEFINE_PROP_UINT32("width", AppleDisplayPipeV4State, width, 0),
    DEFINE_PROP_UINT32("height", AppleDisplayPipeV4State, height, 0),
};

static void adp_v4_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.hold = adp_v4_reset_hold;

    dc->desc = "Apple Display Pipe V4";
    device_class_set_props(dc, adp_v4_props);
    dc->realize = adp_v4_realize;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(AppleDisplayPipeV4State, adp_v4, APPLE_DISPLAY_PIPE_V4, SYS_BUS_DEVICE)

void adp_v4_set_boot_fb(AppleDisplayPipeV4State* adp, uint32_t fb_iova)
{
    adp->boot_fb_valid = true;
    adp->boot_fb_iova  = fb_iova;
    adp_v4_boot_display(adp);
}

void adp_v4_clear_boot_fb(AppleDisplayPipeV4State* adp) { adp->boot_fb_valid = false; }
