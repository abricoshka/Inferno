/*
 * Apple A13 GXF.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
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

#include "qemu/osdep.h"
#include "exec/cputlb.h"
#include "hw/arm/a13_gxf.h"
#include "target/arm/cpregs.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"

static CPAccessResult access_gxf(CPUARMState* env, const ARMCPRegInfo* ri, bool isread)
{
    if (arm_is_guarded(env)) { return CP_ACCESS_OK; }
    return CP_ACCESS_TRAP_EL1;
}

static uint64_t tpidr_el1_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    if (arm_is_guarded(env)) { return env->gxf.tpidr_gl[1]; }
    return env->cp15.tpidr_el[1];
}

static void tpidr_el1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    if (arm_is_guarded(env)) { env->gxf.tpidr_gl[1] = value; }
    else {
        env->cp15.tpidr_el[1] = value;
    }
}

static uint64_t vbar_el1_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    if (arm_is_guarded(env)) { return env->gxf.vbar_gl[1]; }
    return raw_read(env, ri);
}

static void vbar_el1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    if (arm_is_guarded(env)) { env->gxf.vbar_gl[1] = value & ~0x1FULL; }
    else if (!(env->cp15.vmsa_lock_el1 & VMSA_LOCK_VBAR_EL1)) {
        raw_write(env, ri, value & ~0x1FULL);
    }
}

static uint64_t spsr_el1_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    if (arm_is_guarded(env)) { return env->gxf.spsr_gl[1]; }
    return env->banked_spsr[BANK_SVC];
}

static void spsr_el1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    if (arm_is_guarded(env)) { env->gxf.spsr_gl[1] = value; }
    else {
        env->banked_spsr[BANK_SVC] = value;
    }
}

static uint64_t elr_el1_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    if (arm_is_guarded(env)) { return env->gxf.elr_gl[1]; }
    return env->elr_el[1];
}

static void elr_el1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    if (arm_is_guarded(env)) { env->gxf.elr_gl[1] = value; }
    else {
        env->elr_el[1] = value;
    }
}

static uint64_t esr_el1_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    if (arm_is_guarded(env)) { return env->gxf.esr_gl[1]; }
    return env->cp15.esr_el[1];
}

static void esr_el1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    if (arm_is_guarded(env)) { env->gxf.esr_gl[1] = value; }
    else {
        env->cp15.esr_el[1] = value;
    }
}

static uint64_t far_el1_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    if (arm_is_guarded(env)) { return env->gxf.far_gl[1]; }
    return env->cp15.far_el[1];
}

static void far_el1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    if (arm_is_guarded(env)) { env->gxf.far_gl[1] = value; }
    else {
        env->cp15.far_el[1] = value;
    }
}

#define SPRR_EL0_IDXMASK (ARMMMUIdxBit_E10_0)
#define SPRR_EL1_IDXMASK (ARMMMUIdxBit_E10_1 | ARMMMUIdxBit_E10_1_PAN | ARMMMUIdxBit_GE10_1 | ARMMMUIdxBit_GE10_1_PAN)

static void sprr_config_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    if (raw_read(env, ri) == value) { return; }
    raw_write(env, ri, value);
    tlb_flush_by_mmuidx(env_cpu(env), SPRR_EL0_IDXMASK | SPRR_EL1_IDXMASK);
}

static uint32_t sprr_changed_indices(uint64_t old_perm, uint64_t new_perm)
{
    uint64_t diff    = old_perm ^ new_perm;
    uint32_t changed = 0;

    for (int i = 0; i < 16; i++) {
        if (extract64(diff, i * 4, 4)) { changed |= 1U << i; }
    }
    return changed;
}

static bool sprr_tlb_entry_is_stale(const CPUTLBEntryFull* full, uint64_t changed)
{
    uint8_t key = full->extra.arm.sprr;
    return (key & ARM_SPRR_TLB_VALID) && extract64(changed, key & ~ARM_SPRR_TLB_VALID, 1);
}

static void sprr_perm_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value, MMUIdxMap idxmap, int select)
{
    uint32_t changed = sprr_changed_indices(raw_read(env, ri), value);

    if (changed == 0) { return; }
    raw_write(env, ri, value);
    tlb_flush_matching_by_mmuidx(env_cpu(env), idxmap, sprr_tlb_entry_is_stale, (uint64_t)changed << (select * 16));
}

static void sprr_uperm_br0_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    uint64_t mask = 0;

    if (arm_current_el(env)) {
        sprr_perm_write(env, ri, value, SPRR_EL0_IDXMASK, 0);
        return;
    }

    for (int i = 0; i < 16; i++) { mask |= extract64(env->sprr.sprr_umprr_br[0], i * 2, 2) << (i * 4); }

    sprr_perm_write(env, ri, (raw_read(env, ri) & ~mask) | (value & mask), SPRR_EL0_IDXMASK, 0);
}

static void sprr_uperm_br1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{ sprr_perm_write(env, ri, value, SPRR_EL0_IDXMASK, 1); }

static void sprr_pperm_br0_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{ sprr_perm_write(env, ri, value, SPRR_EL1_IDXMASK, 0); }

static void sprr_pperm_br1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{ sprr_perm_write(env, ri, value, SPRR_EL1_IDXMASK, 1); }

static uint64_t gxf_cpreg_raw_read(CPUARMState* env, const ARMCPRegInfo* ri)
{ return *(uint64_t*)((char*)(env) + (ri)->bank_fieldoffsets[0]); }

static void gxf_cpreg_raw_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{ *(uint64_t*)((char*)(env) + (ri)->bank_fieldoffsets[0]) = value; }

static const ARMCPRegInfo apple_a13_gxf_cp_override_reginfo[] = {
    {
        .name        = "TPIDR_EL1",
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 0,
        .opc2        = 4,
        .crn         = 13,
        .crm         = 0,
        .access      = PL1_RW,
        .type        = ARM_CP_OVERRIDE,
        .readfn      = tpidr_el1_read,
        .writefn     = tpidr_el1_write,
        .raw_readfn  = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, cp15.tpidr_el[1]),
        .resetvalue  = 0,
    },
    {
        .name              = "VBAR",
        .state             = ARM_CP_STATE_BOTH,
        .opc0              = 3,
        .crn               = 12,
        .crm               = 0,
        .opc1              = 0,
        .opc2              = 0,
        .access            = PL1_RW,
        .type              = ARM_CP_OVERRIDE,
        .readfn            = vbar_el1_read,
        .writefn           = vbar_el1_write,
        .raw_readfn        = raw_read,
        .raw_writefn       = raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, cp15.vbar_s), offsetof(CPUARMState, cp15.vbar_ns)},
        .resetvalue        = 0,
    },
    {
        .name        = "SPSR_EL1",
        .state       = ARM_CP_STATE_AA64,
        .type        = ARM_CP_ALIAS | ARM_CP_OVERRIDE,
        .opc0        = 3,
        .opc1        = 0,
        .crn         = 4,
        .crm         = 0,
        .opc2        = 0,
        .access      = PL1_RW,
        .raw_readfn  = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, banked_spsr[BANK_SVC]),
        .readfn      = spsr_el1_read,
        .writefn     = spsr_el1_write,
        .resetvalue  = 0,
    },
    {
        .name        = "ELR_EL1",
        .state       = ARM_CP_STATE_AA64,
        .type        = ARM_CP_ALIAS | ARM_CP_OVERRIDE,
        .opc0        = 3,
        .opc1        = 0,
        .crn         = 4,
        .crm         = 0,
        .opc2        = 1,
        .access      = PL1_RW,
        .raw_readfn  = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, elr_el[1]),
        .readfn      = elr_el1_read,
        .writefn     = elr_el1_write,
        .resetvalue  = 0,
    },
    {
        .name        = "ESR_EL1",
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .crn         = 5,
        .crm         = 2,
        .opc1        = 0,
        .opc2        = 0,
        .access      = PL1_RW,
        .type        = ARM_CP_OVERRIDE,
        .accessfn    = access_tvm_trvm,
        .raw_readfn  = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, cp15.esr_el[1]),
        .readfn      = esr_el1_read,
        .writefn     = esr_el1_write,
        .resetvalue  = 0,
    },
    {
        .name        = "FAR_EL1",
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .crn         = 6,
        .crm         = 0,
        .opc1        = 0,
        .opc2        = 0,
        .type        = ARM_CP_OVERRIDE,
        .access      = PL1_RW,
        .accessfn    = access_tvm_trvm,
        .raw_readfn  = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, cp15.far_el[1]),
        .readfn      = far_el1_read,
        .writefn     = far_el1_write,
        .resetvalue  = 0,
    },
};

static const ARMCPRegInfo apple_a13_gxf_cp_reginfo[] = {
    {
        .name        = "GXF_CONFIG_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 2,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_config_el[1]),
    },
    {
        .name        = "GXF_STATUS_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 8,
        .opc2        = 0,
        .access      = PL1_R,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_status_el[1]),
    },
    {
        .name        = "GXF_ENTER_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 8,
        .opc2        = 1,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_enter_el[1]),
    },
    {
        .name        = "GXF_ABORT_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 8,
        .opc2        = 2,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_abort_el[1]),
    },
    {
        .name        = "ASPSR_GL11",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 8,
        .opc2        = 3,
        .access      = PL1_RW,
        .accessfn    = access_gxf,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.aspsr_gl[1]),
    },
    {
        .name              = "SP_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 0,
        .access            = PL2_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.sp_gl[1]), offsetof(CPUARMState, sp_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "TPIDR_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 1,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.tpidr_gl[1]), offsetof(CPUARMState, cp15.tpidr_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "VBAR_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 2,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.vbar_gl[1]), offsetof(CPUARMState, cp15.vbar_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "SPSR_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 3,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.spsr_gl[1]), offsetof(CPUARMState, banked_spsr[BANK_SVC])},
        .resetvalue        = 0,
    },
    {
        .name              = "ESR_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 5,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.esr_gl[1]), offsetof(CPUARMState, cp15.esr_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "ELR_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 6,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.elr_gl[1]), offsetof(CPUARMState, elr_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "FAR_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 7,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.far_gl[1]), offsetof(CPUARMState, cp15.far_el[1])},
        .resetvalue        = 0,
    },
    // TODO: Implement lockdown for these registers to prevent unexpected
    // changes
    {
        .name        = "SPRR_CONFIG_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 0,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .writefn     = sprr_config_write,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_config_el1),
    },
    {
        .name        = "SPRR_GTR_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 1,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_gtr_el1),
    },
    {
        .name        = "SPRR_UPERM_BR0_EL0",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 5,
        .access      = PL0_RW,
        .resetvalue  = 0,
        .writefn     = sprr_uperm_br0_write,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_uperm_br[0]),
    },
    {
        .name        = "SPRR_UPERM_BR1_EL0",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 6,
        .access      = PL1_RW | PL0_R,
        .resetvalue  = 0,
        .writefn     = sprr_uperm_br1_write,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_uperm_br[1]),
    },
    {
        .name        = "SPRR_PPERM_BR0_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 7,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .writefn     = sprr_pperm_br0_write,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_pperm_br[0]),
    },
    {
        .name        = "SPRR_PPERM_BR1_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 0,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .writefn     = sprr_pperm_br1_write,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_pperm_br[1]),
    },
    {
        .name        = "SPRR_UMPRR_BR0_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 1,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_umprr_br[0]),
    },
    {
        .name        = "SPRR_UMPRR_BR1_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 2,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_umprr_br[1]),
    },
    {
        .name        = "SPRR_PMPRR_BR0_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 3,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_pmprr_br[0]),
    },
    {
        .name        = "SPRR_PMPRR_BR1_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 4,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_pmprr_br[1]),
    },
};

void apple_a13_init_gxf_override(AppleA13State* cpu)
{
    if (arm_feature(&cpu->parent_obj.env, ARM_FEATURE_GXF)) {
        define_arm_cp_regs(&cpu->parent_obj, apple_a13_gxf_cp_override_reginfo);
    }
}

void apple_a13_init_gxf(AppleA13State* cpu)
{
    if (arm_feature(&cpu->parent_obj.env, ARM_FEATURE_GXF)) {
        define_arm_cp_regs(&cpu->parent_obj, apple_a13_gxf_cp_reginfo);
    }
}
