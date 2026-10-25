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
#pragma once

#include "qemu/osdep.h"
#include "system/dma.h"

typedef enum AppleSurfaceFormat
{
    APPLE_SURFACE_FORMAT_NONE,
    APPLE_SURFACE_FORMAT_ARGB, /* B, G, R, A bytes */
    APPLE_SURFACE_FORMAT_ABGR, /* R, G, B, A bytes */
    APPLE_SURFACE_FORMAT_RGB565,
    APPLE_SURFACE_FORMAT_AR30,
    APPLE_SURFACE_FORMAT_AR64,      /* B, G, R, A, 16 bits each, MSB-aligned */
    APPLE_SURFACE_FORMAT_AR64_HALF, /* B, G, R, A, half floats */
    APPLE_SURFACE_FORMAT_YUY2,
    APPLE_SURFACE_FORMAT_NV12,
    APPLE_SURFACE_FORMAT_NV16,
    APPLE_SURFACE_FORMAT_NV24,
    APPLE_SURFACE_FORMAT_P010, /* NV12 with 16-bit MSB-aligned samples */
    APPLE_SURFACE_FORMAT_P210,
    APPLE_SURFACE_FORMAT_P410,
    APPLE_SURFACE_FORMAT_I400, /* luma, depth bits LSB-aligned in 8 or 16 */
} AppleSurfaceFormat;

typedef enum AppleSurfaceMatrix
{
    APPLE_SURFACE_MATRIX_BT601,
    APPLE_SURFACE_MATRIX_BT709,
} AppleSurfaceMatrix;

/* Plane 0 is luma or the packed pixels, plane 1 interleaved chroma. */
typedef struct AppleSurface
{
    AppleSurfaceFormat format;
    uint32_t           depth;
    uint32_t           width;
    uint32_t           height;
    dma_addr_t         base[2];
    uint32_t           stride[2];
} AppleSurface;

typedef enum AppleImageKind
{
    APPLE_IMAGE_ARGB,
    APPLE_IMAGE_I444,
    APPLE_IMAGE_I422,
    APPLE_IMAGE_I420,
    APPLE_IMAGE_I400,
} AppleImageKind;

typedef struct AppleImage
{
    uint8_t*       plane[3];
    int            stride[3];
    uint32_t       width;
    uint32_t       height;
    AppleImageKind kind;
} AppleImage;

uint32_t apple_surface_bytes_per_pixel(AppleSurfaceFormat format, uint32_t depth, unsigned plane);
bool     apple_surface_is_biplanar(AppleSurfaceFormat format);
bool     apple_surface_load(AppleImage* image, AddressSpace* as, const AppleSurface* surface);
bool     apple_surface_store(const AppleImage* image, AddressSpace* as, const AppleSurface* surface);
void     apple_surface_copy(AddressSpace* as, const AppleSurface* src, const AppleSurface* dst);

AppleImageKind apple_surface_image_kind(AppleSurfaceFormat format);
void           apple_image_init(AppleImage* image, AppleImageKind kind, uint32_t width, uint32_t height);
void           apple_image_destroy(AppleImage* image);
void           apple_image_rotate(AppleImage* dst, const AppleImage* src, int rotation);
void           apple_image_scale(AppleImage* dst, const AppleImage* src, uint32_t width, uint32_t height);
void           apple_image_mirror(AppleImage* dst, const AppleImage* src, bool flip_x);
int  apple_image_convert(AppleImage* dst, const AppleImage* src, AppleImageKind kind, AppleSurfaceMatrix matrix);
void apple_image_shuffle(AppleImage* image, const uint8_t order[4]);
void apple_image_premultiply(AppleImage* image);
