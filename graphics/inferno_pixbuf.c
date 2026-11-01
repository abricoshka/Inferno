/*
 * libyuv-based handling of surfaces in guest memory.
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
#include "graphics/inferno_pixbuf.h"
#include <libyuv.h>
#include <math.h>

#define PLANAR3(i) (i)->plane[0], (i)->stride[0], (i)->plane[1], (i)->stride[1], (i)->plane[2], (i)->stride[2]

uint32_t inferno_dma_surface_bytes_per_pixel(InfernoDMASurfaceFormat format, uint32_t depth, unsigned plane)
{
    switch (format) {
        case INFERNO_DMA_SURFACE_FORMAT_ARGB     :
        case INFERNO_DMA_SURFACE_FORMAT_ABGR     :
        case INFERNO_DMA_SURFACE_FORMAT_AR30     : return 4;
        case INFERNO_DMA_SURFACE_FORMAT_AR64     :
        case INFERNO_DMA_SURFACE_FORMAT_AR64_HALF: return 8;
        case INFERNO_DMA_SURFACE_FORMAT_RGB565   :
        case INFERNO_DMA_SURFACE_FORMAT_YUY2     : return 2;
        case INFERNO_DMA_SURFACE_FORMAT_NV12     :
        case INFERNO_DMA_SURFACE_FORMAT_NV16     :
        case INFERNO_DMA_SURFACE_FORMAT_NV24     : return plane == 0 ? 1 : 2;
        case INFERNO_DMA_SURFACE_FORMAT_P010     :
        case INFERNO_DMA_SURFACE_FORMAT_P210     :
        case INFERNO_DMA_SURFACE_FORMAT_P410     : return plane == 0 ? 2 : 4;
        case INFERNO_DMA_SURFACE_FORMAT_I400     : return depth > 8 ? 2 : 1;
        default                                  : return 0;
    }
}

bool inferno_dma_surface_is_biplanar(InfernoDMASurfaceFormat format)
{ return format >= INFERNO_DMA_SURFACE_FORMAT_NV12 && format <= INFERNO_DMA_SURFACE_FORMAT_P410; }

static bool inferno_dma_surface_is_16bit_biplanar(InfernoDMASurfaceFormat format)
{ return format >= INFERNO_DMA_SURFACE_FORMAT_P010 && format <= INFERNO_DMA_SURFACE_FORMAT_P410; }

InfernoPixbufKind inferno_dma_surface_pixbuf_kind(InfernoDMASurfaceFormat format)
{
    switch (format) {
        case INFERNO_DMA_SURFACE_FORMAT_NV24:
        case INFERNO_DMA_SURFACE_FORMAT_P410: return INFERNO_PIXBUF_I444;
        case INFERNO_DMA_SURFACE_FORMAT_NV16:
        case INFERNO_DMA_SURFACE_FORMAT_P210:
        case INFERNO_DMA_SURFACE_FORMAT_YUY2: return INFERNO_PIXBUF_I422;
        case INFERNO_DMA_SURFACE_FORMAT_NV12:
        case INFERNO_DMA_SURFACE_FORMAT_P010: return INFERNO_PIXBUF_I420;
        case INFERNO_DMA_SURFACE_FORMAT_I400: return INFERNO_PIXBUF_I400;
        default                             : return INFERNO_PIXBUF_ARGB;
    }
}

static bool inferno_pixbuf_kind_has_chroma(InfernoPixbufKind kind)
{ return kind == INFERNO_PIXBUF_I444 || kind == INFERNO_PIXBUF_I422 || kind == INFERNO_PIXBUF_I420; }

static void inferno_pixbuf_kind_chroma_dims(InfernoPixbufKind kind, uint32_t width, uint32_t height, uint32_t* cw,
                                            uint32_t* ch)
{
    *cw = kind == INFERNO_PIXBUF_I444 ? width : (width + 1) / 2;
    *ch = kind == INFERNO_PIXBUF_I420 ? (height + 1) / 2 : height;
}

void inferno_pixbuf_init(InfernoPixbuf* image, InfernoPixbufKind kind, uint32_t width, uint32_t height)
{
    uint32_t cw, ch;
    size_t   luma_size;
    size_t   chroma_size;

    *image        = (InfernoPixbuf){0};
    image->kind   = kind;
    image->width  = width;
    image->height = height;

    if (kind == INFERNO_PIXBUF_ARGB) {
        image->stride[0] = (int)width * 4;
        image->plane[0]  = g_malloc((size_t)image->stride[0] * height);
        return;
    }

    image->stride[0] = (int)width;
    luma_size        = (size_t)width * height;

    if (!inferno_pixbuf_kind_has_chroma(kind)) {
        image->plane[0] = g_malloc(luma_size);
        return;
    }

    inferno_pixbuf_kind_chroma_dims(kind, width, height, &cw, &ch);
    image->stride[1] = image->stride[2] = (int)cw;
    chroma_size                         = (size_t)cw * ch;
    image->plane[0]                     = g_malloc(luma_size + 2 * chroma_size);
    image->plane[1]                     = image->plane[0] + luma_size;
    image->plane[2]                     = image->plane[1] + chroma_size;
}

void inferno_pixbuf_destroy(InfernoPixbuf* image) { g_free(image->plane[0]); }

void inferno_pixbuf_rotate(InfernoPixbuf* dst, const InfernoPixbuf* src, int rotation)
{
    bool swap = rotation == kRotate90 || rotation == kRotate270;

    inferno_pixbuf_init(dst, src->kind, swap ? src->height : src->width, swap ? src->width : src->height);

    switch (src->kind) {
        case INFERNO_PIXBUF_ARGB:
            ARGBRotate(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height,
                       rotation);
            return;
        case INFERNO_PIXBUF_I400:
            RotatePlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height,
                        rotation);
            return;
        case INFERNO_PIXBUF_I444:
            I444Rotate(PLANAR3(src), PLANAR3(dst), (int)src->width, (int)src->height, rotation);
            return;
        case INFERNO_PIXBUF_I422:
            I422Rotate(PLANAR3(src), PLANAR3(dst), (int)src->width, (int)src->height, rotation);
            return;
        case INFERNO_PIXBUF_I420:
            I420Rotate(PLANAR3(src), PLANAR3(dst), (int)src->width, (int)src->height, rotation);
            return;
    }
}

void inferno_pixbuf_scale(InfernoPixbuf* dst, const InfernoPixbuf* src, uint32_t width, uint32_t height)
{
    inferno_pixbuf_init(dst, src->kind, width, height);

    switch (src->kind) {
        case INFERNO_PIXBUF_ARGB:
            ARGBScale(src->plane[0], src->stride[0], (int)src->width, (int)src->height, dst->plane[0], dst->stride[0],
                      (int)width, (int)height, kFilterBox);
            return;
        case INFERNO_PIXBUF_I400:
            ScalePlane(src->plane[0], src->stride[0], (int)src->width, (int)src->height, dst->plane[0], dst->stride[0],
                       (int)width, (int)height, kFilterBox);
            return;
        case INFERNO_PIXBUF_I444:
            I444Scale(PLANAR3(src), (int)src->width, (int)src->height, PLANAR3(dst), (int)width, (int)height,
                      kFilterBox);
            return;
        case INFERNO_PIXBUF_I422:
            I422Scale(PLANAR3(src), (int)src->width, (int)src->height, PLANAR3(dst), (int)width, (int)height,
                      kFilterBox);
            return;
        case INFERNO_PIXBUF_I420:
            I420Scale(PLANAR3(src), (int)src->width, (int)src->height, PLANAR3(dst), (int)width, (int)height,
                      kFilterBox);
            return;
    }
}

void inferno_pixbuf_mirror(InfernoPixbuf* dst, const InfernoPixbuf* src, bool flip_x)
{
    uint32_t cw, ch;
    int      i;

    inferno_pixbuf_init(dst, src->kind, src->width, src->height);

    if (src->kind == INFERNO_PIXBUF_ARGB) {
        if (flip_x) {
            ARGBMirror(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height);
        }
        else {
            ARGBCopy(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, -(int)src->height);
        }
        return;
    }

    inferno_pixbuf_kind_chroma_dims(src->kind, src->width, src->height, &cw, &ch);

    for (i = 0; i < (inferno_pixbuf_kind_has_chroma(src->kind) ? 3 : 1); i++) {
        int w = i == 0 ? (int)src->width : (int)cw;
        int h = i == 0 ? (int)src->height : (int)ch;

        if (flip_x) { MirrorPlane(src->plane[i], src->stride[i], dst->plane[i], dst->stride[i], w, h); }
        else {
            CopyPlane(src->plane[i], src->stride[i], dst->plane[i], dst->stride[i], w, -h);
        }
    }
}

static void inferno_pixbuf_from_i400(InfernoPixbuf* dst, const InfernoPixbuf* src)
{
    uint32_t cw, ch;

    CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height);

    if (!inferno_pixbuf_kind_has_chroma(dst->kind)) { return; }

    inferno_pixbuf_kind_chroma_dims(dst->kind, dst->width, dst->height, &cw, &ch);
    SetPlane(dst->plane[1], dst->stride[1], (int)cw, (int)ch, 0x80);
    SetPlane(dst->plane[2], dst->stride[2], (int)cw, (int)ch, 0x80);
}

static const struct YuvConstants* inferno_pixbuf_yuv_constants(InfernoPixbufMatrix matrix)
{
    switch (matrix) {
        case INFERNO_PIXBUF_MATRIX_BT709: return &kYuvH709Constants;
        default                         : return &kYuvI601Constants;
    }
}

int inferno_pixbuf_convert(InfernoPixbuf* dst, const InfernoPixbuf* src, InfernoPixbufKind kind,
                           InfernoPixbufMatrix matrix)
{
    int w = (int)src->width;
    int h = (int)src->height;

    inferno_pixbuf_init(dst, kind, src->width, src->height);

    if (src->kind == INFERNO_PIXBUF_I400) {
        if (kind == INFERNO_PIXBUF_ARGB) {
            return I400ToARGB(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        }
        inferno_pixbuf_from_i400(dst, src);
        return 0;
    }

    if (kind == INFERNO_PIXBUF_I400) {
        if (src->kind == INFERNO_PIXBUF_ARGB) {
            return ARGBToI400(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        }
        CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        return 0;
    }

    if (src->kind == INFERNO_PIXBUF_ARGB) {
        switch (kind) {
            case INFERNO_PIXBUF_I444: return ARGBToI444(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
            case INFERNO_PIXBUF_I422: return ARGBToI422(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
            default                 : return ARGBToI420(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
        }
    }

    if (kind == INFERNO_PIXBUF_ARGB) {
        const struct YuvConstants* constants = inferno_pixbuf_yuv_constants(matrix);

        switch (src->kind) {
            case INFERNO_PIXBUF_I444:
                return I444ToARGBMatrix(PLANAR3(src), dst->plane[0], dst->stride[0], constants, w, h);
            case INFERNO_PIXBUF_I422:
                return I422ToARGBMatrix(PLANAR3(src), dst->plane[0], dst->stride[0], constants, w, h);
            default: return I420ToARGBMatrix(PLANAR3(src), dst->plane[0], dst->stride[0], constants, w, h);
        }
    }

    switch (src->kind) {
        case INFERNO_PIXBUF_I444:
            if (kind == INFERNO_PIXBUF_I420) { return I444ToI420(PLANAR3(src), PLANAR3(dst), w, h); }
            /* No I444ToI422 in libyuv: halve the chroma planes horizontally, full height. */
            CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
            ScalePlane(src->plane[1], src->stride[1], w, h, dst->plane[1], dst->stride[1], dst->stride[1], h,
                       kFilterBilinear);
            return ScalePlane(src->plane[2], src->stride[2], w, h, dst->plane[2], dst->stride[2], dst->stride[2], h,
                              kFilterBilinear);
        case INFERNO_PIXBUF_I422:
            return kind == INFERNO_PIXBUF_I444 ? I422ToI444(PLANAR3(src), PLANAR3(dst), w, h) :
                                                 I422ToI420(PLANAR3(src), PLANAR3(dst), w, h);
        default:
            return kind == INFERNO_PIXBUF_I444 ? I420ToI444(PLANAR3(src), PLANAR3(dst), w, h) :
                                                 I420ToI422(PLANAR3(src), PLANAR3(dst), w, h);
    }
}

void inferno_pixbuf_shuffle(InfernoPixbuf* image, const uint8_t order[4])
{
    uint8_t shuffler[16];
    int     i;

    for (i = 0; i < 16; i++) { shuffler[i] = (i & ~3) + (order[i & 3] & 3); }

    ARGBShuffle(image->plane[0], image->stride[0], image->plane[0], image->stride[0], shuffler, (int)image->width,
                (int)image->height);
}

void inferno_pixbuf_premultiply(InfernoPixbuf* image)
{
    ARGBAttenuate(image->plane[0], image->stride[0], image->plane[0], image->stride[0], (int)image->width,
                  (int)image->height);
}

void inferno_pixbuf_fill(InfernoPixbuf* image, int x, int y, int width, int height, uint32_t argb)
{
    int x0 = MAX(x, 0);
    int y0 = MAX(y, 0);
    int x1 = MIN(x + width, (int)image->width);
    int y1 = MIN(y + height, (int)image->height);

    if (x1 <= x0 || y1 <= y0) { return; }

    ARGBRect(image->plane[0], image->stride[0], x0, y0, x1 - x0, y1 - y0, argb);
}

void inferno_pixbuf_move(InfernoPixbuf* image, int xs, int ys, int xd, int yd, int width, int height)
{
    ptrdiff_t step = image->stride[0];
    uint8_t*  src  = image->plane[0] + (ptrdiff_t)ys * step + xs * 4;
    uint8_t*  dst  = image->plane[0] + (ptrdiff_t)yd * step + xd * 4;

    if (yd > ys) {
        src  += (height - 1) * step;
        dst  += (height - 1) * step;
        step  = -step;
    }

    for (; height > 0; height--, src += step, dst += step) { memmove(dst, src, (size_t)width * 4); }
}

void inferno_pixbuf_composite(InfernoPixbuf* dst, const InfernoPixbuf* src, int x, int y, bool blend)
{
    int            sx = MAX(-x, 0);
    int            sy = MAX(-y, 0);
    int            dx = MAX(x, 0);
    int            dy = MAX(y, 0);
    int            w  = MIN((int)src->width - sx, (int)dst->width - dx);
    int            h  = MIN((int)src->height - sy, (int)dst->height - dy);
    const uint8_t* in;
    uint8_t*       out;

    if (w <= 0 || h <= 0) { return; }

    in  = src->plane[0] + (size_t)sy * src->stride[0] + sx * 4;
    out = dst->plane[0] + (size_t)dy * dst->stride[0] + dx * 4;

    if (blend) { ARGBBlend(in, src->stride[0], out, dst->stride[0], out, dst->stride[0], w, h); }
    else {
        ARGBCopy(in, src->stride[0], out, dst->stride[0], w, h);
    }
}

void inferno_pixbuf_draw_glyph(InfernoPixbuf* image, int x, int y, const uint8_t* glyph, int height, uint32_t fg,
                               uint32_t bg)
{
    if (x < 0 || y < 0 || x + 8 > (int)image->width || y + height > (int)image->height) { return; }

    for (int row = 0; row < height; row++) {
        uint32_t* out = (uint32_t*)(image->plane[0] + (size_t)(y + row) * image->stride[0]) + x;

        for (int col = 0; col < 8; col++) { out[col] = glyph[row] & (0x80 >> col) ? fg : bg; }
    }
}

void inferno_pixbuf_row_to_rgb(const InfernoPixbuf* image, int x, int y, int width, uint8_t* rgb)
{ ARGBToRAW(image->plane[0] + (size_t)y * image->stride[0] + x * 4, image->stride[0], rgb, width * 3, width, 1); }

static void inferno_dma_surface_rows(AddressSpace* as, dma_addr_t base, uint32_t stride, uint32_t row_bytes,
                                     uint32_t height, void* buf, bool write)
{
    uint8_t* rows = buf;

    if (row_bytes == 0 || height == 0) { return; }

    if (stride == row_bytes) {
        size_t total = (size_t)row_bytes * height;
        if (write) { dma_memory_write(as, base, rows, total, MEMTXATTRS_UNSPECIFIED); }
        else {
            dma_memory_read(as, base, rows, total, MEMTXATTRS_UNSPECIFIED);
        }
        return;
    }

    for (uint32_t y = 0; y < height; y++) {
        dma_addr_t addr = base + (dma_addr_t)y * stride;
        uint8_t*   row  = rows + (size_t)y * row_bytes;
        if (write) { dma_memory_write(as, addr, row, row_bytes, MEMTXATTRS_UNSPECIFIED); }
        else {
            dma_memory_read(as, addr, row, row_bytes, MEMTXATTRS_UNSPECIFIED);
        }
    }
}

static uint8_t inferno_dma_surface_half_to_unorm8(uint16_t half)
{
    uint32_t exp  = (half >> 10) & 0x1F;
    uint32_t mant = half & 0x3FF;
    float    value;

    if (half & 0x8000) { return 0; }
    if (exp == 0x1F) { return mant == 0 ? 0xFF : 0; }

    value = exp == 0 ? ldexpf((float)mant, -24) : ldexpf((float)(mant | 0x400), (int)exp - 25);
    return value >= 1.0f ? 0xFF : (uint8_t)lrintf(value * 255.0f);
}

static void inferno_dma_surface_half_to_argb(const uint16_t* src, int src_stride, const InfernoPixbuf* dst)
{
    for (uint32_t y = 0; y < dst->height; y++) {
        const uint16_t* in  = src + (size_t)y * src_stride;
        uint8_t*        out = dst->plane[0] + (size_t)y * dst->stride[0];

        for (uint32_t x = 0; x < dst->width * 4; x++) { out[x] = inferno_dma_surface_half_to_unorm8(in[x]); }
    }
}

static int inferno_dma_surface_packed_to_pixbuf(InfernoDMASurfaceFormat format, const uint8_t* src, int stride,
                                                const InfernoPixbuf* dst)
{
    int w = (int)dst->width;
    int h = (int)dst->height;

    switch (format) {
        case INFERNO_DMA_SURFACE_FORMAT_ARGB  : return ARGBCopy(src, stride, dst->plane[0], dst->stride[0], w, h);
        case INFERNO_DMA_SURFACE_FORMAT_ABGR  : return ABGRToARGB(src, stride, dst->plane[0], dst->stride[0], w, h);
        case INFERNO_DMA_SURFACE_FORMAT_RGB565: return RGB565ToARGB(src, stride, dst->plane[0], dst->stride[0], w, h);
        case INFERNO_DMA_SURFACE_FORMAT_AR30  : return AR30ToARGB(src, stride, dst->plane[0], dst->stride[0], w, h);
        case INFERNO_DMA_SURFACE_FORMAT_AR64:
            return AR64ToARGB((const uint16_t*)src, stride / 2, dst->plane[0], dst->stride[0], w, h);
        case INFERNO_DMA_SURFACE_FORMAT_AR64_HALF:
            inferno_dma_surface_half_to_argb((const uint16_t*)src, stride / 2, dst);
            return 0;
        case INFERNO_DMA_SURFACE_FORMAT_YUY2: return YUY2ToI422(src, stride, PLANAR3(dst), w, h);
        default                             : return -1;
    }
}

static int inferno_dma_surface_packed_from_pixbuf(InfernoDMASurfaceFormat format, const InfernoPixbuf* src,
                                                  uint8_t* dst, int stride)
{
    int w = (int)src->width;
    int h = (int)src->height;

    switch (format) {
        case INFERNO_DMA_SURFACE_FORMAT_ARGB: return ARGBCopy(src->plane[0], src->stride[0], dst, stride, w, h);
        case INFERNO_DMA_SURFACE_FORMAT_ABGR: return ARGBToABGR(src->plane[0], src->stride[0], dst, stride, w, h);
        case INFERNO_DMA_SURFACE_FORMAT_AR30: return ARGBToAR30(src->plane[0], src->stride[0], dst, stride, w, h);
        case INFERNO_DMA_SURFACE_FORMAT_YUY2: return I422ToYUY2(PLANAR3(src), dst, stride, w, h);
        default                             : return -1;
    }
}

static void inferno_dma_surface_load_chroma(const InfernoPixbuf* image, AddressSpace* as,
                                            const InfernoDMASurface* surface)
{
    uint32_t cw, ch;
    uint8_t* uv;

    inferno_pixbuf_kind_chroma_dims(image->kind, image->width, image->height, &cw, &ch);
    uv = g_malloc((size_t)cw * 2 * ch);

    if (inferno_dma_surface_is_16bit_biplanar(surface->format)) {
        uint16_t* uv16 = g_malloc((size_t)cw * 4 * ch);

        inferno_dma_surface_rows(as, surface->base[1], surface->stride[1], cw * 4, ch, uv16, false);
        Convert16To8Plane(uv16, (int)cw * 2, uv, (int)cw * 2, 256, (int)cw * 2, (int)ch);
        g_free(uv16);
    }
    else {
        inferno_dma_surface_rows(as, surface->base[1], surface->stride[1], cw * 2, ch, uv, false);
    }

    SplitUVPlane(uv, (int)cw * 2, image->plane[1], image->stride[1], image->plane[2], image->stride[2], (int)cw,
                 (int)ch);
    g_free(uv);
}

static void inferno_dma_surface_load_luma16(const InfernoPixbuf* image, AddressSpace* as,
                                            const InfernoDMASurface* surface, int scale)
{
    uint16_t* luma = g_malloc((size_t)image->width * 2 * image->height);

    inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], image->width * 2, image->height, luma, false);
    Convert16To8Plane(luma, (int)image->width, image->plane[0], image->stride[0], scale, (int)image->width,
                      (int)image->height);
    g_free(luma);
}

bool inferno_dma_surface_load(InfernoPixbuf* image, AddressSpace* as, const InfernoDMASurface* surface)
{
    uint32_t row_bytes = surface->width * inferno_dma_surface_bytes_per_pixel(surface->format, surface->depth, 0);
    uint8_t* staging;
    int      ret;

    if (row_bytes == 0 || surface->height == 0) { return false; }

    inferno_pixbuf_init(image, inferno_dma_surface_pixbuf_kind(surface->format), surface->width, surface->height);

    if (inferno_dma_surface_is_16bit_biplanar(surface->format)) {
        inferno_dma_surface_load_luma16(image, as, surface, 256);
        inferno_dma_surface_load_chroma(image, as, surface);
        return true;
    }

    if (inferno_dma_surface_is_biplanar(surface->format)) {
        inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, surface->height, image->plane[0],
                                 false);
        inferno_dma_surface_load_chroma(image, as, surface);
        return true;
    }

    if (surface->format == INFERNO_DMA_SURFACE_FORMAT_I400) {
        if (surface->depth == 8) {
            inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, surface->height,
                                     image->plane[0], false);
        }
        else {
            inferno_dma_surface_load_luma16(image, as, surface, 65536 >> (surface->depth - 8));
        }
        return true;
    }

    if (surface->format == INFERNO_DMA_SURFACE_FORMAT_ARGB) {
        inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, surface->height, image->plane[0],
                                 false);
        return true;
    }

    staging = g_malloc((size_t)row_bytes * surface->height);
    inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, surface->height, staging, false);
    ret = inferno_dma_surface_packed_to_pixbuf(surface->format, staging, (int)row_bytes, image);
    g_free(staging);

    if (ret) {
        inferno_pixbuf_destroy(image);
        return false;
    }
    return true;
}

bool inferno_dma_surface_store(const InfernoPixbuf* image, AddressSpace* as, const InfernoDMASurface* surface)
{
    uint32_t row_bytes = image->width * inferno_dma_surface_bytes_per_pixel(surface->format, surface->depth, 0);
    uint8_t* staging;
    int      ret;

    if (inferno_dma_surface_is_biplanar(surface->format)) {
        bool     wide = inferno_dma_surface_is_16bit_biplanar(surface->format);
        uint32_t cw, ch;
        uint8_t* uv;

        if (wide) {
            staging = g_malloc((size_t)row_bytes * image->height);
            Convert8To16Plane(image->plane[0], image->stride[0], (uint16_t*)staging, (int)image->width, 16,
                              (int)image->width, (int)image->height);
            inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, staging, true);
            g_free(staging);
        }
        else {
            inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height,
                                     image->plane[0], true);
        }

        inferno_pixbuf_kind_chroma_dims(image->kind, image->width, image->height, &cw, &ch);
        uv = g_malloc((size_t)cw * 2 * ch);
        MergeUVPlane(image->plane[1], image->stride[1], image->plane[2], image->stride[2], uv, (int)cw * 2, (int)cw,
                     (int)ch);

        if (wide) {
            uint16_t* uv16 = g_malloc((size_t)cw * 4 * ch);

            Convert8To16Plane(uv, (int)cw * 2, uv16, (int)cw * 2, 16, (int)cw * 2, (int)ch);
            inferno_dma_surface_rows(as, surface->base[1], surface->stride[1], cw * 4, ch, uv16, true);
            g_free(uv16);
        }
        else {
            inferno_dma_surface_rows(as, surface->base[1], surface->stride[1], cw * 2, ch, uv, true);
        }
        g_free(uv);
        return true;
    }

    if (surface->format == INFERNO_DMA_SURFACE_FORMAT_I400) {
        if (surface->depth == 8) {
            inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height,
                                     image->plane[0], true);
            return true;
        }

        staging = g_malloc((size_t)row_bytes * image->height);
        Convert8To16Plane(image->plane[0], image->stride[0], (uint16_t*)staging, (int)(row_bytes / 2),
                          (int)surface->depth, (int)image->width, (int)image->height);
        inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, staging, true);
        g_free(staging);
        return true;
    }

    if (surface->format == INFERNO_DMA_SURFACE_FORMAT_ARGB) {
        inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, image->plane[0],
                                 true);
        return true;
    }

    staging = g_malloc((size_t)row_bytes * image->height);
    ret     = inferno_dma_surface_packed_from_pixbuf(surface->format, image, staging, (int)row_bytes);
    if (ret == 0) {
        inferno_dma_surface_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, staging, true);
    }
    g_free(staging);
    return ret == 0;
}

void inferno_dma_surface_copy(AddressSpace* as, const InfernoDMASurface* src, const InfernoDMASurface* dst)
{
    uint32_t row_bytes = src->width * inferno_dma_surface_bytes_per_pixel(src->format, src->depth, 0);
    uint8_t* buf;

    buf = g_malloc((size_t)row_bytes * src->height);
    inferno_dma_surface_rows(as, src->base[0], src->stride[0], row_bytes, src->height, buf, false);
    inferno_dma_surface_rows(as, dst->base[0], dst->stride[0], row_bytes, dst->height, buf, true);
    g_free(buf);

    if (inferno_dma_surface_is_biplanar(src->format)) {
        uint32_t cw, ch;

        inferno_pixbuf_kind_chroma_dims(inferno_dma_surface_pixbuf_kind(src->format), src->width, src->height, &cw,
                                        &ch);
        row_bytes = cw * inferno_dma_surface_bytes_per_pixel(src->format, src->depth, 1);
        buf       = g_malloc((size_t)row_bytes * ch);
        inferno_dma_surface_rows(as, src->base[1], src->stride[1], row_bytes, ch, buf, false);
        inferno_dma_surface_rows(as, dst->base[1], dst->stride[1], row_bytes, ch, buf, true);
        g_free(buf);
    }
}
