/*
 * Apple DFU and Recovery Mode USB device.
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
#include "qom/object.h"

#define TYPE_APPLE_DFU "apple-dfu"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDFUState, APPLE_DFU)

typedef void (*AppleDFUAttachFunc)(void* opaque, bool recovery);

/// Attach in DFU Mode, reporting `ap_nonce` and `sep_nonce` to the host. `attached` is called on every attach, in DFU
/// or Recovery Mode.
void apple_dfu_start(AppleDFUState* s, const uint8_t* ap_nonce, size_t ap_nonce_len, const uint8_t* sep_nonce,
                     size_t sep_nonce_len, AppleDFUAttachFunc attached, void* opaque);

/// Whether the device is started and the host has not sent `bootx` yet.
bool apple_dfu_waiting(AppleDFUState* s);

/// Whether the host has sent `bootx`.
bool apple_dfu_booted(AppleDFUState* s);

/// Copy the manifest the booted images were personalised with.
bool apple_dfu_copy_ticket(AppleDFUState* s, gchar** data, gsize* length);

/// Copy the last uploaded image with IM4P payload `type`.
bool apple_dfu_copy_image(AppleDFUState* s, const char* type, uint8_t** data, gsize* length);
