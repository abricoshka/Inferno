/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Host FPCR/FPSR synchronisation for native guest FP execution.
 */

#pragma once

#include "cpu.h"

#if defined(__aarch64__) && defined(CONFIG_TCG)
/* Not the trap enables, and not FEAT_AFP, which falls back to softfloat. */
    #define ARM_HOST_FPCR_MASK (FPCR_RMODE_MASK | FPCR_FZ | FPCR_DN | FPCR_AHP | FPCR_FZ16)

static inline uint64_t arm_host_fpcr_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}

static inline void arm_host_fpcr_write(uint64_t v) { __asm__ volatile("msr fpcr, %0" ::"r"(v) : "memory"); }

static inline uint64_t arm_host_fpsr_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpsr" : "=r"(v));
    return v;
}

static inline void arm_host_fpsr_write(uint64_t v) { __asm__ volatile("msr fpsr, %0" ::"r"(v) : "memory"); }

static inline void arm_host_fpcr_enter(CPUARMState* env)
{
    uint64_t want = env->vfp.fpcr & ARM_HOST_FPCR_MASK;
    uint64_t cur  = arm_host_fpcr_read();

    env->host_fpcr_saved = cur;
    env->host_fpsr_saved = arm_host_fpsr_read();
    env->host_fp_active  = true;

    if (cur != want) { arm_host_fpcr_write(want); }
    if (env->host_fpsr_saved) { arm_host_fpsr_write(0); }
}

/* Must not change state: the cpreg reset check reads FPSR twice. */
static inline uint64_t arm_host_fpsr_peek(CPUARMState* env)
{
    if (!env->host_fp_active) { return 0; }
    return arm_host_fpsr_read() & (FPSR_CEXC_MASK | FPSR_QC);
}

static inline void arm_host_fpsr_drain(CPUARMState* env)
{
    uint64_t cur;

    if (!env->host_fp_active) { return; }

    cur = arm_host_fpsr_read();
    if (!cur) { return; }

    env->vfp.fpsr |= cur & FPSR_CEXC_MASK;
    if (cur & FPSR_QC) { env->vfp.qc[0] = 1; }
    arm_host_fpsr_write(0);
}

static inline void arm_host_fpsr_ack(CPUARMState* env)
{
    if (!env->host_fp_active) { return; }
    arm_host_fpsr_write(0);
}

static inline void arm_host_fpcr_reload(CPUARMState* env)
{
    uint64_t want = env->vfp.fpcr & ARM_HOST_FPCR_MASK;

    if (!env->host_fp_active) { return; }
    if (arm_host_fpcr_read() != want) { arm_host_fpcr_write(want); }
}

static inline void arm_host_fpcr_exit(CPUARMState* env)
{
    uint64_t cur;

    if (!env->host_fp_active) { return; }
    cur = arm_host_fpcr_read();
    arm_host_fpsr_drain(env);

    if (cur != env->host_fpcr_saved) { arm_host_fpcr_write(env->host_fpcr_saved); }
    arm_host_fpsr_write(env->host_fpsr_saved);
    env->host_fp_active = false;
}
#elif defined(__x86_64__) && defined(CONFIG_TCG)
/* Only the rounding mode follows the guest; exceptions masked, no flush-to-zero. */
    #define ARM_HOST_MXCSR_BASE  0x1f80
    #define ARM_HOST_MXCSR_FLAGS 0x3f

static inline uint32_t arm_host_mxcsr_read(void)
{
    uint32_t v;
    __asm__ volatile("stmxcsr %0" : "=m"(v));
    return v;
}

static inline void arm_host_mxcsr_write(uint32_t v) { __asm__ volatile("ldmxcsr %0" ::"m"(v) : "memory"); }

static inline uint32_t arm_host_mxcsr_rounding(CPUARMState* env)
{
    switch (extract64(env->vfp.fpcr, 22, 2)) {
        case FPROUNDING_TIEEVEN: return 0 << 13;
        case FPROUNDING_NEGINF : return 1 << 13;
        case FPROUNDING_POSINF : return 2 << 13;
        default                : return 3 << 13;
    }
}

static inline uint64_t arm_host_mxcsr_to_fpsr(uint32_t m)
{
    return (m & (1 << 0) ? FPSR_IOC : 0) | (m & (1 << 2) ? FPSR_DZC : 0) | (m & (1 << 3) ? FPSR_OFC : 0)
           | (m & (1 << 4) ? FPSR_UFC : 0) | (m & (1 << 5) ? FPSR_IXC : 0);
}

static inline void arm_host_fpcr_enter(CPUARMState* env)
{
    uint32_t want = ARM_HOST_MXCSR_BASE | arm_host_mxcsr_rounding(env);

    env->host_fpcr_saved = arm_host_mxcsr_read();
    env->host_fp_active  = true;
    if (env->host_fpcr_saved != want) { arm_host_mxcsr_write(want); }
}

static inline uint64_t arm_host_fpsr_peek(CPUARMState* env)
{
    if (!env->host_fp_active) { return 0; }
    return arm_host_mxcsr_to_fpsr(arm_host_mxcsr_read());
}

static inline void arm_host_fpsr_drain(CPUARMState* env)
{
    uint32_t cur;

    if (!env->host_fp_active) { return; }

    cur = arm_host_mxcsr_read();
    if (!(cur & ARM_HOST_MXCSR_FLAGS)) { return; }

    env->vfp.fpsr |= arm_host_mxcsr_to_fpsr(cur);
    arm_host_mxcsr_write(cur & ~ARM_HOST_MXCSR_FLAGS);
}

static inline void arm_host_fpsr_ack(CPUARMState* env)
{
    uint32_t cur;

    if (!env->host_fp_active) { return; }

    cur = arm_host_mxcsr_read();
    if (cur & ARM_HOST_MXCSR_FLAGS) { arm_host_mxcsr_write(cur & ~ARM_HOST_MXCSR_FLAGS); }
}

static inline void arm_host_fpcr_reload(CPUARMState* env)
{
    uint32_t cur, want;

    if (!env->host_fp_active) { return; }

    cur  = arm_host_mxcsr_read();
    want = (cur & ARM_HOST_MXCSR_FLAGS) | ARM_HOST_MXCSR_BASE | arm_host_mxcsr_rounding(env);
    if (cur != want) { arm_host_mxcsr_write(want); }
}

static inline void arm_host_fpcr_exit(CPUARMState* env)
{
    if (!env->host_fp_active) { return; }
    arm_host_fpsr_drain(env);
    arm_host_mxcsr_write(env->host_fpcr_saved);
    env->host_fp_active = false;
}
#else
static inline void arm_host_fpcr_enter(CPUARMState* env) { }

static inline void arm_host_fpcr_exit(CPUARMState* env) { }

static inline uint64_t arm_host_fpsr_peek(CPUARMState* env) { return 0; }

static inline void arm_host_fpsr_drain(CPUARMState* env) { }

static inline void arm_host_fpsr_ack(CPUARMState* env) { }

static inline void arm_host_fpcr_reload(CPUARMState* env) { }
#endif
