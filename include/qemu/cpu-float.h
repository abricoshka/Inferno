#pragma once

#include "fpu/softfloat-types.h"

/* Unions for reinterpreting between floats and integers.  */

typedef union
{
    float32  f;
    uint32_t l;
} CPU_FloatU;

typedef union
{
    float64 d;
#if HOST_BIG_ENDIAN
    struct
    {
        uint32_t upper;
        uint32_t lower;
    } l;
#else
    struct
    {
        uint32_t lower;
        uint32_t upper;
    } l;
#endif
    uint64_t ll;
} CPU_DoubleU;
