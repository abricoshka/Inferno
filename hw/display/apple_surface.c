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
#include "hw/display/apple_surface.h"
#include <libyuv.h>
#include <math.h>

#define PLANAR3(i) (i)->plane[0], (i)->stride[0], (i)->plane[1], (i)->stride[1], (i)->plane[2], (i)->stride[2]

uint32_t apple_surface_bytes_per_pixel(AppleSurfaceFormat format, uint32_t depth, unsigned plane)
{
    switch (format) {
        case APPLE_SURFACE_FORMAT_ARGB     :
        case APPLE_SURFACE_FORMAT_ABGR     :
        case APPLE_SURFACE_FORMAT_AR30     : return 4;
        case APPLE_SURFACE_FORMAT_AR64     :
        case APPLE_SURFACE_FORMAT_AR64_HALF: return 8;
        case APPLE_SURFACE_FORMAT_RGB565   :
        case APPLE_SURFACE_FORMAT_YUY2     : return 2;
        case APPLE_SURFACE_FORMAT_NV12     :
        case APPLE_SURFACE_FORMAT_NV16     :
        case APPLE_SURFACE_FORMAT_NV24     : return plane == 0 ? 1 : 2;
        case APPLE_SURFACE_FORMAT_P010     :
        case APPLE_SURFACE_FORMAT_P210     :
        case APPLE_SURFACE_FORMAT_P410     : return plane == 0 ? 2 : 4;
        case APPLE_SURFACE_FORMAT_I400     : return depth > 8 ? 2 : 1;
        default                            : return 0;
    }
}

bool apple_surface_is_biplanar(AppleSurfaceFormat format)
{ return format >= APPLE_SURFACE_FORMAT_NV12 && format <= APPLE_SURFACE_FORMAT_P410; }

static bool apple_surface_is_16bit_biplanar(AppleSurfaceFormat format)
{ return format >= APPLE_SURFACE_FORMAT_P010 && format <= APPLE_SURFACE_FORMAT_P410; }

AppleImageKind apple_surface_image_kind(AppleSurfaceFormat format)
{
    switch (format) {
        case APPLE_SURFACE_FORMAT_NV24:
        case APPLE_SURFACE_FORMAT_P410: return APPLE_IMAGE_I444;
        case APPLE_SURFACE_FORMAT_NV16:
        case APPLE_SURFACE_FORMAT_P210:
        case APPLE_SURFACE_FORMAT_YUY2: return APPLE_IMAGE_I422;
        case APPLE_SURFACE_FORMAT_NV12:
        case APPLE_SURFACE_FORMAT_P010: return APPLE_IMAGE_I420;
        case APPLE_SURFACE_FORMAT_I400: return APPLE_IMAGE_I400;
        default                       : return APPLE_IMAGE_ARGB;
    }
}

static bool apple_image_kind_has_chroma(AppleImageKind kind)
{ return kind == APPLE_IMAGE_I444 || kind == APPLE_IMAGE_I422 || kind == APPLE_IMAGE_I420; }

static void apple_image_kind_chroma_dims(AppleImageKind kind, uint32_t width, uint32_t height, uint32_t* cw,
                                         uint32_t* ch)
{
    *cw = kind == APPLE_IMAGE_I444 ? width : (width + 1) / 2;
    *ch = kind == APPLE_IMAGE_I420 ? (height + 1) / 2 : height;
}

void apple_image_init(AppleImage* image, AppleImageKind kind, uint32_t width, uint32_t height)
{
    uint32_t cw, ch;
    size_t   luma_size;
    size_t   chroma_size;

    *image        = (AppleImage){0};
    image->kind   = kind;
    image->width  = width;
    image->height = height;

    if (kind == APPLE_IMAGE_ARGB) {
        image->stride[0] = (int)width * 4;
        image->plane[0]  = g_malloc((size_t)image->stride[0] * height);
        return;
    }

    image->stride[0] = (int)width;
    luma_size        = (size_t)width * height;

    if (!apple_image_kind_has_chroma(kind)) {
        image->plane[0] = g_malloc(luma_size);
        return;
    }

    apple_image_kind_chroma_dims(kind, width, height, &cw, &ch);
    image->stride[1] = image->stride[2] = (int)cw;
    chroma_size                         = (size_t)cw * ch;
    image->plane[0]                     = g_malloc(luma_size + 2 * chroma_size);
    image->plane[1]                     = image->plane[0] + luma_size;
    image->plane[2]                     = image->plane[1] + chroma_size;
}

void apple_image_destroy(AppleImage* image) { g_free(image->plane[0]); }

void apple_image_rotate(AppleImage* dst, const AppleImage* src, int rotation)
{
    bool swap = rotation == kRotate90 || rotation == kRotate270;

    apple_image_init(dst, src->kind, swap ? src->height : src->width, swap ? src->width : src->height);

    switch (src->kind) {
        case APPLE_IMAGE_ARGB:
            ARGBRotate(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height,
                       rotation);
            return;
        case APPLE_IMAGE_I400:
            RotatePlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height,
                        rotation);
            return;
        case APPLE_IMAGE_I444:
            I444Rotate(PLANAR3(src), PLANAR3(dst), (int)src->width, (int)src->height, rotation);
            return;
        case APPLE_IMAGE_I422:
            I422Rotate(PLANAR3(src), PLANAR3(dst), (int)src->width, (int)src->height, rotation);
            return;
        case APPLE_IMAGE_I420:
            I420Rotate(PLANAR3(src), PLANAR3(dst), (int)src->width, (int)src->height, rotation);
            return;
    }
}

void apple_image_scale(AppleImage* dst, const AppleImage* src, uint32_t width, uint32_t height)
{
    apple_image_init(dst, src->kind, width, height);

    switch (src->kind) {
        case APPLE_IMAGE_ARGB:
            ARGBScale(src->plane[0], src->stride[0], (int)src->width, (int)src->height, dst->plane[0], dst->stride[0],
                      (int)width, (int)height, kFilterBox);
            return;
        case APPLE_IMAGE_I400:
            ScalePlane(src->plane[0], src->stride[0], (int)src->width, (int)src->height, dst->plane[0], dst->stride[0],
                       (int)width, (int)height, kFilterBox);
            return;
        case APPLE_IMAGE_I444:
            I444Scale(PLANAR3(src), (int)src->width, (int)src->height, PLANAR3(dst), (int)width, (int)height,
                      kFilterBox);
            return;
        case APPLE_IMAGE_I422:
            I422Scale(PLANAR3(src), (int)src->width, (int)src->height, PLANAR3(dst), (int)width, (int)height,
                      kFilterBox);
            return;
        case APPLE_IMAGE_I420:
            I420Scale(PLANAR3(src), (int)src->width, (int)src->height, PLANAR3(dst), (int)width, (int)height,
                      kFilterBox);
            return;
    }
}

void apple_image_mirror(AppleImage* dst, const AppleImage* src, bool flip_x)
{
    uint32_t cw, ch;
    int      i;

    apple_image_init(dst, src->kind, src->width, src->height);

    if (src->kind == APPLE_IMAGE_ARGB) {
        if (flip_x) {
            ARGBMirror(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height);
        }
        else {
            ARGBCopy(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, -(int)src->height);
        }
        return;
    }

    apple_image_kind_chroma_dims(src->kind, src->width, src->height, &cw, &ch);

    for (i = 0; i < (apple_image_kind_has_chroma(src->kind) ? 3 : 1); i++) {
        int w = i == 0 ? (int)src->width : (int)cw;
        int h = i == 0 ? (int)src->height : (int)ch;

        if (flip_x) { MirrorPlane(src->plane[i], src->stride[i], dst->plane[i], dst->stride[i], w, h); }
        else {
            CopyPlane(src->plane[i], src->stride[i], dst->plane[i], dst->stride[i], w, -h);
        }
    }
}

static void apple_image_from_i400(AppleImage* dst, const AppleImage* src)
{
    uint32_t cw, ch;

    CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height);

    if (!apple_image_kind_has_chroma(dst->kind)) { return; }

    apple_image_kind_chroma_dims(dst->kind, dst->width, dst->height, &cw, &ch);
    SetPlane(dst->plane[1], dst->stride[1], (int)cw, (int)ch, 0x80);
    SetPlane(dst->plane[2], dst->stride[2], (int)cw, (int)ch, 0x80);
}

static const struct YuvConstants* apple_surface_yuv_constants(AppleSurfaceMatrix matrix)
{
    switch (matrix) {
        case APPLE_SURFACE_MATRIX_BT709: return &kYuvH709Constants;
        default                        : return &kYuvI601Constants;
    }
}

int apple_image_convert(AppleImage* dst, const AppleImage* src, AppleImageKind kind, AppleSurfaceMatrix matrix)
{
    int w = (int)src->width;
    int h = (int)src->height;

    apple_image_init(dst, kind, src->width, src->height);

    if (src->kind == APPLE_IMAGE_I400) {
        if (kind == APPLE_IMAGE_ARGB) {
            return I400ToARGB(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        }
        apple_image_from_i400(dst, src);
        return 0;
    }

    if (kind == APPLE_IMAGE_I400) {
        if (src->kind == APPLE_IMAGE_ARGB) {
            return ARGBToI400(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        }
        CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        return 0;
    }

    if (src->kind == APPLE_IMAGE_ARGB) {
        switch (kind) {
            case APPLE_IMAGE_I444: return ARGBToI444(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
            case APPLE_IMAGE_I422: return ARGBToI422(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
            default              : return ARGBToI420(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
        }
    }

    if (kind == APPLE_IMAGE_ARGB) {
        const struct YuvConstants* constants = apple_surface_yuv_constants(matrix);

        switch (src->kind) {
            case APPLE_IMAGE_I444:
                return I444ToARGBMatrix(PLANAR3(src), dst->plane[0], dst->stride[0], constants, w, h);
            case APPLE_IMAGE_I422:
                return I422ToARGBMatrix(PLANAR3(src), dst->plane[0], dst->stride[0], constants, w, h);
            default: return I420ToARGBMatrix(PLANAR3(src), dst->plane[0], dst->stride[0], constants, w, h);
        }
    }

    switch (src->kind) {
        case APPLE_IMAGE_I444:
            if (kind == APPLE_IMAGE_I420) { return I444ToI420(PLANAR3(src), PLANAR3(dst), w, h); }
            /* No I444ToI422 in libyuv: halve the chroma planes horizontally, full height. */
            CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
            ScalePlane(src->plane[1], src->stride[1], w, h, dst->plane[1], dst->stride[1], dst->stride[1], h,
                       kFilterBilinear);
            return ScalePlane(src->plane[2], src->stride[2], w, h, dst->plane[2], dst->stride[2], dst->stride[2], h,
                              kFilterBilinear);
        case APPLE_IMAGE_I422:
            return kind == APPLE_IMAGE_I444 ? I422ToI444(PLANAR3(src), PLANAR3(dst), w, h) :
                                              I422ToI420(PLANAR3(src), PLANAR3(dst), w, h);
        default:
            return kind == APPLE_IMAGE_I444 ? I420ToI444(PLANAR3(src), PLANAR3(dst), w, h) :
                                              I420ToI422(PLANAR3(src), PLANAR3(dst), w, h);
    }
}

void apple_image_shuffle(AppleImage* image, const uint8_t order[4])
{
    uint8_t shuffler[16];
    int     i;

    for (i = 0; i < 16; i++) { shuffler[i] = (i & ~3) + (order[i & 3] & 3); }

    ARGBShuffle(image->plane[0], image->stride[0], image->plane[0], image->stride[0], shuffler, (int)image->width,
                (int)image->height);
}

void apple_image_premultiply(AppleImage* image)
{
    ARGBAttenuate(image->plane[0], image->stride[0], image->plane[0], image->stride[0], (int)image->width,
                  (int)image->height);
}

static void apple_surface_dma_rows(AddressSpace* as, dma_addr_t base, uint32_t stride, uint32_t row_bytes,
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

static uint8_t apple_surface_half_to_unorm8(uint16_t half)
{
    uint32_t exp  = (half >> 10) & 0x1F;
    uint32_t mant = half & 0x3FF;
    float    value;

    if (half & 0x8000) { return 0; }
    if (exp == 0x1F) { return mant == 0 ? 0xFF : 0; }

    value = exp == 0 ? ldexpf((float)mant, -24) : ldexpf((float)(mant | 0x400), (int)exp - 25);
    return value >= 1.0f ? 0xFF : (uint8_t)lrintf(value * 255.0f);
}

static void apple_surface_half_to_argb(const uint16_t* src, int src_stride, const AppleImage* dst)
{
    for (uint32_t y = 0; y < dst->height; y++) {
        const uint16_t* in  = src + (size_t)y * src_stride;
        uint8_t*        out = dst->plane[0] + (size_t)y * dst->stride[0];

        for (uint32_t x = 0; x < dst->width * 4; x++) { out[x] = apple_surface_half_to_unorm8(in[x]); }
    }
}

static int apple_surface_packed_to_image(AppleSurfaceFormat format, const uint8_t* src, int stride,
                                         const AppleImage* dst)
{
    int w = (int)dst->width;
    int h = (int)dst->height;

    switch (format) {
        case APPLE_SURFACE_FORMAT_ARGB  : return ARGBCopy(src, stride, dst->plane[0], dst->stride[0], w, h);
        case APPLE_SURFACE_FORMAT_ABGR  : return ABGRToARGB(src, stride, dst->plane[0], dst->stride[0], w, h);
        case APPLE_SURFACE_FORMAT_RGB565: return RGB565ToARGB(src, stride, dst->plane[0], dst->stride[0], w, h);
        case APPLE_SURFACE_FORMAT_AR30  : return AR30ToARGB(src, stride, dst->plane[0], dst->stride[0], w, h);
        case APPLE_SURFACE_FORMAT_AR64:
            return AR64ToARGB((const uint16_t*)src, stride / 2, dst->plane[0], dst->stride[0], w, h);
        case APPLE_SURFACE_FORMAT_AR64_HALF:
            apple_surface_half_to_argb((const uint16_t*)src, stride / 2, dst);
            return 0;
        case APPLE_SURFACE_FORMAT_YUY2: return YUY2ToI422(src, stride, PLANAR3(dst), w, h);
        default                       : return -1;
    }
}

static int apple_surface_packed_from_image(AppleSurfaceFormat format, const AppleImage* src, uint8_t* dst, int stride)
{
    int w = (int)src->width;
    int h = (int)src->height;

    switch (format) {
        case APPLE_SURFACE_FORMAT_ARGB: return ARGBCopy(src->plane[0], src->stride[0], dst, stride, w, h);
        case APPLE_SURFACE_FORMAT_ABGR: return ARGBToABGR(src->plane[0], src->stride[0], dst, stride, w, h);
        case APPLE_SURFACE_FORMAT_AR30: return ARGBToAR30(src->plane[0], src->stride[0], dst, stride, w, h);
        case APPLE_SURFACE_FORMAT_YUY2: return I422ToYUY2(PLANAR3(src), dst, stride, w, h);
        default                       : return -1;
    }
}

static void apple_surface_load_chroma(const AppleImage* image, AddressSpace* as, const AppleSurface* surface)
{
    uint32_t cw, ch;
    uint8_t* uv;

    apple_image_kind_chroma_dims(image->kind, image->width, image->height, &cw, &ch);
    uv = g_malloc((size_t)cw * 2 * ch);

    if (apple_surface_is_16bit_biplanar(surface->format)) {
        uint16_t* uv16 = g_malloc((size_t)cw * 4 * ch);

        apple_surface_dma_rows(as, surface->base[1], surface->stride[1], cw * 4, ch, uv16, false);
        Convert16To8Plane(uv16, (int)cw * 2, uv, (int)cw * 2, 256, (int)cw * 2, (int)ch);
        g_free(uv16);
    }
    else {
        apple_surface_dma_rows(as, surface->base[1], surface->stride[1], cw * 2, ch, uv, false);
    }

    SplitUVPlane(uv, (int)cw * 2, image->plane[1], image->stride[1], image->plane[2], image->stride[2], (int)cw,
                 (int)ch);
    g_free(uv);
}

static void apple_surface_load_luma16(const AppleImage* image, AddressSpace* as, const AppleSurface* surface, int scale)
{
    uint16_t* luma = g_malloc((size_t)image->width * 2 * image->height);

    apple_surface_dma_rows(as, surface->base[0], surface->stride[0], image->width * 2, image->height, luma, false);
    Convert16To8Plane(luma, (int)image->width, image->plane[0], image->stride[0], scale, (int)image->width,
                      (int)image->height);
    g_free(luma);
}

bool apple_surface_load(AppleImage* image, AddressSpace* as, const AppleSurface* surface)
{
    uint32_t row_bytes = surface->width * apple_surface_bytes_per_pixel(surface->format, surface->depth, 0);
    uint8_t* staging;
    int      ret;

    if (row_bytes == 0 || surface->height == 0) { return false; }

    apple_image_init(image, apple_surface_image_kind(surface->format), surface->width, surface->height);

    if (apple_surface_is_16bit_biplanar(surface->format)) {
        apple_surface_load_luma16(image, as, surface, 256);
        apple_surface_load_chroma(image, as, surface);
        return true;
    }

    if (apple_surface_is_biplanar(surface->format)) {
        apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, surface->height, image->plane[0],
                               false);
        apple_surface_load_chroma(image, as, surface);
        return true;
    }

    if (surface->format == APPLE_SURFACE_FORMAT_I400) {
        if (surface->depth == 8) {
            apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, surface->height,
                                   image->plane[0], false);
        }
        else {
            apple_surface_load_luma16(image, as, surface, 65536 >> (surface->depth - 8));
        }
        return true;
    }

    if (surface->format == APPLE_SURFACE_FORMAT_ARGB) {
        apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, surface->height, image->plane[0],
                               false);
        return true;
    }

    staging = g_malloc((size_t)row_bytes * surface->height);
    apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, surface->height, staging, false);
    ret = apple_surface_packed_to_image(surface->format, staging, (int)row_bytes, image);
    g_free(staging);

    if (ret) {
        apple_image_destroy(image);
        return false;
    }
    return true;
}

bool apple_surface_store(const AppleImage* image, AddressSpace* as, const AppleSurface* surface)
{
    uint32_t row_bytes = image->width * apple_surface_bytes_per_pixel(surface->format, surface->depth, 0);
    uint8_t* staging;
    int      ret;

    if (apple_surface_is_biplanar(surface->format)) {
        bool     wide = apple_surface_is_16bit_biplanar(surface->format);
        uint32_t cw, ch;
        uint8_t* uv;

        if (wide) {
            staging = g_malloc((size_t)row_bytes * image->height);
            Convert8To16Plane(image->plane[0], image->stride[0], (uint16_t*)staging, (int)image->width, 16,
                              (int)image->width, (int)image->height);
            apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, staging, true);
            g_free(staging);
        }
        else {
            apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, image->plane[0],
                                   true);
        }

        apple_image_kind_chroma_dims(image->kind, image->width, image->height, &cw, &ch);
        uv = g_malloc((size_t)cw * 2 * ch);
        MergeUVPlane(image->plane[1], image->stride[1], image->plane[2], image->stride[2], uv, (int)cw * 2, (int)cw,
                     (int)ch);

        if (wide) {
            uint16_t* uv16 = g_malloc((size_t)cw * 4 * ch);

            Convert8To16Plane(uv, (int)cw * 2, uv16, (int)cw * 2, 16, (int)cw * 2, (int)ch);
            apple_surface_dma_rows(as, surface->base[1], surface->stride[1], cw * 4, ch, uv16, true);
            g_free(uv16);
        }
        else {
            apple_surface_dma_rows(as, surface->base[1], surface->stride[1], cw * 2, ch, uv, true);
        }
        g_free(uv);
        return true;
    }

    if (surface->format == APPLE_SURFACE_FORMAT_I400) {
        if (surface->depth == 8) {
            apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, image->plane[0],
                                   true);
            return true;
        }

        staging = g_malloc((size_t)row_bytes * image->height);
        Convert8To16Plane(image->plane[0], image->stride[0], (uint16_t*)staging, (int)(row_bytes / 2),
                          (int)surface->depth, (int)image->width, (int)image->height);
        apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, staging, true);
        g_free(staging);
        return true;
    }

    if (surface->format == APPLE_SURFACE_FORMAT_ARGB) {
        apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, image->plane[0],
                               true);
        return true;
    }

    staging = g_malloc((size_t)row_bytes * image->height);
    ret     = apple_surface_packed_from_image(surface->format, image, staging, (int)row_bytes);
    if (ret == 0) {
        apple_surface_dma_rows(as, surface->base[0], surface->stride[0], row_bytes, image->height, staging, true);
    }
    g_free(staging);
    return ret == 0;
}

void apple_surface_copy(AddressSpace* as, const AppleSurface* src, const AppleSurface* dst)
{
    uint32_t row_bytes = src->width * apple_surface_bytes_per_pixel(src->format, src->depth, 0);
    uint8_t* buf;

    buf = g_malloc((size_t)row_bytes * src->height);
    apple_surface_dma_rows(as, src->base[0], src->stride[0], row_bytes, src->height, buf, false);
    apple_surface_dma_rows(as, dst->base[0], dst->stride[0], row_bytes, dst->height, buf, true);
    g_free(buf);

    if (apple_surface_is_biplanar(src->format)) {
        uint32_t cw, ch;

        apple_image_kind_chroma_dims(apple_surface_image_kind(src->format), src->width, src->height, &cw, &ch);
        row_bytes = cw * apple_surface_bytes_per_pixel(src->format, src->depth, 1);
        buf       = g_malloc((size_t)row_bytes * ch);
        apple_surface_dma_rows(as, src->base[1], src->stride[1], row_bytes, ch, buf, false);
        apple_surface_dma_rows(as, dst->base[1], dst->stride[1], row_bytes, ch, buf, true);
        g_free(buf);
    }
}
