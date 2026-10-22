/*
 * No host specific sha2 acceleration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#define HAVE_SHA256_ACCEL false
#define HAVE_SHA512_ACCEL false

void sha256h_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha256h2_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha256su0_accel(uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha256su1_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");

void sha512h_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512h2_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512su0_accel(uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512su1_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
