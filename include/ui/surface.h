/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU UI Console
 */
#pragma once

#include "graphics/inferno_pixbuf.h"

#define QEMU_ALLOCATED_FLAG   0x01
#define QEMU_PLACEHOLDER_FLAG 0x02

/* 32-bit xRGB, libyuv's ARGB. */
typedef struct DisplaySurface
{
    InfernoPixbuf image;
    uint8_t       flags;
    unsigned      refcount;
} DisplaySurface;

DisplaySurface* qemu_create_displaysurface_from(int width, int height, int linesize, uint8_t* data);
DisplaySurface* qemu_create_placeholder_surface(int w, int h, const char* msg);

DisplaySurface* qemu_create_displaysurface(int width, int height);
DisplaySurface* qemu_displaysurface_ref(DisplaySurface* surface);
void            qemu_displaysurface_unref(DisplaySurface* surface);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(DisplaySurface, qemu_displaysurface_unref)

static inline int surface_is_allocated(DisplaySurface* surface) { return surface->flags & QEMU_ALLOCATED_FLAG; }

static inline int surface_is_placeholder(DisplaySurface* surface) { return surface->flags & QEMU_PLACEHOLDER_FLAG; }

static inline int surface_stride(DisplaySurface* s) { return s->image.stride[0]; }

static inline void* surface_data(DisplaySurface* s) { return s->image.plane[0]; }

static inline int surface_width(DisplaySurface* s) { return (int)s->image.width; }

static inline int surface_height(DisplaySurface* s) { return (int)s->image.height; }
