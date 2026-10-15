/*
 * Apple GXF/SPRR and PAC emulation for hardware accelerators: `hvc` ABI.
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

/*
 * Hardware accelerators can't apply SPRR or Apple's PAC keys or trap GXF, so
 * the GXF transitions, GXF/SPRR/MMU/PAC register accesses, ASID invalidations,
 * lower-EL exception entries and exception returns of the monitors and the
 * kernel are rewritten to `hvc` with this immediate (after m1n1):
 *   [15]    0 = control, 1 = system register access
 *   control:
 *     0x1420 | imm5   genter
 *     0x1400          gexit
 *     0x1300          end of the hypervisor's TLB flush stub
 *     0x1320 | Xt     tlbi aside1{,is,os}{,nxs}, Xt
 *     0x1340          exception entry from a lower EL, the vector's first instruction
 *     0x1341          eret
 *   system register:
 *     [14:6]  AppleVirtVReg
 *     [5]     1 = mrs, 0 = msr
 *     [4:0]   Xt
 */
#define APPLE_VIRT_HVC_SYSREG         (1 << 15)
#define APPLE_VIRT_HVC_VREG(imm)      (((imm) >> 6) & 0x1FF)
#define APPLE_VIRT_HVC_READ(imm)      (((imm) >> 5) & 1)
#define APPLE_VIRT_HVC_RT(imm)        ((imm) & 0x1F)
#define APPLE_VIRT_HVC_GENTER         0x1420
#define APPLE_VIRT_HVC_GXF_MASK       0xFFE0
#define APPLE_VIRT_HVC_GEXIT          0x1400
#define APPLE_VIRT_HVC_RESUME         0x1300
#define APPLE_VIRT_HVC_TLBI_ASID      0x1320
#define APPLE_VIRT_HVC_TLBI_ASID_MASK 0xFFE0
#define APPLE_VIRT_HVC_LOWER_ENTRY    0x1340
#define APPLE_VIRT_HVC_ERET           0x1341

typedef enum
{
    APPLE_VIRT_VREG_SPRR_CONFIG_EL1,
    APPLE_VIRT_VREG_GXF_CONFIG_EL1,
    APPLE_VIRT_VREG_GXF_STATUS_EL1,
    APPLE_VIRT_VREG_GXF_ENTER_EL1,
    APPLE_VIRT_VREG_GXF_ABORT_EL1,
    APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0,
    APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0,
    APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1,
    APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1,
    APPLE_VIRT_VREG_ASPSR_GL11,
    APPLE_VIRT_VREG_SP_GL11,
    APPLE_VIRT_VREG_TPIDR_GL11,
    APPLE_VIRT_VREG_VBAR_GL11,
    APPLE_VIRT_VREG_SPSR_GL11,
    APPLE_VIRT_VREG_ESR_GL11,
    APPLE_VIRT_VREG_ELR_GL11,
    APPLE_VIRT_VREG_FAR_GL11,
    APPLE_VIRT_VREG_TTBR0_EL1,
    APPLE_VIRT_VREG_TTBR1_EL1,
    APPLE_VIRT_VREG_TCR_EL1,
    APPLE_VIRT_VREG_SCTLR_EL1,
    APPLE_VIRT_VREG_SP_EL1,
    APPLE_VIRT_VREG_APIAKEYLO_EL1,
    APPLE_VIRT_VREG_APIAKEYHI_EL1,
    APPLE_VIRT_VREG_APIBKEYLO_EL1,
    APPLE_VIRT_VREG_APIBKEYHI_EL1,
    APPLE_VIRT_VREG_APDAKEYLO_EL1,
    APPLE_VIRT_VREG_APDAKEYHI_EL1,
    APPLE_VIRT_VREG_APDBKEYLO_EL1,
    APPLE_VIRT_VREG_APDBKEYHI_EL1,
    APPLE_VIRT_VREG_APGAKEYLO_EL1,
    APPLE_VIRT_VREG_APGAKEYHI_EL1,
    // Not rewritten by the patcher, as they trap anyway.
    APPLE_VIRT_VREG_APCTL_EL1,
    APPLE_VIRT_VREG_KERNKEYLO_EL1,
    APPLE_VIRT_VREG_KERNKEYHI_EL1,
    APPLE_VIRT_VREG_MAX,
} AppleVirtVReg;

#define APPLE_VIRT_SYSREG(op0, op1, crn, crm, op2) (((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

/// The `AppleVirtVReg` of `S<op0>_<op1>_C<crn>_C<crm>_<op2>`, or -1 if GXF/PAC emulation doesn't handle it.
static inline int apple_virt_vreg(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2)
{
    switch (APPLE_VIRT_SYSREG(op0, op1, crn, crm, op2)) {
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 0): return APPLE_VIRT_VREG_SPRR_CONFIG_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 2): return APPLE_VIRT_VREG_GXF_CONFIG_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 8, 0): return APPLE_VIRT_VREG_GXF_STATUS_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 8, 1): return APPLE_VIRT_VREG_GXF_ENTER_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 8, 2): return APPLE_VIRT_VREG_GXF_ABORT_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 5): return APPLE_VIRT_VREG_SPRR_UPERM_BR0_EL0;
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 6): return APPLE_VIRT_VREG_SPRR_UPERM_BR1_EL0;
        case APPLE_VIRT_SYSREG(3, 6, 15, 1, 7): return APPLE_VIRT_VREG_SPRR_PPERM_BR0_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 3, 0): return APPLE_VIRT_VREG_SPRR_PPERM_BR1_EL1;
        case APPLE_VIRT_SYSREG(3, 6, 15, 8, 3): return APPLE_VIRT_VREG_ASPSR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 0): return APPLE_VIRT_VREG_SP_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 1): return APPLE_VIRT_VREG_TPIDR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 2): return APPLE_VIRT_VREG_VBAR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 3): return APPLE_VIRT_VREG_SPSR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 5): return APPLE_VIRT_VREG_ESR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 6): return APPLE_VIRT_VREG_ELR_GL11;
        case APPLE_VIRT_SYSREG(3, 6, 15, 9, 7): return APPLE_VIRT_VREG_FAR_GL11;
        case APPLE_VIRT_SYSREG(3, 0, 2, 0, 0) : return APPLE_VIRT_VREG_TTBR0_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 0, 1) : return APPLE_VIRT_VREG_TTBR1_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 0, 2) : return APPLE_VIRT_VREG_TCR_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 1, 0, 0) : return APPLE_VIRT_VREG_SCTLR_EL1;
        case APPLE_VIRT_SYSREG(3, 4, 4, 1, 0) : return APPLE_VIRT_VREG_SP_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 1, 0) : return APPLE_VIRT_VREG_APIAKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 1, 1) : return APPLE_VIRT_VREG_APIAKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 1, 2) : return APPLE_VIRT_VREG_APIBKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 1, 3) : return APPLE_VIRT_VREG_APIBKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 2, 0) : return APPLE_VIRT_VREG_APDAKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 2, 1) : return APPLE_VIRT_VREG_APDAKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 2, 2) : return APPLE_VIRT_VREG_APDBKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 2, 3) : return APPLE_VIRT_VREG_APDBKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 3, 0) : return APPLE_VIRT_VREG_APGAKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 0, 2, 3, 1) : return APPLE_VIRT_VREG_APGAKEYHI_EL1;
        case APPLE_VIRT_SYSREG(3, 4, 15, 0, 4): return APPLE_VIRT_VREG_APCTL_EL1;
        case APPLE_VIRT_SYSREG(3, 4, 15, 1, 0): return APPLE_VIRT_VREG_KERNKEYLO_EL1;
        case APPLE_VIRT_SYSREG(3, 4, 15, 1, 1): return APPLE_VIRT_VREG_KERNKEYHI_EL1;
        default                               : return -1;
    }
}
