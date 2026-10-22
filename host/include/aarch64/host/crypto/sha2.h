/*
 * AArch64 specific sha2 acceleration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "host/cpuinfo.h"
#include <arm_neon.h>

#ifdef __ARM_FEATURE_SHA2
    #define HAVE_SHA256_ACCEL true
#else
    #define HAVE_SHA256_ACCEL likely(cpuinfo& CPUINFO_SHA256)
#endif
#define HAVE_SHA512_ACCEL false

static inline void sha256h_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    uint32x4_t d = vreinterpretq_u32_u64(vld1q_u64(rd));
    uint32x4_t n = vreinterpretq_u32_u64(vld1q_u64(rn));
    uint32x4_t m = vreinterpretq_u32_u64(vld1q_u64(rm));

    asm(".arch_extension sha2\n\t"
        "sha256h %q0, %q1, %2.4s"
        : "+w"(d)
        : "w"(n), "w"(m));
    vst1q_u64(rd, vreinterpretq_u64_u32(d));
}

static inline void sha256h2_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    uint32x4_t d = vreinterpretq_u32_u64(vld1q_u64(rd));
    uint32x4_t n = vreinterpretq_u32_u64(vld1q_u64(rn));
    uint32x4_t m = vreinterpretq_u32_u64(vld1q_u64(rm));

    asm(".arch_extension sha2\n\t"
        "sha256h2 %q0, %q1, %2.4s"
        : "+w"(d)
        : "w"(n), "w"(m));
    vst1q_u64(rd, vreinterpretq_u64_u32(d));
}

static inline void sha256su0_accel(uint64_t* rd, const uint64_t* rm)
{
    uint32x4_t d = vreinterpretq_u32_u64(vld1q_u64(rd));
    uint32x4_t m = vreinterpretq_u32_u64(vld1q_u64(rm));

    asm(".arch_extension sha2\n\t"
        "sha256su0 %0.4s, %1.4s"
        : "+w"(d)
        : "w"(m));
    vst1q_u64(rd, vreinterpretq_u64_u32(d));
}

static inline void sha256su1_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    uint32x4_t d = vreinterpretq_u32_u64(vld1q_u64(rd));
    uint32x4_t n = vreinterpretq_u32_u64(vld1q_u64(rn));
    uint32x4_t m = vreinterpretq_u32_u64(vld1q_u64(rm));

    asm(".arch_extension sha2\n\t"
        "sha256su1 %0.4s, %1.4s, %2.4s"
        : "+w"(d)
        : "w"(n), "w"(m));
    vst1q_u64(rd, vreinterpretq_u64_u32(d));
}

void sha512h_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512h2_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512su0_accel(uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512su1_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
