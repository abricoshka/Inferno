/*
 * QEMU float support
 *
 * The code in this source file is derived from release 2a of the SoftFloat
 * IEC/IEEE Floating-point Arithmetic Package. Those parts of the code (and
 * some later contributions) are provided under that license, as detailed below.
 * It has subsequently been modified by contributors to the QEMU Project,
 * so some portions are provided under:
 *  the SoftFloat-2a license
 *  the BSD license
 *  GPL-v2-or-later
 *
 * Any future contributions to this file after December 1st 2014 will be
 * taken to be licensed under the Softfloat-2a license unless specifically
 * indicated otherwise.
 */

/*
===============================================================================
This C header file is part of the SoftFloat IEC/IEEE Floating-point
Arithmetic Package, Release 2a.

Written by John R. Hauser.  This work was made possible in part by the
International Computer Science Institute, located at Suite 600, 1947 Center
Street, Berkeley, California 94704.  Funding was partially provided by the
National Science Foundation under grant MIP-9311980.  The original version
of this code was written as part of a project to build a fixed-point vector
processor in collaboration with the University of California at Berkeley,
overseen by Profs. Nelson Morgan and John Wawrzynek.  More information
is available through the Web page `http://HTTP.CS.Berkeley.EDU/~jhauser/
arithmetic/SoftFloat.html'.

THIS SOFTWARE IS DISTRIBUTED AS IS, FOR FREE.  Although reasonable effort
has been made to avoid it, THIS SOFTWARE MAY CONTAIN FAULTS THAT WILL AT
TIMES RESULT IN INCORRECT BEHAVIOR.  USE OF THIS SOFTWARE IS RESTRICTED TO
PERSONS AND ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ANY
AND ALL LOSSES, COSTS, OR OTHER PROBLEMS ARISING FROM ITS USE.

Derivative works are acceptable, even for commercial purposes, so long as
(1) they include prominent notice that the work is derivative, and (2) they
include prominent notice akin to these four paragraphs for those parts of
this code that are retained.

===============================================================================
*/

/* BSD licensing:
 * Copyright (c) 2006, Fabrice Bellard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Portions of this work are licensed under the terms of the GNU GPL,
 * version 2 or later. See the COPYING file in the top-level directory.
 */

#pragma once

/*----------------------------------------------------------------------------
| Software IEC/IEEE floating-point ordering relations
*----------------------------------------------------------------------------*/

typedef enum
{
    float_relation_less      = -1,
    float_relation_equal     = 0,
    float_relation_greater   = 1,
    float_relation_unordered = 2
} FloatRelation;

#include "fpu/softfloat-types.h"
#include "fpu/softfloat-helpers.h"
#include "qemu/int128.h"

/*----------------------------------------------------------------------------
| Routine to raise any or all of the software IEC/IEEE floating-point
| exception flags.
*----------------------------------------------------------------------------*/
static inline void float_raise(uint16_t flags, float_status* status) { status->float_exception_flags |= flags; }

/*----------------------------------------------------------------------------
| If `a' is denormal and we are in flush-to-zero mode then set the
| input-denormal exception and return zero. Otherwise just return the value.
*----------------------------------------------------------------------------*/
float16  float16_squash_input_denormal(float16 a, float_status* status);
float32  float32_squash_input_denormal(float32 a, float_status* status);
float64  float64_squash_input_denormal(float64 a, float_status* status);
bfloat16 bfloat16_squash_input_denormal(bfloat16 a, float_status* status);

/*----------------------------------------------------------------------------
| Options to indicate which negations to perform in float*_muladd()
| Using these differs from negating an input or output before calling
| the muladd function in that this means that a NaN doesn't have its
| sign bit inverted before it is propagated.
*----------------------------------------------------------------------------*/
enum
{
    float_muladd_negate_c       = 1,
    float_muladd_negate_product = 2,
    float_muladd_negate_result  = 4,
};

/*----------------------------------------------------------------------------
| Software IEC/IEEE integer-to-floating-point conversion routines.
*----------------------------------------------------------------------------*/

float16 int16_to_float16_scalbn(int16_t a, int, float_status* status);
float16 int32_to_float16_scalbn(int32_t a, int, float_status* status);
float16 int64_to_float16_scalbn(int64_t a, int, float_status* status);
float16 uint16_to_float16_scalbn(uint16_t a, int, float_status* status);
float16 uint32_to_float16_scalbn(uint32_t a, int, float_status* status);
float16 uint64_to_float16_scalbn(uint64_t a, int, float_status* status);

float16 int8_to_float16(int8_t a, float_status* status);
float16 int16_to_float16(int16_t a, float_status* status);
float16 int32_to_float16(int32_t a, float_status* status);
float16 int64_to_float16(int64_t a, float_status* status);
float16 uint8_to_float16(uint8_t a, float_status* status);
float16 uint16_to_float16(uint16_t a, float_status* status);
float16 uint32_to_float16(uint32_t a, float_status* status);
float16 uint64_to_float16(uint64_t a, float_status* status);

float32 int16_to_float32_scalbn(int16_t, int, float_status* status);
float32 int32_to_float32_scalbn(int32_t, int, float_status* status);
float32 int64_to_float32_scalbn(int64_t, int, float_status* status);
float32 uint16_to_float32_scalbn(uint16_t, int, float_status* status);
float32 uint32_to_float32_scalbn(uint32_t, int, float_status* status);
float32 uint64_to_float32_scalbn(uint64_t, int, float_status* status);

float32 int16_to_float32(int16_t, float_status* status);
float32 int32_to_float32(int32_t, float_status* status);
float32 int64_to_float32(int64_t, float_status* status);
float32 uint16_to_float32(uint16_t, float_status* status);
float32 uint32_to_float32(uint32_t, float_status* status);
float32 uint64_to_float32(uint64_t, float_status* status);

float64 int16_to_float64_scalbn(int16_t, int, float_status* status);
float64 int32_to_float64_scalbn(int32_t, int, float_status* status);
float64 int64_to_float64_scalbn(int64_t, int, float_status* status);
float64 uint16_to_float64_scalbn(uint16_t, int, float_status* status);
float64 uint32_to_float64_scalbn(uint32_t, int, float_status* status);
float64 uint64_to_float64_scalbn(uint64_t, int, float_status* status);

float64 int16_to_float64(int16_t, float_status* status);
float64 int32_to_float64(int32_t, float_status* status);
float64 int64_to_float64(int64_t, float_status* status);
float64 uint16_to_float64(uint16_t, float_status* status);
float64 uint32_to_float64(uint32_t, float_status* status);
float64 uint64_to_float64(uint64_t, float_status* status);
/*----------------------------------------------------------------------------
| OCP FP{4,8} conversion routines.
*----------------------------------------------------------------------------*/

float8_e4m3 float4_e2m1_to_float8_e4m3(float4_e2m1, float_status* status);

bfloat16    float8_e4m3_to_bfloat16(float8_e4m3, float_status* status);
float8_e4m3 bfloat16_to_float8_e4m3(bfloat16, bool sat, float_status* status);
float8_e4m3 float32_to_float8_e4m3(float32, bool sat, float_status* status);

bfloat16    float8_e5m2_to_bfloat16(float8_e5m2, float_status* status);
float8_e5m2 bfloat16_to_float8_e5m2(bfloat16, bool sat, float_status* status);
float8_e5m2 float32_to_float8_e5m2(float32, bool sat, float_status* status);

/*----------------------------------------------------------------------------
| Software half-precision conversion routines.
*----------------------------------------------------------------------------*/

float16 float32_to_float16(float32, bool ieee, float_status* status);
float32 float16_to_float32(float16, bool ieee, float_status* status);
float16 float64_to_float16(float64 a, bool ieee, float_status* status);
float64 float16_to_float64(float16 a, bool ieee, float_status* status);

int8_t  float16_to_int8_scalbn(float16, FloatRoundMode, int, float_status* status);
int16_t float16_to_int16_scalbn(float16, FloatRoundMode, int, float_status*);
int32_t float16_to_int32_scalbn(float16, FloatRoundMode, int, float_status*);
int64_t float16_to_int64_scalbn(float16, FloatRoundMode, int, float_status*);

int8_t  float16_to_int8(float16, float_status* status);
int16_t float16_to_int16(float16, float_status* status);
int32_t float16_to_int32(float16, float_status* status);
int64_t float16_to_int64(float16, float_status* status);

int16_t float16_to_int16_round_to_zero(float16, float_status* status);
int32_t float16_to_int32_round_to_zero(float16, float_status* status);
int64_t float16_to_int64_round_to_zero(float16, float_status* status);

uint8_t  float16_to_uint8_scalbn(float16 a, FloatRoundMode, int, float_status* status);
uint16_t float16_to_uint16_scalbn(float16 a, FloatRoundMode, int, float_status* status);
uint32_t float16_to_uint32_scalbn(float16 a, FloatRoundMode, int, float_status* status);
uint64_t float16_to_uint64_scalbn(float16 a, FloatRoundMode, int, float_status* status);

uint8_t  float16_to_uint8(float16 a, float_status* status);
uint16_t float16_to_uint16(float16 a, float_status* status);
uint32_t float16_to_uint32(float16 a, float_status* status);
uint64_t float16_to_uint64(float16 a, float_status* status);

uint16_t float16_to_uint16_round_to_zero(float16 a, float_status* status);
uint32_t float16_to_uint32_round_to_zero(float16 a, float_status* status);
uint64_t float16_to_uint64_round_to_zero(float16 a, float_status* status);

/*----------------------------------------------------------------------------
| Software half-precision operations.
*----------------------------------------------------------------------------*/

float16       float16_round_to_int(float16, float_status* status);
float16       float16_add(float16, float16, float_status* status);
float16       float16_sub(float16, float16, float_status* status);
float16       float16_mul(float16, float16, float_status* status);
float16       float16_muladd(float16, float16, float16, int, float_status* status);
float16       float16_muladd_scalbn(float16, float16, float16, int, int, float_status* status);
float16       float16_div(float16, float16, float_status* status);
float16       float16_scalbn(float16, int, float_status* status);
float16       float16_min(float16, float16, float_status* status);
float16       float16_max(float16, float16, float_status* status);
float16       float16_minnum(float16, float16, float_status* status);
float16       float16_maxnum(float16, float16, float_status* status);
float16       float16_minnummag(float16, float16, float_status* status);
float16       float16_maxnummag(float16, float16, float_status* status);
float16       float16_minimum_number(float16, float16, float_status* status);
float16       float16_maximum_number(float16, float16, float_status* status);
float16       float16_sqrt(float16, float_status* status);
FloatRelation float16_compare(float16, float16, float_status* status);
FloatRelation float16_compare_quiet(float16, float16, float_status* status);

bool    float16_is_quiet_nan(float16, float_status* status);
bool    float16_is_signaling_nan(float16, float_status* status);
float16 float16_silence_nan(float16, float_status* status);

static inline bool float16_is_any_nan(float16 a) { return ((float16_val(a) & ~0x8000) > 0x7c00); }

static inline bool float16_is_neg(float16 a) { return float16_val(a) >> 15; }

static inline bool float16_is_infinity(float16 a) { return (float16_val(a) & 0x7fff) == 0x7c00; }

static inline bool float16_is_zero(float16 a) { return (float16_val(a) & 0x7fff) == 0; }

static inline bool float16_is_zero_or_denormal(float16 a) { return (float16_val(a) & 0x7c00) == 0; }

static inline bool float16_is_normal(float16 a) { return (((float16_val(a) >> 10) + 1) & 0x1f) >= 2; }

static inline float16 float16_abs(float16 a)
{
    /* Note that abs does *not* handle NaN specially, nor does
     * it flush denormal inputs to zero.
     */
    return make_float16(float16_val(a) & 0x7fff);
}

static inline float16 float16_chs(float16 a)
{
    /* Note that chs does *not* handle NaN specially, nor does
     * it flush denormal inputs to zero.
     */
    return make_float16(float16_val(a) ^ 0x8000);
}

static inline float16 float16_set_sign(float16 a, int sign)
{ return make_float16((float16_val(a) & 0x7fff) | (sign << 15)); }

static inline bool float16_eq(float16 a, float16 b, float_status* s)
{ return float16_compare(a, b, s) == float_relation_equal; }

static inline bool float16_le(float16 a, float16 b, float_status* s)
{ return float16_compare(a, b, s) <= float_relation_equal; }

static inline bool float16_lt(float16 a, float16 b, float_status* s)
{ return float16_compare(a, b, s) < float_relation_equal; }

static inline bool float16_unordered(float16 a, float16 b, float_status* s)
{ return float16_compare(a, b, s) == float_relation_unordered; }

static inline bool float16_eq_quiet(float16 a, float16 b, float_status* s)
{ return float16_compare_quiet(a, b, s) == float_relation_equal; }

static inline bool float16_le_quiet(float16 a, float16 b, float_status* s)
{ return float16_compare_quiet(a, b, s) <= float_relation_equal; }

static inline bool float16_lt_quiet(float16 a, float16 b, float_status* s)
{ return float16_compare_quiet(a, b, s) < float_relation_equal; }

static inline bool float16_unordered_quiet(float16 a, float16 b, float_status* s)
{ return float16_compare_quiet(a, b, s) == float_relation_unordered; }

#define float16_zero           make_float16(0)
#define float16_half           make_float16(0x3800)
#define float16_one            make_float16(0x3c00)
#define float16_one_point_five make_float16(0x3e00)
#define float16_two            make_float16(0x4000)
#define float16_three          make_float16(0x4200)
#define float16_infinity       make_float16(0x7c00)

/*----------------------------------------------------------------------------
| Software bfloat16 conversion routines.
*----------------------------------------------------------------------------*/

bfloat16 bfloat16_round_to_int(bfloat16, float_status* status);
bfloat16 float32_to_bfloat16(float32, float_status* status);
float32  bfloat16_to_float32(bfloat16, float_status* status);
bfloat16 float64_to_bfloat16(float64 a, float_status* status);
float64  bfloat16_to_float64(bfloat16 a, float_status* status);

int8_t  bfloat16_to_int8_scalbn(bfloat16, FloatRoundMode, int, float_status* status);
int16_t bfloat16_to_int16_scalbn(bfloat16, FloatRoundMode, int, float_status* status);
int32_t bfloat16_to_int32_scalbn(bfloat16, FloatRoundMode, int, float_status* status);
int64_t bfloat16_to_int64_scalbn(bfloat16, FloatRoundMode, int, float_status* status);

int8_t  bfloat16_to_int8(bfloat16, float_status* status);
int16_t bfloat16_to_int16(bfloat16, float_status* status);
int32_t bfloat16_to_int32(bfloat16, float_status* status);
int64_t bfloat16_to_int64(bfloat16, float_status* status);

int8_t  bfloat16_to_int8_round_to_zero(bfloat16, float_status* status);
int16_t bfloat16_to_int16_round_to_zero(bfloat16, float_status* status);
int32_t bfloat16_to_int32_round_to_zero(bfloat16, float_status* status);
int64_t bfloat16_to_int64_round_to_zero(bfloat16, float_status* status);

uint8_t  bfloat16_to_uint8_scalbn(bfloat16 a, FloatRoundMode, int, float_status* status);
uint16_t bfloat16_to_uint16_scalbn(bfloat16 a, FloatRoundMode, int, float_status* status);
uint32_t bfloat16_to_uint32_scalbn(bfloat16 a, FloatRoundMode, int, float_status* status);
uint64_t bfloat16_to_uint64_scalbn(bfloat16 a, FloatRoundMode, int, float_status* status);

uint8_t  bfloat16_to_uint8(bfloat16 a, float_status* status);
uint16_t bfloat16_to_uint16(bfloat16 a, float_status* status);
uint32_t bfloat16_to_uint32(bfloat16 a, float_status* status);
uint64_t bfloat16_to_uint64(bfloat16 a, float_status* status);

uint8_t  bfloat16_to_uint8_round_to_zero(bfloat16 a, float_status* status);
uint16_t bfloat16_to_uint16_round_to_zero(bfloat16 a, float_status* status);
uint32_t bfloat16_to_uint32_round_to_zero(bfloat16 a, float_status* status);
uint64_t bfloat16_to_uint64_round_to_zero(bfloat16 a, float_status* status);

bfloat16 int8_to_bfloat16_scalbn(int8_t a, int, float_status* status);
bfloat16 int16_to_bfloat16_scalbn(int16_t a, int, float_status* status);
bfloat16 int32_to_bfloat16_scalbn(int32_t a, int, float_status* status);
bfloat16 int64_to_bfloat16_scalbn(int64_t a, int, float_status* status);
bfloat16 uint8_to_bfloat16_scalbn(uint8_t a, int, float_status* status);
bfloat16 uint16_to_bfloat16_scalbn(uint16_t a, int, float_status* status);
bfloat16 uint32_to_bfloat16_scalbn(uint32_t a, int, float_status* status);
bfloat16 uint64_to_bfloat16_scalbn(uint64_t a, int, float_status* status);

bfloat16 int8_to_bfloat16(int8_t a, float_status* status);
bfloat16 int16_to_bfloat16(int16_t a, float_status* status);
bfloat16 int32_to_bfloat16(int32_t a, float_status* status);
bfloat16 int64_to_bfloat16(int64_t a, float_status* status);
bfloat16 uint8_to_bfloat16(uint8_t a, float_status* status);
bfloat16 uint16_to_bfloat16(uint16_t a, float_status* status);
bfloat16 uint32_to_bfloat16(uint32_t a, float_status* status);
bfloat16 uint64_to_bfloat16(uint64_t a, float_status* status);

/*----------------------------------------------------------------------------
| Software bfloat16 operations.
*----------------------------------------------------------------------------*/

bfloat16      bfloat16_add(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_sub(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_mul(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_div(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_muladd(bfloat16, bfloat16, bfloat16, int, float_status* status);
float16       bfloat16_scalbn(bfloat16, int, float_status* status);
bfloat16      bfloat16_min(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_max(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_minnum(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_maxnum(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_minnummag(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_maxnummag(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_minimum_number(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_maximum_number(bfloat16, bfloat16, float_status* status);
bfloat16      bfloat16_sqrt(bfloat16, float_status* status);
FloatRelation bfloat16_compare(bfloat16, bfloat16, float_status* status);
FloatRelation bfloat16_compare_quiet(bfloat16, bfloat16, float_status* status);

bool     bfloat16_is_quiet_nan(bfloat16, float_status* status);
bool     bfloat16_is_signaling_nan(bfloat16, float_status* status);
bfloat16 bfloat16_silence_nan(bfloat16, float_status* status);
bfloat16 bfloat16_default_nan(float_status* status);

static inline bool bfloat16_is_any_nan(bfloat16 a) { return ((a & ~0x8000) > 0x7F80); }

static inline bool bfloat16_is_neg(bfloat16 a) { return a >> 15; }

static inline bool bfloat16_is_infinity(bfloat16 a) { return (a & 0x7fff) == 0x7F80; }

static inline bool bfloat16_is_zero(bfloat16 a) { return (a & 0x7fff) == 0; }

static inline bool bfloat16_is_zero_or_denormal(bfloat16 a) { return (a & 0x7F80) == 0; }

static inline bool bfloat16_is_normal(bfloat16 a) { return (((a >> 7) + 1) & 0xff) >= 2; }

static inline bfloat16 bfloat16_abs(bfloat16 a)
{
    /* Note that abs does *not* handle NaN specially, nor does
     * it flush denormal inputs to zero.
     */
    return a & 0x7fff;
}

static inline bfloat16 bfloat16_chs(bfloat16 a)
{
    /* Note that chs does *not* handle NaN specially, nor does
     * it flush denormal inputs to zero.
     */
    return a ^ 0x8000;
}

static inline bfloat16 bfloat16_set_sign(bfloat16 a, int sign) { return (a & 0x7fff) | (sign << 15); }

static inline bool bfloat16_eq(bfloat16 a, bfloat16 b, float_status* s)
{ return bfloat16_compare(a, b, s) == float_relation_equal; }

static inline bool bfloat16_le(bfloat16 a, bfloat16 b, float_status* s)
{ return bfloat16_compare(a, b, s) <= float_relation_equal; }

static inline bool bfloat16_lt(bfloat16 a, bfloat16 b, float_status* s)
{ return bfloat16_compare(a, b, s) < float_relation_equal; }

static inline bool bfloat16_unordered(bfloat16 a, bfloat16 b, float_status* s)
{ return bfloat16_compare(a, b, s) == float_relation_unordered; }

static inline bool bfloat16_eq_quiet(bfloat16 a, bfloat16 b, float_status* s)
{ return bfloat16_compare_quiet(a, b, s) == float_relation_equal; }

static inline bool bfloat16_le_quiet(bfloat16 a, bfloat16 b, float_status* s)
{ return bfloat16_compare_quiet(a, b, s) <= float_relation_equal; }

static inline bool bfloat16_lt_quiet(bfloat16 a, bfloat16 b, float_status* s)
{ return bfloat16_compare_quiet(a, b, s) < float_relation_equal; }

static inline bool bfloat16_unordered_quiet(bfloat16 a, bfloat16 b, float_status* s)
{ return bfloat16_compare_quiet(a, b, s) == float_relation_unordered; }

#define bfloat16_zero           0
#define bfloat16_half           0x3f00
#define bfloat16_one            0x3f80
#define bfloat16_one_point_five 0x3fc0
#define bfloat16_two            0x4000
#define bfloat16_three          0x4040
#define bfloat16_infinity       0x7f80

/*----------------------------------------------------------------------------
| The pattern for a default generated half-precision NaN.
*----------------------------------------------------------------------------*/
float16 float16_default_nan(float_status* status);

/*----------------------------------------------------------------------------
| Software IEC/IEEE single-precision conversion routines.
*----------------------------------------------------------------------------*/

int16_t float32_to_int16_scalbn(float32, FloatRoundMode, int, float_status*);
int32_t float32_to_int32_scalbn(float32, FloatRoundMode, int, float_status*);
int64_t float32_to_int64_scalbn(float32, FloatRoundMode, int, float_status*);

int16_t float32_to_int16(float32, float_status* status);
int32_t float32_to_int32(float32, float_status* status);
int64_t float32_to_int64(float32, float_status* status);

int16_t float32_to_int16_round_to_zero(float32, float_status* status);
int32_t float32_to_int32_round_to_zero(float32, float_status* status);
int64_t float32_to_int64_round_to_zero(float32, float_status* status);

uint16_t float32_to_uint16_scalbn(float32, FloatRoundMode, int, float_status*);
uint32_t float32_to_uint32_scalbn(float32, FloatRoundMode, int, float_status*);
uint64_t float32_to_uint64_scalbn(float32, FloatRoundMode, int, float_status*);

uint16_t float32_to_uint16(float32, float_status* status);
uint32_t float32_to_uint32(float32, float_status* status);
uint64_t float32_to_uint64(float32, float_status* status);

uint16_t float32_to_uint16_round_to_zero(float32, float_status* status);
uint32_t float32_to_uint32_round_to_zero(float32, float_status* status);
uint64_t float32_to_uint64_round_to_zero(float32, float_status* status);

float64 float32_to_float64(float32, float_status* status);

/*----------------------------------------------------------------------------
| Software IEC/IEEE single-precision operations.
*----------------------------------------------------------------------------*/
float32       float32_round_to_int(float32, float_status* status);
float32       float32_add(float32, float32, float_status* status);
float32       float32_sub(float32, float32, float_status* status);
float32       float32_mul(float32, float32, float_status* status);
float32       float32_div(float32, float32, float_status* status);
float32       float32_rem(float32, float32, float_status* status);
float32       float32_muladd(float32, float32, float32, int, float_status* status);
float32       float32_muladd_scalbn(float32, float32, float32, int, int, float_status* status);
float32       float32_sqrt(float32, float_status* status);
float32       float32_exp2(float32, float_status* status);
float32       float32_log2(float32, float_status* status);
FloatRelation float32_compare(float32, float32, float_status* status);
FloatRelation float32_compare_quiet(float32, float32, float_status* status);
float32       float32_min(float32, float32, float_status* status);
float32       float32_max(float32, float32, float_status* status);
float32       float32_minnum(float32, float32, float_status* status);
float32       float32_maxnum(float32, float32, float_status* status);
float32       float32_minnummag(float32, float32, float_status* status);
float32       float32_maxnummag(float32, float32, float_status* status);
float32       float32_minimum_number(float32, float32, float_status* status);
float32       float32_maximum_number(float32, float32, float_status* status);
bool          float32_is_quiet_nan(float32, float_status* status);
bool          float32_is_signaling_nan(float32, float_status* status);
float32       float32_silence_nan(float32, float_status* status);
float32       float32_scalbn(float32, int, float_status* status);

static inline float32 float32_abs(float32 a)
{
    /* Note that abs does *not* handle NaN specially, nor does
     * it flush denormal inputs to zero.
     */
    return make_float32(float32_val(a) & 0x7fffffff);
}

static inline float32 float32_chs(float32 a)
{
    /* Note that chs does *not* handle NaN specially, nor does
     * it flush denormal inputs to zero.
     */
    return make_float32(float32_val(a) ^ 0x80000000);
}

static inline bool float32_is_infinity(float32 a) { return (float32_val(a) & 0x7fffffff) == 0x7f800000; }

static inline bool float32_is_neg(float32 a) { return float32_val(a) >> 31; }

static inline bool float32_is_zero(float32 a) { return (float32_val(a) & 0x7fffffff) == 0; }

static inline bool float32_is_any_nan(float32 a) { return ((float32_val(a) & ~(1 << 31)) > 0x7f800000UL); }

static inline bool float32_is_zero_or_denormal(float32 a) { return (float32_val(a) & 0x7f800000) == 0; }

static inline bool float32_is_normal(float32 a) { return (((float32_val(a) >> 23) + 1) & 0xff) >= 2; }

static inline bool float32_is_denormal(float32 a) { return float32_is_zero_or_denormal(a) && !float32_is_zero(a); }

static inline bool float32_is_zero_or_normal(float32 a) { return float32_is_normal(a) || float32_is_zero(a); }

static inline float32 float32_set_sign(float32 a, int sign)
{ return make_float32((float32_val(a) & 0x7fffffff) | (sign << 31)); }

static inline bool float32_eq(float32 a, float32 b, float_status* s)
{ return float32_compare(a, b, s) == float_relation_equal; }

static inline bool float32_le(float32 a, float32 b, float_status* s)
{ return float32_compare(a, b, s) <= float_relation_equal; }

static inline bool float32_lt(float32 a, float32 b, float_status* s)
{ return float32_compare(a, b, s) < float_relation_equal; }

static inline bool float32_unordered(float32 a, float32 b, float_status* s)
{ return float32_compare(a, b, s) == float_relation_unordered; }

static inline bool float32_eq_quiet(float32 a, float32 b, float_status* s)
{ return float32_compare_quiet(a, b, s) == float_relation_equal; }

static inline bool float32_le_quiet(float32 a, float32 b, float_status* s)
{ return float32_compare_quiet(a, b, s) <= float_relation_equal; }

static inline bool float32_lt_quiet(float32 a, float32 b, float_status* s)
{ return float32_compare_quiet(a, b, s) < float_relation_equal; }

static inline bool float32_unordered_quiet(float32 a, float32 b, float_status* s)
{ return float32_compare_quiet(a, b, s) == float_relation_unordered; }

#define float32_zero           make_float32(0)
#define float32_half           make_float32(0x3f000000)
#define float32_one            make_float32(0x3f800000)
#define float32_one_point_five make_float32(0x3fc00000)
#define float32_two            make_float32(0x40000000)
#define float32_three          make_float32(0x40400000)
#define float32_infinity       make_float32(0x7f800000)

/*----------------------------------------------------------------------------
| Packs the sign `zSign', exponent `zExp', and significand `zSig' into a
| single-precision floating-point value, returning the result.  After being
| shifted into the proper positions, the three fields are simply added
| together to form the result.  This means that any integer portion of `zSig'
| will be added into the exponent.  Since a properly normalized significand
| will have an integer portion equal to 1, the `zExp' input should be 1 less
| than the desired result exponent whenever `zSig' is a complete, normalized
| significand.
*----------------------------------------------------------------------------*/

static inline float32 packFloat32(bool zSign, int zExp, uint32_t zSig)
{ return make_float32((((uint32_t)zSign) << 31) + (((uint32_t)zExp) << 23) + zSig); }

/*----------------------------------------------------------------------------
| The pattern for a default generated single-precision NaN.
*----------------------------------------------------------------------------*/
float32 float32_default_nan(float_status* status);

/*----------------------------------------------------------------------------
| Software IEC/IEEE double-precision conversion routines.
*----------------------------------------------------------------------------*/

int16_t float64_to_int16_scalbn(float64, FloatRoundMode, int, float_status*);
int32_t float64_to_int32_scalbn(float64, FloatRoundMode, int, float_status*);
int64_t float64_to_int64_scalbn(float64, FloatRoundMode, int, float_status*);

int16_t float64_to_int16(float64, float_status* status);
int32_t float64_to_int32(float64, float_status* status);
int64_t float64_to_int64(float64, float_status* status);

int16_t float64_to_int16_round_to_zero(float64, float_status* status);
int32_t float64_to_int32_round_to_zero(float64, float_status* status);
int64_t float64_to_int64_round_to_zero(float64, float_status* status);

int32_t float64_to_int32_modulo(float64, FloatRoundMode, float_status* status);
int64_t float64_to_int64_modulo(float64, FloatRoundMode, float_status* status);

uint16_t float64_to_uint16_scalbn(float64, FloatRoundMode, int, float_status*);
uint32_t float64_to_uint32_scalbn(float64, FloatRoundMode, int, float_status*);
uint64_t float64_to_uint64_scalbn(float64, FloatRoundMode, int, float_status*);

uint16_t float64_to_uint16(float64, float_status* status);
uint32_t float64_to_uint32(float64, float_status* status);
uint64_t float64_to_uint64(float64, float_status* status);

uint16_t float64_to_uint16_round_to_zero(float64, float_status* status);
uint32_t float64_to_uint32_round_to_zero(float64, float_status* status);
uint64_t float64_to_uint64_round_to_zero(float64, float_status* status);

float32 float64_to_float32(float64, float_status* status);

/*----------------------------------------------------------------------------
| Software IEC/IEEE double-precision operations.
*----------------------------------------------------------------------------*/
float64       float64_round_to_int(float64, float_status* status);
float64       float64_add(float64, float64, float_status* status);
float64       float64_sub(float64, float64, float_status* status);
float64       float64_mul(float64, float64, float_status* status);
float64       float64_div(float64, float64, float_status* status);
float64       float64_rem(float64, float64, float_status* status);
float64       float64_muladd(float64, float64, float64, int, float_status* status);
float64       float64_muladd_scalbn(float64, float64, float64, int, int, float_status* status);
float64       float64_sqrt(float64, float_status* status);
float64       float64_log2(float64, float_status* status);
FloatRelation float64_compare(float64, float64, float_status* status);
FloatRelation float64_compare_quiet(float64, float64, float_status* status);
float64       float64_min(float64, float64, float_status* status);
float64       float64_max(float64, float64, float_status* status);
float64       float64_minnum(float64, float64, float_status* status);
float64       float64_maxnum(float64, float64, float_status* status);
float64       float64_minnummag(float64, float64, float_status* status);
float64       float64_maxnummag(float64, float64, float_status* status);
float64       float64_minimum_number(float64, float64, float_status* status);
float64       float64_maximum_number(float64, float64, float_status* status);
bool          float64_is_quiet_nan(float64 a, float_status* status);
bool          float64_is_signaling_nan(float64, float_status* status);
float64       float64_silence_nan(float64, float_status* status);
float64       float64_scalbn(float64, int, float_status* status);

static inline float64 float64_abs(float64 a)
{
    /* Note that abs does *not* handle NaN specially, nor does
     * it flush denormal inputs to zero.
     */
    return make_float64(float64_val(a) & 0x7fffffffffffffffLL);
}

static inline float64 float64_chs(float64 a)
{
    /* Note that chs does *not* handle NaN specially, nor does
     * it flush denormal inputs to zero.
     */
    return make_float64(float64_val(a) ^ 0x8000000000000000LL);
}

static inline bool float64_is_infinity(float64 a)
{ return (float64_val(a) & 0x7fffffffffffffffLL) == 0x7ff0000000000000LL; }

static inline bool float64_is_neg(float64 a) { return float64_val(a) >> 63; }

static inline bool float64_is_zero(float64 a) { return (float64_val(a) & 0x7fffffffffffffffLL) == 0; }

static inline bool float64_is_any_nan(float64 a) { return ((float64_val(a) & ~(1ULL << 63)) > 0x7ff0000000000000ULL); }

static inline bool float64_is_zero_or_denormal(float64 a) { return (float64_val(a) & 0x7ff0000000000000LL) == 0; }

static inline bool float64_is_normal(float64 a) { return (((float64_val(a) >> 52) + 1) & 0x7ff) >= 2; }

static inline bool float64_is_denormal(float64 a) { return float64_is_zero_or_denormal(a) && !float64_is_zero(a); }

static inline bool float64_is_zero_or_normal(float64 a) { return float64_is_normal(a) || float64_is_zero(a); }

static inline float64 float64_set_sign(float64 a, int sign)
{ return make_float64((float64_val(a) & 0x7fffffffffffffffULL) | ((int64_t)sign << 63)); }

static inline bool float64_eq(float64 a, float64 b, float_status* s)
{ return float64_compare(a, b, s) == float_relation_equal; }

static inline bool float64_le(float64 a, float64 b, float_status* s)
{ return float64_compare(a, b, s) <= float_relation_equal; }

static inline bool float64_lt(float64 a, float64 b, float_status* s)
{ return float64_compare(a, b, s) < float_relation_equal; }

static inline bool float64_unordered(float64 a, float64 b, float_status* s)
{ return float64_compare(a, b, s) == float_relation_unordered; }

static inline bool float64_eq_quiet(float64 a, float64 b, float_status* s)
{ return float64_compare_quiet(a, b, s) == float_relation_equal; }

static inline bool float64_le_quiet(float64 a, float64 b, float_status* s)
{ return float64_compare_quiet(a, b, s) <= float_relation_equal; }

static inline bool float64_lt_quiet(float64 a, float64 b, float_status* s)
{ return float64_compare_quiet(a, b, s) < float_relation_equal; }

static inline bool float64_unordered_quiet(float64 a, float64 b, float_status* s)
{ return float64_compare_quiet(a, b, s) == float_relation_unordered; }

#define float64_zero           make_float64(0)
#define float64_half           make_float64(0x3fe0000000000000LL)
#define float64_one            make_float64(0x3ff0000000000000LL)
#define float64_one_point_five make_float64(0x3FF8000000000000ULL)
#define float64_two            make_float64(0x4000000000000000ULL)
#define float64_three          make_float64(0x4008000000000000ULL)
#define float64_ln2            make_float64(0x3fe62e42fefa39efLL)
#define float64_infinity       make_float64(0x7ff0000000000000LL)

/*----------------------------------------------------------------------------
| The pattern for a default generated double-precision NaN.
*----------------------------------------------------------------------------*/
float64 float64_default_nan(float_status* status);

/*----------------------------------------------------------------------------
| Software IEC/IEEE double-precision operations, rounding to single precision,
| returning a result in double precision, with only one rounding step.
*----------------------------------------------------------------------------*/

float64 float64r32_add(float64, float64, float_status* status);
float64 float64r32_sub(float64, float64, float_status* status);
float64 float64r32_mul(float64, float64, float_status* status);
float64 float64r32_div(float64, float64, float_status* status);
float64 float64r32_muladd(float64, float64, float64, int, float_status* status);
float64 float64r32_sqrt(float64, float_status* status);
