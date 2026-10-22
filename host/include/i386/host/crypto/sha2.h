/*
 * x86 specific sha2 acceleration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "host/cpuinfo.h"
#include <immintrin.h>

#if defined(__SHA__) && defined(__SSSE3__)
    #define HAVE_SHA256_ACCEL true
    #define ATTR_SHA256_ACCEL
#else
    #define HAVE_SHA256_ACCEL likely(cpuinfo& CPUINFO_SHA)
    #define ATTR_SHA256_ACCEL __attribute__((target("sha,ssse3")))
#endif
#if defined(__SHA512__)
    #define HAVE_SHA512_ACCEL true
    #define ATTR_SHA512_ACCEL
#elif defined(__has_include)
    #if __has_include(<sha512intrin.h>)
        #define ATTR_SHA512_ACCEL __attribute__((target("sha512")))
    #endif
#endif
#ifndef HAVE_SHA512_ACCEL
    #ifdef ATTR_SHA512_ACCEL
        #define HAVE_SHA512_ACCEL likely(cpuinfo& CPUINFO_SHA512)
    #else
        #define HAVE_SHA512_ACCEL false
    #endif
#endif

static inline __m128i ATTR_SHA256_ACCEL sha256_accel_rounds(__m128i abcd, __m128i efgh, __m128i wk, __m128i* cdgh)
{
    __m128i dcba = _mm_shuffle_epi32(abcd, 0x1b);
    __m128i hgfe = _mm_shuffle_epi32(efgh, 0x1b);
    __m128i abef = _mm_unpackhi_epi64(hgfe, dcba);

    *cdgh = _mm_sha256rnds2_epu32(_mm_unpacklo_epi64(hgfe, dcba), abef, wk);
    return _mm_sha256rnds2_epu32(abef, *cdgh, _mm_shuffle_epi32(wk, 0x0e));
}

static inline void ATTR_SHA256_ACCEL sha256h_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    __m128i cdgh;
    __m128i abef = sha256_accel_rounds(_mm_loadu_si128((const __m128i*)rd), _mm_loadu_si128((const __m128i*)rn),
                                       _mm_loadu_si128((const __m128i*)rm), &cdgh);

    _mm_storeu_si128((__m128i*)rd, _mm_shuffle_epi32(_mm_unpackhi_epi64(cdgh, abef), 0x1b));
}

static inline void ATTR_SHA256_ACCEL sha256h2_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    __m128i cdgh;
    __m128i abef = sha256_accel_rounds(_mm_loadu_si128((const __m128i*)rn), _mm_loadu_si128((const __m128i*)rd),
                                       _mm_loadu_si128((const __m128i*)rm), &cdgh);

    _mm_storeu_si128((__m128i*)rd, _mm_shuffle_epi32(_mm_unpacklo_epi64(cdgh, abef), 0x1b));
}

static inline void ATTR_SHA256_ACCEL sha256su0_accel(uint64_t* rd, const uint64_t* rm)
{
    __m128i d = _mm_loadu_si128((const __m128i*)rd);
    __m128i m = _mm_loadu_si128((const __m128i*)rm);

    _mm_storeu_si128((__m128i*)rd, _mm_sha256msg1_epu32(d, m));
}

static inline void ATTR_SHA256_ACCEL sha256su1_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    __m128i d = _mm_loadu_si128((const __m128i*)rd);
    __m128i n = _mm_loadu_si128((const __m128i*)rn);
    __m128i m = _mm_loadu_si128((const __m128i*)rm);

    _mm_storeu_si128((__m128i*)rd, _mm_sha256msg2_epu32(_mm_add_epi32(d, _mm_alignr_epi8(m, n, 4)), m));
}

#ifdef ATTR_SHA512_ACCEL
static inline void ATTR_SHA512_ACCEL sha512h_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    uint64_t m0   = rm[0];
    __m256i  abef = _mm256_set_epi64x(0, 0, rm[1], rn[0]);
    __m256i  cdgh = _mm256_set_epi64x(0, m0, rn[1], 0);
    __m128i  wk   = _mm_set_epi64x(rd[0] - rn[1], rd[1]);
    uint64_t r[4];

    _mm256_storeu_si256((__m256i*)r, _mm256_sha512rnds2_epi64(cdgh, abef, wk));
    rd[0] = r[1];
    rd[1] = r[0] - m0;
}

static inline void ATTR_SHA512_ACCEL sha512h2_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    __m256i  abef = _mm256_set_epi64x(rm[0], rm[1], 0, 0);
    __m256i  cdgh = _mm256_set_epi64x(rn[0], -rd[1], 0, 0);
    __m128i  wk   = _mm_set_epi64x(rd[0], rd[1]);
    uint64_t r[4];

    _mm256_storeu_si256((__m256i*)r, _mm256_sha512rnds2_epi64(cdgh, abef, wk));
    rd[0] = r[3];
    rd[1] = r[2];
}

static inline void ATTR_SHA512_ACCEL sha512su0_accel(uint64_t* rd, const uint64_t* rn)
{
    uint64_t r[4];

    _mm256_storeu_si256((__m256i*)r,
                        _mm256_sha512msg1_epi64(_mm256_set_epi64x(0, rn[0], rd[1], rd[0]), _mm_setzero_si128()));
    rd[0] = r[0];
    rd[1] = r[1];
}

static inline void ATTR_SHA512_ACCEL sha512su1_accel(uint64_t* rd, const uint64_t* rn, const uint64_t* rm)
{
    __m256i  d = _mm256_set_epi64x(0, 0, rd[1] + rm[1], rd[0] + rm[0]);
    __m256i  n = _mm256_set_epi64x(rn[1], rn[0], 0, 0);
    uint64_t r[4];

    _mm256_storeu_si256((__m256i*)r, _mm256_sha512msg2_epi64(d, n));
    rd[0] = r[0];
    rd[1] = r[1];
}
#else
void sha512h_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512h2_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512su0_accel(uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
void sha512su1_accel(uint64_t*, const uint64_t*, const uint64_t*) QEMU_ERROR("unsupported accel");
#endif
