/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Target dependent generic vector operation expansion
 *
 * Copyright (c) 2018 Linaro
 */

#pragma once

#include "tcg/tcg-op-gvec-common.h"

#ifndef TARGET_LONG_BITS
    #error must include QEMU headers
#endif

#if TARGET_LONG_BITS == 64
    #define tcg_gen_gvec_dup_tl tcg_gen_gvec_dup_i64
#elif TARGET_LONG_BITS == 32
    #define tcg_gen_gvec_dup_tl tcg_gen_gvec_dup_i32
#else
    #error
#endif
