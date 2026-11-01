/*
 * libyuv-based image buffers, and surfaces in guest memory.
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
#pragma once

#include "qemu/osdep.h"
#include "system/dma.h"

typedef enum InfernoDMASurfaceFormat
{
    INFERNO_DMA_SURFACE_FORMAT_NONE,
    INFERNO_DMA_SURFACE_FORMAT_ARGB, /* B, G, R, A bytes */
    INFERNO_DMA_SURFACE_FORMAT_ABGR, /* R, G, B, A bytes */
    INFERNO_DMA_SURFACE_FORMAT_RGB565,
    INFERNO_DMA_SURFACE_FORMAT_AR30,
    INFERNO_DMA_SURFACE_FORMAT_AR64,      /* B, G, R, A, 16 bits each, MSB-aligned */
    INFERNO_DMA_SURFACE_FORMAT_AR64_HALF, /* B, G, R, A, half floats */
    INFERNO_DMA_SURFACE_FORMAT_YUY2,
    INFERNO_DMA_SURFACE_FORMAT_NV12,
    INFERNO_DMA_SURFACE_FORMAT_NV16,
    INFERNO_DMA_SURFACE_FORMAT_NV24,
    INFERNO_DMA_SURFACE_FORMAT_P010, /* NV12 with 16-bit MSB-aligned samples */
    INFERNO_DMA_SURFACE_FORMAT_P210,
    INFERNO_DMA_SURFACE_FORMAT_P410,
    INFERNO_DMA_SURFACE_FORMAT_I400, /* luma, depth bits LSB-aligned in 8 or 16 */
} InfernoDMASurfaceFormat;

typedef enum InfernoPixbufMatrix
{
    INFERNO_PIXBUF_MATRIX_BT601,
    INFERNO_PIXBUF_MATRIX_BT709,
} InfernoPixbufMatrix;

/* Plane 0 is luma or the packed pixels, plane 1 interleaved chroma. */
typedef struct InfernoDMASurface
{
    InfernoDMASurfaceFormat format;
    uint32_t                depth;
    uint32_t                width;
    uint32_t                height;
    dma_addr_t              base[2];
    uint32_t                stride[2];
} InfernoDMASurface;

typedef enum InfernoPixbufKind
{
    INFERNO_PIXBUF_ARGB,
    INFERNO_PIXBUF_I444,
    INFERNO_PIXBUF_I422,
    INFERNO_PIXBUF_I420,
    INFERNO_PIXBUF_I400,
} InfernoPixbufKind;

typedef struct InfernoPixbuf
{
    uint8_t*          plane[3];
    int               stride[3];
    uint32_t          width;
    uint32_t          height;
    InfernoPixbufKind kind;
} InfernoPixbuf;

uint32_t inferno_dma_surface_bytes_per_pixel(InfernoDMASurfaceFormat format, uint32_t depth, unsigned plane);
bool     inferno_dma_surface_is_biplanar(InfernoDMASurfaceFormat format);
bool     inferno_dma_surface_load(InfernoPixbuf* image, AddressSpace* as, const InfernoDMASurface* surface);
bool     inferno_dma_surface_store(const InfernoPixbuf* image, AddressSpace* as, const InfernoDMASurface* surface);
void     inferno_dma_surface_copy(AddressSpace* as, const InfernoDMASurface* src, const InfernoDMASurface* dst);

InfernoPixbufKind inferno_dma_surface_pixbuf_kind(InfernoDMASurfaceFormat format);
void              inferno_pixbuf_init(InfernoPixbuf* image, InfernoPixbufKind kind, uint32_t width, uint32_t height);
void              inferno_pixbuf_destroy(InfernoPixbuf* image);
void              inferno_pixbuf_rotate(InfernoPixbuf* dst, const InfernoPixbuf* src, int rotation);
void              inferno_pixbuf_scale(InfernoPixbuf* dst, const InfernoPixbuf* src, uint32_t width, uint32_t height);
void              inferno_pixbuf_mirror(InfernoPixbuf* dst, const InfernoPixbuf* src, bool flip_x);
int               inferno_pixbuf_convert(InfernoPixbuf* dst, const InfernoPixbuf* src, InfernoPixbufKind kind,
                                         InfernoPixbufMatrix matrix);
void              inferno_pixbuf_shuffle(InfernoPixbuf* image, const uint8_t order[4]);
void              inferno_pixbuf_premultiply(InfernoPixbuf* image);
void              inferno_pixbuf_fill(InfernoPixbuf* image, int x, int y, int width, int height, uint32_t argb);
void              inferno_pixbuf_move(InfernoPixbuf* image, int xs, int ys, int xd, int yd, int width, int height);
void              inferno_pixbuf_composite(InfernoPixbuf* dst, const InfernoPixbuf* src, int x, int y, bool blend);
void inferno_pixbuf_draw_glyph(InfernoPixbuf* image, int x, int y, const uint8_t* glyph, int height, uint32_t fg,
                               uint32_t bg);
void inferno_pixbuf_row_to_rgb(const InfernoPixbuf* image, int x, int y, int width, uint8_t* rgb);
