/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) -- ARM specifics
 *
 * Copyright (c) 2025 Mohamed Mediouni
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/*
 * Hyper-V only delivers interrupts through its GICv3, which XNU and the SEP
 * don't drive: QEMU enables it and raises each vCPU's IRQ and FIQ lines as
 * two level-sensitive SPIs routed to it. Nothing acknowledges them, so they
 * stay pending while their line is high, like a CPU's own lines. Everything
 * arrives as an IRQ, so the patched IRQ vectors ask `APPLE_VIRT_HVC_IRQ_*`
 * first, which takes the FIQ vector instead for the FIQ line, or returns
 * from the exception if it's only that and the interrupted context masks
 * FIQs, holding it off with the priority mask until an exit sees them
 * unmasked. The timers' PPIs only tell QEMU they fired, like HVF's vtimer
 * exits: their outputs go through the machine's wiring, and a PPI stays
 * disabled until an exit sees the timer stop asserting it. Hyper-V has no
 * exits for system registers or debug exceptions, so every IMPLEMENTATION
 * DEFINED register access is patched to an `hvc`, and page table stores are
 * always emulated.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/whpx.h"
#include "system/whpx_int.h"
#include "hw/arm/apple_virt_hvc.h"
#include "hw/irq.h"
#include "cpu.h"
#include "cpregs.h"
#include "internals.h"
#include "syndrome.h"
#include "emulate/aarch64.h"
#include "apple-virt.h"
#include "accel/accel-private.h"
#include "whpx_arm.h"

// The GIC sits at the top of the IPA space, out of the machine's way.
#define WHPX_GIC_SIZE    (16 * MiB)
#define WHPX_GITS_OFFSET 0x20000
#define WHPX_GICR_OFFSET 0x100000
#define WHPX_GICR_SIZE   0x20000
// The FIQ lines' priority, which the priority mask can hold off on its own.
#define WHPX_PRIO_IRQ 0x80
#define WHPX_PRIO_FIQ 0xA0

#define WHPX_INTID_PMU    23
#define WHPX_INTID_VTIMER 27
#define WHPX_INTID_PTIMER 30
// A vCPU's IRQ line, and its FIQ line after it.
#define WHPX_INTID_LINES(cpu) (32 + 2 * (cpu)->cpu_index)

#define WHPX_MAX_REGS 128

typedef struct
{
    WHV_REGISTER_NAME name;
    uint8_t           op0, op1, crn, crm, op2;
} WHPXSysReg;

// The system registers that are synced through `env`'s cpreg list.
static const WHPXSysReg whpx_sysregs[] = {
    {WHvArm64RegisterMdscrEl1, 2, 0, 0, 2, 2},       {WHvArm64RegisterSctlrEl1, 3, 0, 1, 0, 0},
    {WHvArm64RegisterCpacrEl1, 3, 0, 1, 0, 2},       {WHvArm64RegisterTtbr0El1, 3, 0, 2, 0, 0},
    {WHvArm64RegisterTtbr1El1, 3, 0, 2, 0, 1},       {WHvArm64RegisterTcrEl1, 3, 0, 2, 0, 2},
    {WHvArm64RegisterApiAKeyLoEl1, 3, 0, 2, 1, 0},   {WHvArm64RegisterApiAKeyHiEl1, 3, 0, 2, 1, 1},
    {WHvArm64RegisterApiBKeyLoEl1, 3, 0, 2, 1, 2},   {WHvArm64RegisterApiBKeyHiEl1, 3, 0, 2, 1, 3},
    {WHvArm64RegisterApdAKeyLoEl1, 3, 0, 2, 2, 0},   {WHvArm64RegisterApdAKeyHiEl1, 3, 0, 2, 2, 1},
    {WHvArm64RegisterApdBKeyLoEl1, 3, 0, 2, 2, 2},   {WHvArm64RegisterApdBKeyHiEl1, 3, 0, 2, 2, 3},
    {WHvArm64RegisterApgAKeyLoEl1, 3, 0, 2, 3, 0},   {WHvArm64RegisterApgAKeyHiEl1, 3, 0, 2, 3, 1},
    {WHvArm64RegisterSpsrEl1, 3, 0, 4, 0, 0},        {WHvArm64RegisterElrEl1, 3, 0, 4, 0, 1},
    {WHvArm64RegisterSpEl0, 3, 0, 4, 1, 0},          {WHvArm64RegisterEsrEl1, 3, 0, 5, 2, 0},
    {WHvArm64RegisterFarEl1, 3, 0, 6, 0, 0},         {WHvArm64RegisterParEl1, 3, 0, 7, 4, 0},
    {WHvArm64RegisterMairEl1, 3, 0, 10, 2, 0},       {WHvArm64RegisterVbarEl1, 3, 0, 12, 0, 0},
    {WHvArm64RegisterContextidrEl1, 3, 0, 13, 0, 1}, {WHvArm64RegisterTpidrEl1, 3, 0, 13, 0, 4},
    {WHvArm64RegisterCntkctlEl1, 3, 0, 14, 1, 0},    {WHvArm64RegisterCsselrEl1, 3, 2, 0, 0, 0},
    {WHvArm64RegisterTpidrEl0, 3, 3, 13, 0, 2},      {WHvArm64RegisterTpidrroEl0, 3, 3, 13, 0, 3},
    {WHvArm64RegisterCntvCtlEl0, 3, 3, 14, 3, 1},    {WHvArm64RegisterCntvCvalEl0, 3, 3, 14, 3, 2},
    {WHvArm64RegisterSpEl1, 3, 4, 4, 1, 0},
};

typedef struct
{
    uint64_t          kvm_id;
    WHV_REGISTER_NAME name;
} WHPXCPReg;

static hwaddr whpx_gic_base;

static uint64_t whpx_get_reg(CPUState* cpu, WHV_REGISTER_NAME name)
{
    WHV_REGISTER_VALUE value;

    assert_whpx_ok(whp.WHvGetVirtualProcessorRegisters(whpx_partition, cpu->cpu_index, &name, 1, &value));
    return value.Reg64;
}

static void whpx_set_reg(CPUState* cpu, WHV_REGISTER_NAME name, uint64_t reg)
{
    WHV_REGISTER_VALUE value = {.Reg64 = reg};

    assert_whpx_ok(whp.WHvSetVirtualProcessorRegisters(whpx_partition, cpu->cpu_index, &name, 1, &value));
}

static void whpx_flush_cpu_state(CPUState* cpu)
{
    if (cpu->vcpu_dirty) {
        whpx_arch_put_registers(cpu, cpu->accel->reset);
        cpu->vcpu_dirty   = false;
        cpu->accel->reset = false;
    }
}

// Xt of the vCPU, wherever its state is.
static uint64_t whpx_get_xreg(CPUState* cpu, int rt)
{
    if (rt == 31) { return 0; }
    if (cpu->vcpu_dirty) { return cpu_env(cpu)->xregs[rt]; }
    return whpx_get_reg(cpu, WHvArm64RegisterX0 + rt);
}

static void whpx_set_xreg(CPUState* cpu, int rt, uint64_t value)
{
    if (rt == 31) { return; }
    if (cpu->vcpu_dirty) { cpu_env(cpu)->xregs[rt] = value; }
    else {
        whpx_set_reg(cpu, WHvArm64RegisterX0 + rt, value);
    }
}

static int whpx_current_el(CPUState* cpu)
{
    if (cpu->vcpu_dirty) { return arm_current_el(cpu_env(cpu)); }
    return extract64(whpx_get_reg(cpu, WHvArm64RegisterPstate), 2, 2);
}

static void whpx_raise_exception(CPUState* cpu, uint64_t pc, uint32_t excp, uint32_t syndrome)
{
    CPUARMState* env = cpu_env(cpu);

    cpu_synchronize_state(cpu);
    env->pc                  = pc;
    cpu->exception_index     = excp;
    env->exception.target_el = 1;
    env->exception.syndrome  = syndrome;
    arm_cpu_do_interrupt(cpu);
}

void whpx_arch_get_registers(CPUState* cpu)
{
    ARMCPU*            arm_cpu = ARM_CPU(cpu);
    CPUARMState*       env     = &arm_cpu->env;
    WHV_REGISTER_NAME  names[WHPX_MAX_REGS];
    WHV_REGISTER_VALUE values[WHPX_MAX_REGS];
    uint32_t           n = 0;

    for (int i = 0; i < 31; i++) { names[n++] = WHvArm64RegisterX0 + i; }
    for (int i = 0; i < 32; i++) { names[n++] = WHvArm64RegisterQ0 + i; }
    names[n++] = WHvArm64RegisterPc;
    names[n++] = WHvArm64RegisterPstate;
    names[n++] = WHvArm64RegisterFpcr;
    names[n++] = WHvArm64RegisterFpsr;
    for (int i = 0; i < arm_cpu->cpreg_array_len; i++) { names[n++] = cpu->accel->cpreg_names[i]; }
    assert_whpx_ok(whp.WHvGetVirtualProcessorRegisters(whpx_partition, cpu->cpu_index, names, n, values));

    n = 0;
    for (int i = 0; i < 31; i++) { env->xregs[i] = values[n++].Reg64; }
    for (int i = 0; i < 32; i++) { memcpy(&env->vfp.zregs[i], &values[n++].Reg128, 16); }
    env->pc = values[n++].Reg64;
    pstate_write(env, values[n++].Reg64);
    vfp_set_fpcr(env, values[n++].Reg64);
    vfp_set_fpsr(env, values[n++].Reg64);
    for (int i = 0; i < arm_cpu->cpreg_array_len; i++) { arm_cpu->cpreg_values[i] = values[n++].Reg64; }

    apple_virt_sync_begin(cpu);
    if (!write_list_to_cpustate(arm_cpu)) { assert_not_reached(); }
    apple_virt_sync_end(cpu);

    aarch64_restore_sp(env, arm_current_el(env));
}

// Enables the interrupts that are delivered to the vCPU, which XNU doesn't do.
static void whpx_gic_reset(CPUState* cpu)
{
    WHV_ARM64_LOCAL_INTERRUPT_CONTROLLER_STATE local;
    UINT32                                     size;

    assert_whpx_ok(whp.WHvGetVirtualProcessorState(whpx_partition, cpu->cpu_index,
                                                   WHvVirtualProcessorStateTypeInterruptControllerState, &local,
                                                   sizeof(local), &size));
    local.IccIgrpen1El1 = 1;
    local.IccPmrEl1     = 0xFF;
    for (int intid = 16; intid < 32; intid++) {
        WHV_ARM64_INTERRUPT_STATE* s = &local.BankedInterruptState[intid];

        s->Enabled                  = intid == WHPX_INTID_VTIMER || intid == WHPX_INTID_PTIMER;
        s->GicrIpriorityrConfigured = WHPX_PRIO_IRQ;
    }
    assert_whpx_ok(whp.WHvSetVirtualProcessorState(
        whpx_partition, cpu->cpu_index, WHvVirtualProcessorStateTypeInterruptControllerState, &local, sizeof(local)));
    cpu->accel->timers_masked = false;
    cpu->accel->fiq_held      = false;
}

void whpx_arch_put_registers(CPUState* cpu, bool reset)
{
    ARMCPU*            arm_cpu = ARM_CPU(cpu);
    CPUARMState*       env     = &arm_cpu->env;
    WHV_REGISTER_NAME  names[WHPX_MAX_REGS];
    WHV_REGISTER_VALUE values[WHPX_MAX_REGS] = {0};
    uint32_t           n                     = 0;

    aarch64_save_sp(env, arm_current_el(env));
    apple_virt_sync_begin(cpu);
    if (!write_cpustate_to_list(arm_cpu, false)) { assert_not_reached(); }
    apple_virt_sync_end(cpu);

    for (int i = 0; i < 31; i++) {
        names[n]          = WHvArm64RegisterX0 + i;
        values[n++].Reg64 = env->xregs[i];
    }
    for (int i = 0; i < 32; i++) {
        names[n] = WHvArm64RegisterQ0 + i;
        memcpy(&values[n++].Reg128, &env->vfp.zregs[i], 16);
    }
    names[n]          = WHvArm64RegisterPc;
    values[n++].Reg64 = env->pc;
    names[n]          = WHvArm64RegisterPstate;
    values[n++].Reg64 = pstate_read(env);
    names[n]          = WHvArm64RegisterFpcr;
    values[n++].Reg64 = vfp_get_fpcr(env);
    names[n]          = WHvArm64RegisterFpsr;
    values[n++].Reg64 = vfp_get_fpsr(env);
    for (int i = 0; i < arm_cpu->cpreg_array_len; i++) {
        names[n]          = cpu->accel->cpreg_names[i];
        values[n++].Reg64 = arm_cpu->cpreg_values[i];
    }
    if (reset) {
        // A vCPU that's off is held until it's turned on, and a reset one doesn't wait for an interrupt.
        names[n]                                    = WHvRegisterInternalActivityState;
        values[n++].InternalActivity.StartupSuspend = arm_cpu->power_state == ARM_POWER_OFF;
    }
    assert_whpx_ok(whp.WHvSetVirtualProcessorRegisters(whpx_partition, cpu->cpu_index, names, n, values));

    if (reset) { whpx_gic_reset(cpu); }
}

void whpx_arm_set_irq(void* opaque, int irq, int level)
{
    CPUState*             cs   = CPU(opaque);
    CPUARMState*          env  = cpu_env(cs);
    uint32_t              mask = irq == ARM_CPU_FIQ ? CPU_INTERRUPT_FIQ : CPU_INTERRUPT_HARD;
    WHV_INTERRUPT_CONTROL ctl  = {0};

    // Only the IRQ and FIQ lines are wired.
    if (irq != ARM_CPU_IRQ && irq != ARM_CPU_FIQ) { return; }

    if (level) { qatomic_or(&env->irq_line_state, mask); }
    else {
        qatomic_and(&env->irq_line_state, ~mask);
    }

    ctl.InterruptControl.InterruptType = WHvArm64InterruptTypeFixed;
    ctl.InterruptControl.Asserted      = level != 0;
    ctl.RequestedVector                = WHPX_INTID_LINES(cs) + (irq == ARM_CPU_FIQ);
    assert_whpx_ok(whp.WHvRequestInterrupt(whpx_partition, &ctl, sizeof(ctl)));
}

// Routes the vCPU's lines to it.
static void whpx_gic_route(CPUState* cpu)
{
    WHV_ARM64_GLOBAL_INTERRUPT_CONTROLLER_STATE* global;
    UINT32                                       size  = 0;
    uint32_t                                     found = 0;

    whp.WHvGetVirtualProcessorState(whpx_partition, WHV_ANY_VP, WHvVirtualProcessorStateTypeGlobalInterruptState, NULL,
                                    0, &size);
    global = g_malloc0(size);
    assert_whpx_ok(whp.WHvGetVirtualProcessorState(
        whpx_partition, WHV_ANY_VP, WHvVirtualProcessorStateTypeGlobalInterruptState, global, size, &size));
    global->GicdCtlrEnableGrp1A = 1;
    for (uint32_t i = 0; i < global->NumInterrupts; i++) {
        WHV_ARM64_GLOBAL_INTERRUPT_STATE* s = &global->Interrupts[i];

        if (s->InterruptId != WHPX_INTID_LINES(cpu) && s->InterruptId != WHPX_INTID_LINES(cpu) + 1) { continue; }
        // GICD_IROUTER<n> holds the affinity, which is also the vCPU's MPIDR_EL1.
        s->TargetMpidr                  = ARM_CPU(cpu)->mp_affinity;
        s->InterruptState.Enabled       = 1;
        s->InterruptState.EdgeTriggered = 0;
        s->InterruptState.GicrIpriorityrConfigured =
            s->InterruptId == WHPX_INTID_LINES(cpu) ? WHPX_PRIO_IRQ : WHPX_PRIO_FIQ;
        ++found;
    }
    if (found != 2) {
        error_report("WHPX: the GIC has no SPIs %u and %u for CPU %d", WHPX_INTID_LINES(cpu), WHPX_INTID_LINES(cpu) + 1,
                     cpu->cpu_index);
        exit(1);
    }
    assert_whpx_ok(whp.WHvSetVirtualProcessorState(whpx_partition, WHV_ANY_VP,
                                                   WHvVirtualProcessorStateTypeGlobalInterruptState, global, size));
    g_free(global);
}

static int compare_cpreg(const void* a, const void* b)
{
    uint64_t x = ((const WHPXCPReg*)a)->kvm_id;
    uint64_t y = ((const WHPXCPReg*)b)->kvm_id;

    return x < y ? -1 : x > y;
}

int whpx_arch_init_vcpu(CPUState* cpu)
{
    ARMCPU*      arm_cpu = ARM_CPU(cpu);
    CPUARMState* env     = &arm_cpu->env;
    WHPXCPReg    list[ARRAY_SIZE(whpx_sysregs)];
    uint32_t     n = 0;

    env->aarch64 = true;
    __asm__("mrs %0, cntfrq_el0" : "=r"(arm_cpu->gt_cntfrq_hz));

    for (size_t i = 0; i < ARRAY_SIZE(whpx_sysregs); i++) {
        const WHPXSysReg* r      = &whpx_sysregs[i];
        uint64_t          kvm_id = KVMID_AA64_SYS_REG64(r->op0, r->op1, r->crn, r->crm, r->op2);

        if (ARMCPRegTable_cget(arm_cpu->cp_regs, kvm_to_cpreg_id(kvm_id)) == NULL) { continue; }
        list[n++] = (WHPXCPReg){.kvm_id = kvm_id, .name = r->name};
    }
    // cpreg tuples must be in strictly ascending order.
    qsort(list, n, sizeof(list[0]), compare_cpreg);

    arm_cpu->cpreg_indexes         = g_renew(uint64_t, arm_cpu->cpreg_indexes, n);
    arm_cpu->cpreg_values          = g_renew(uint64_t, arm_cpu->cpreg_values, n);
    arm_cpu->cpreg_vmstate_indexes = g_renew(uint64_t, arm_cpu->cpreg_vmstate_indexes, n);
    arm_cpu->cpreg_vmstate_values  = g_renew(uint64_t, arm_cpu->cpreg_vmstate_values, n);
    cpu->accel->cpreg_names        = g_new(WHV_REGISTER_NAME, n);
    memset(arm_cpu->cpreg_values, 0, n * sizeof(uint64_t));
    for (uint32_t i = 0; i < n; i++) {
        arm_cpu->cpreg_indexes[i]  = list[i].kvm_id;
        cpu->accel->cpreg_names[i] = list[i].name;
    }
    arm_cpu->cpreg_array_len         = n;
    arm_cpu->cpreg_vmstate_array_len = n;
    if (!write_cpustate_to_list(arm_cpu, false)) { assert_not_reached(); }

    // CP_NO_RAW registers aren't in the list.
    whpx_set_reg(cpu, WHvArm64RegisterMidrEl1, arm_cpu->midr);
    whpx_set_reg(cpu, WHvArm64RegisterMpidrEl1, arm_cpu->mp_affinity | (1ULL << 31));
    whpx_set_reg(cpu, WHvArm64RegisterGicrBaseGpa, whpx_gic_base + WHPX_GICR_OFFSET + cpu->cpu_index * WHPX_GICR_SIZE);

    whpx_gic_route(cpu);
    return 0;
}

void whpx_arch_vcpu_destroy(CPUState* cpu) { g_free(cpu->accel->cpreg_names); }

// Passes fired timers on to their outputs and disables their PPIs, re-enables the PPIs of timers that stopped firing,
// and lifts the hold on FIQs once they're unmasked.
static void whpx_sync_interrupts(CPUState* cpu)
{
    static const int ppis[]  = {[GTIMER_PHYS] = WHPX_INTID_PTIMER, [GTIMER_VIRT] = WHPX_INTID_VTIMER};
    ARMCPU*          arm_cpu = ARM_CPU(cpu);
    WHV_ARM64_LOCAL_INTERRUPT_CONTROLLER_STATE local;
    UINT32                                     size;
    bool                                       changed = false;

    assert_whpx_ok(whp.WHvGetVirtualProcessorState(whpx_partition, cpu->cpu_index,
                                                   WHvVirtualProcessorStateTypeInterruptControllerState, &local,
                                                   sizeof(local), &size));
    cpu->accel->timers_masked = false;
    for (size_t t = 0; t < ARRAY_SIZE(ppis); t++) {
        WHV_ARM64_INTERRUPT_STATE* s = &local.BankedInterruptState[ppis[t]];

        if (s->Asserted == s->Enabled) {
            s->Enabled = !s->Enabled;
            changed    = true;
            bql_lock();
            qemu_set_irq(arm_cpu->gt_timer_outputs[t], s->Asserted);
            bql_unlock();
        }
        cpu->accel->timers_masked |= !s->Enabled;
    }
    if (cpu->accel->fiq_held && !(whpx_get_reg(cpu, WHvArm64RegisterPstate) & PSTATE_F)) {
        local.IccPmrEl1      = 0xFF;
        cpu->accel->fiq_held = false;
        changed              = true;
    }
    if (changed) {
        assert_whpx_ok(whp.WHvSetVirtualProcessorState(whpx_partition, cpu->cpu_index,
                                                       WHvVirtualProcessorStateTypeInterruptControllerState, &local,
                                                       sizeof(local)));
    }
}

// Holds FIQs off with the priority mask until an exit sees them unmasked.
static void whpx_hold_fiq(CPUState* cpu)
{
    WHV_ARM64_LOCAL_INTERRUPT_CONTROLLER_STATE local;
    UINT32                                     size;

    assert_whpx_ok(whp.WHvGetVirtualProcessorState(whpx_partition, cpu->cpu_index,
                                                   WHvVirtualProcessorStateTypeInterruptControllerState, &local,
                                                   sizeof(local), &size));
    local.IccPmrEl1 = WHPX_PRIO_FIQ;
    assert_whpx_ok(whp.WHvSetVirtualProcessorState(
        whpx_partition, cpu->cpu_index, WHvVirtualProcessorStateTypeInterruptControllerState, &local, sizeof(local)));
    cpu->accel->fiq_held = true;
}

// Returns from the IRQ exception the vCPU just took.
static void whpx_return_from_irq(CPUState* cpu)
{
    CPUARMState* env = cpu_env(cpu);

    cpu_synchronize_state(cpu);
    if (!apple_virt_handle_hvc(cpu, APPLE_VIRT_HVC_ERET)) {
        aarch64_save_sp(env, arm_current_el(env));
        env->pc = env->elr_el[1];
        pstate_write(env, env->banked_spsr[BANK_SVC] & ~PSTATE_SS);
        aarch64_restore_sp(env, arm_current_el(env));
    }
    whpx_flush_cpu_state(cpu);
}

// An access to the IMPLEMENTATION DEFINED register by the `hvc` at `pc` that replaced it, as if it had trapped.
static void whpx_handle_impdef(CPUState* cpu, uint64_t pc, uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm,
                               uint32_t op2, bool read, uint32_t rt)
{
    ARMCPU*             arm_cpu = ARM_CPU(cpu);
    CPUARMState*        env     = &arm_cpu->env;
    const ARMCPRegInfo* ri =
        ARMCPRegTable_cget(arm_cpu->cp_regs, ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, crn, crm, op0, op1, op2));
    uint64_t value;

    if (ri == NULL) {
        whpx_raise_exception(cpu, pc, EXCP_UDEF, syn_uncategorized());
        return;
    }
    // Registers that are plain fields of `env` or backed by a device don't depend on the rest of the vCPU's state.
    if (ri->accessfn != NULL || (ri->type & ARM_CP_SPECIAL_MASK)
        || !(read || ri->writefn == NULL || (ri->type & ARM_CP_IO)))
    {
        cpu_synchronize_state(cpu);
    }
    if (!cp_access_ok(whpx_current_el(cpu), ri, read)
        || (ri->accessfn != NULL && ri->accessfn(env, ri, read) != CP_ACCESS_OK))
    {
        whpx_raise_exception(cpu, pc, EXCP_UDEF, syn_uncategorized());
        return;
    }

    BQL_LOCK_GUARD_IF(ri->type & ARM_CP_IO);
    if (read) {
        if (ri->type & ARM_CP_CONST) { value = ri->resetvalue; }
        else if (ri->readfn != NULL) {
            value = ri->readfn(env, ri);
        }
        else {
            value = raw_read(env, ri);
        }
        whpx_set_xreg(cpu, rt, value);
    }
    else if (!(ri->type & ARM_CP_CONST)) {
        value = whpx_get_xreg(cpu, rt);
        if (ri->writefn != NULL) { ri->writefn(env, ri, value); }
        else {
            raw_write(env, ri, value);
        }
    }
}

static int whpx_handle_hypercall(CPUState* cpu, WHV_HYPERCALL_CONTEXT* ctx)
{
    uint16_t imm = ctx->Immediate;
    uint32_t op0, op1, crn, crm, op2, line;

    // Whether the PC is past the `hvc` at the exit isn't documented.
    if (whpx_get_reg(cpu, WHvArm64RegisterPc) == ctx->Header.Pc) {
        whpx_set_reg(cpu, WHvArm64RegisterPc, ctx->Header.Pc + 4);
    }

    if (imm == APPLE_VIRT_HVC_IRQ_ENTRY || imm == APPLE_VIRT_HVC_IRQ_LOWER) {
        if (!apple_virt_handle_hvc_fast(cpu, imm)) {
            cpu_synchronize_state(cpu);
            apple_virt_handle_hvc(cpu, imm);
            whpx_flush_cpu_state(cpu);
        }
        whpx_sync_interrupts(cpu);
        line = qatomic_read(&cpu_env(cpu)->irq_line_state);
        // Unless the interrupted context masks FIQs, the FIQ slot after this one takes a pending FIQ.
        if ((line & CPU_INTERRUPT_FIQ) && !(whpx_get_reg(cpu, WHvArm64RegisterSpsrEl1) & PSTATE_F)) {
            whpx_set_reg(cpu, WHvArm64RegisterPc, ctx->Header.Pc + 0x80);
        }
        // Only a masked FIQ, or a timer that only fired.
        else if (!(line & CPU_INTERRUPT_HARD)) {
            if (line & CPU_INTERRUPT_FIQ) { whpx_hold_fiq(cpu); }
            whpx_return_from_irq(cpu);
        }
        return 0;
    }
    if (apple_virt_handle_hvc_fast(cpu, imm)) { return 0; }
    if (apple_virt_hvc_impdef(imm, &op0, &op1, &crn, &crm, &op2)) {
        whpx_handle_impdef(cpu, ctx->Header.Pc, op0, op1, crn, crm, op2, APPLE_VIRT_HVC_READ(imm),
                           APPLE_VIRT_HVC_RT(imm));
        return 0;
    }
    cpu_synchronize_state(cpu);
    if (apple_virt_handle_hvc(cpu, imm)) { return 0; }

    // The guest's own: there's no EL2 for it.
    whpx_raise_exception(cpu, ctx->Header.Pc, EXCP_UDEF, syn_uncategorized());
    return 0;
}

static int whpx_handle_memory_access(CPUState* cpu, WHV_MEMORY_ACCESS_CONTEXT* ctx)
{
    uint64_t      syndrome = ctx->Syndrome;
    bool          isv      = syndrome & ARM_EL_ISV;
    bool          iswrite  = (syndrome >> 6) & 1;
    bool          sse      = (syndrome >> 21) & 1;
    uint32_t      len      = 1 << ((syndrome >> 22) & 3);
    uint32_t      srt      = (syndrome >> 16) & 0x1F;
    bool          cm       = (syndrome >> 8) & 1;
    AddressSpace* as       = cpu_get_address_space(cpu, ARMASIdx_NS);
    uint64_t      val      = 0;

    if (syn_get_ec(syndrome) != EC_DATAABORT) {
        if (accel_private_map_page(as, ctx->Gpa, false)) { return 0; }
        error_report("WHPX: CPU %d executed unmapped memory at 0x%" PRIx64 " (0x%" PRIx64 ")", cpu->cpu_index,
                     (uint64_t)ctx->Gva, (uint64_t)ctx->Gpa);
        whpx_raise_exception(cpu, ctx->Header.Pc, EXCP_PREFETCH_ABORT, syndrome);
        return 0;
    }

    // A store to a write-protected guest page table, rather than to MMIO.
    if (iswrite && !cm && apple_virt_handle_write_fault(cpu, ctx->Gva, ctx->Gpa)) { return 0; }
    if (accel_private_map_page(as, ctx->Gpa, iswrite)) { return 0; }

    whpx_flush_cpu_state(cpu);
    if (cm) {
        // MMIO isn't cached.
    }
    else if (isv) {
        if (iswrite) {
            val = whpx_get_xreg(cpu, srt);
            if (address_space_write(as, ctx->Gpa, MEMTXATTRS_UNSPECIFIED, &val, len) != MEMTX_OK) {
                whpx_raise_exception(cpu, ctx->Header.Pc, EXCP_DATA_ABORT, syndrome);
                return 0;
            }
        }
        else {
            if (address_space_read(as, ctx->Gpa, MEMTXATTRS_UNSPECIFIED, &val, len) != MEMTX_OK) {
                whpx_raise_exception(cpu, ctx->Header.Pc, EXCP_DATA_ABORT, syndrome);
                return 0;
            }
            if (sse) { val = sextract64(val, 0, len * 8); }
            whpx_set_xreg(cpu, srt, val);
        }
    }
    else if (!arm_aarch64_fallback_emu_single(cpu, as, whpx_get_xreg, whpx_set_xreg)) {
        error_report("WHPX: can't decode the access at 0x%" PRIx64, (uint64_t)ctx->Header.Pc);
        whpx_raise_exception(cpu, ctx->Header.Pc, EXCP_DATA_ABORT, syndrome);
        return 0;
    }

    whpx_set_reg(cpu, WHvArm64RegisterPc, ctx->Header.Pc + 4);
    return 0;
}

static int whpx_handle_exit(CPUState* cpu)
{
    WHV_RUN_VP_EXIT_CONTEXT* exit = &cpu->accel->exit;

    switch (exit->ExitReason) {
        case WHvRunVpExitReasonGpaIntercept:
        case WHvRunVpExitReasonUnmappedGpa : return whpx_handle_memory_access(cpu, &exit->MemoryAccess);
        case WHvRunVpExitReasonHypercall   : return whpx_handle_hypercall(cpu, &exit->Hypercall);
        case WHvRunVpExitReasonCanceled    : return EXCP_INTERRUPT;
        case WHvRunVpExitReasonArm64Reset:
            bql_lock();
            if (exit->Arm64Reset.ResetType == WHvArm64ResetTypePowerOff) {
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            }
            else {
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }
            bql_unlock();
            return EXCP_INTERRUPT;
        default:
            error_report("WHPX: unexpected exit 0x%08x on CPU %d", exit->ExitReason, cpu->cpu_index);
            bql_lock();
            qemu_system_guest_panicked(NULL);
            bql_unlock();
            return EXCP_INTERRUPT;
    }
}

int whpx_arch_vcpu_exec(CPUState* cpu)
{
    int ret = 0;

    if (cpu->halted) {
        if (!cpu_has_work(cpu)) { return EXCP_HLT; }
        cpu->halted = 0;
    }

    bql_unlock();
    cpu_exec_start(cpu);

    do {
        whpx_flush_cpu_state(cpu);
        if (cpu->accel->timers_masked || cpu->accel->fiq_held) { whpx_sync_interrupts(cpu); }
        if (qatomic_load_acquire(&cpu->exit_request)) {
            ret = EXCP_INTERRUPT;
            break;
        }
        assert_whpx_ok(
            whp.WHvRunVirtualProcessor(whpx_partition, cpu->cpu_index, &cpu->accel->exit, sizeof(cpu->accel->exit)));
        ret = whpx_handle_exit(cpu);
    }
    while (ret == 0);

    cpu_exec_end(cpu);
    bql_lock();
    qatomic_set(&cpu->exit_request, false);
    return ret;
}

static WHV_REGISTER_NAME whpx_virt_reg_name(AppleVirtReg reg)
{
    switch (reg) {
        case APPLE_VIRT_REG_PC           : return WHvArm64RegisterPc;
        case APPLE_VIRT_REG_CPSR         : return WHvArm64RegisterPstate;
        case APPLE_VIRT_REG_SP_EL0       : return WHvArm64RegisterSpEl0;
        case APPLE_VIRT_REG_SP_EL1       : return WHvArm64RegisterSpEl1;
        case APPLE_VIRT_REG_TPIDR_EL1    : return WHvArm64RegisterTpidrEl1;
        case APPLE_VIRT_REG_VBAR_EL1     : return WHvArm64RegisterVbarEl1;
        case APPLE_VIRT_REG_SPSR_EL1     : return WHvArm64RegisterSpsrEl1;
        case APPLE_VIRT_REG_ELR_EL1      : return WHvArm64RegisterElrEl1;
        case APPLE_VIRT_REG_ESR_EL1      : return WHvArm64RegisterEsrEl1;
        case APPLE_VIRT_REG_FAR_EL1      : return WHvArm64RegisterFarEl1;
        case APPLE_VIRT_REG_SCTLR_EL1    : return WHvArm64RegisterSctlrEl1;
        case APPLE_VIRT_REG_TCR_EL1      : return WHvArm64RegisterTcrEl1;
        case APPLE_VIRT_REG_TTBR0_EL1    : return WHvArm64RegisterTtbr0El1;
        case APPLE_VIRT_REG_TTBR1_EL1    : return WHvArm64RegisterTtbr1El1;
        case APPLE_VIRT_REG_APIAKEYLO_EL1: return WHvArm64RegisterApiAKeyLoEl1;
        case APPLE_VIRT_REG_APIAKEYHI_EL1: return WHvArm64RegisterApiAKeyHiEl1;
        case APPLE_VIRT_REG_APIBKEYLO_EL1: return WHvArm64RegisterApiBKeyLoEl1;
        case APPLE_VIRT_REG_APIBKEYHI_EL1: return WHvArm64RegisterApiBKeyHiEl1;
        case APPLE_VIRT_REG_APDAKEYLO_EL1: return WHvArm64RegisterApdAKeyLoEl1;
        case APPLE_VIRT_REG_APDAKEYHI_EL1: return WHvArm64RegisterApdAKeyHiEl1;
        case APPLE_VIRT_REG_APDBKEYLO_EL1: return WHvArm64RegisterApdBKeyLoEl1;
        case APPLE_VIRT_REG_APDBKEYHI_EL1: return WHvArm64RegisterApdBKeyHiEl1;
        case APPLE_VIRT_REG_APGAKEYLO_EL1: return WHvArm64RegisterApgAKeyLoEl1;
        case APPLE_VIRT_REG_APGAKEYHI_EL1: return WHvArm64RegisterApgAKeyHiEl1;
        default                          : assert_not_reached();
    }
}

static uint64_t whpx_virt_get_xreg(CPUState* cs, int n) { return whpx_get_reg(cs, WHvArm64RegisterX0 + n); }

static void whpx_virt_set_xreg(CPUState* cs, int n, uint64_t value) { whpx_set_reg(cs, WHvArm64RegisterX0 + n, value); }

static void whpx_virt_get_qreg(CPUState* cs, int n, uint64_t value[2])
{
    WHV_REGISTER_NAME  name = WHvArm64RegisterQ0 + n;
    WHV_REGISTER_VALUE q;

    assert_whpx_ok(whp.WHvGetVirtualProcessorRegisters(whpx_partition, cs->cpu_index, &name, 1, &q));
    memcpy(value, &q.Reg128, 16);
}

static uint64_t whpx_virt_get_reg(CPUState* cs, AppleVirtReg reg) { return whpx_get_reg(cs, whpx_virt_reg_name(reg)); }

static void whpx_virt_set_reg(CPUState* cs, AppleVirtReg reg, uint64_t value)
{ whpx_set_reg(cs, whpx_virt_reg_name(reg), value); }

// Remaps the RAM at [page, page + size).
static bool whpx_virt_protect(hwaddr page, uint64_t size, bool writable)
{
    MemoryRegionSection section = memory_region_find(get_system_memory(), page, size);
    bool                ok;

    ok = section.mr != NULL && memory_region_is_ram(section.mr) && int128_get64(section.size) == size
         && SUCCEEDED(whp.WHvMapGpaRange(
             whpx_partition, (uint8_t*)memory_region_get_ram_ptr(section.mr) + section.offset_within_region, page, size,
             WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute | (writable ? WHvMapGpaRangeFlagWrite : 0)));
    if (section.mr != NULL) { memory_region_unref(section.mr); }
    return ok;
}

static void whpx_virt_map(void* host, hwaddr ipa, uint64_t size, bool exec)
{
    assert_whpx_ok(whp.WHvMapGpaRange(whpx_partition, host, ipa, size,
                                      WHvMapGpaRangeFlagRead | (exec ? WHvMapGpaRangeFlagExecute : 0)));
}

static const AppleVirtOps whpx_apple_virt_ops = {
    .get_xreg       = whpx_virt_get_xreg,
    .set_xreg       = whpx_virt_set_xreg,
    .get_qreg       = whpx_virt_get_qreg,
    .get_reg        = whpx_virt_get_reg,
    .set_reg        = whpx_virt_set_reg,
    .protect        = whpx_virt_protect,
    .map            = whpx_virt_map,
    .gic_interrupts = true,
};

static HRESULT whpx_set_ic(WHV_PARTITION_HANDLE partition)
{
    WHV_PARTITION_PROPERTY prop = {0};

    prop.Arm64IcParameters.EmulationMode                                      = WHvArm64IcEmulationModeGicV3;
    prop.Arm64IcParameters.GicV3Parameters.GicdBaseAddress                    = whpx_gic_base;
    prop.Arm64IcParameters.GicV3Parameters.GitsTranslaterBaseAddress          = whpx_gic_base + WHPX_GITS_OFFSET;
    prop.Arm64IcParameters.GicV3Parameters.GicPpiOverflowInterruptFromCntv    = WHPX_INTID_VTIMER;
    prop.Arm64IcParameters.GicV3Parameters.GicPpiPerformanceMonitorsInterrupt = WHPX_INTID_PMU;
    return whp.WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeArm64IcParameters, &prop, sizeof(prop));
}

int whpx_arch_configure(MachineState* ms)
{
    WHV_CAPABILITY cap = {0};

    assert_whpx_ok(whp.WHvGetCapability(WHvCapabilityCodePhysicalAddressWidth, &cap, sizeof(cap), NULL));
    whpx_gic_base = (1ULL << cap.PhysicalAddressWidth) - WHPX_GIC_SIZE;
    assert_whpx_ok(whpx_set_ic(whpx_partition));
    return 0;
}

// The host's ID registers, as a vCPU of a scratch partition sees them.
static bool whpx_arm_get_host_isar(ARMISARegisters* isar)
{
    static const struct
    {
        WHV_REGISTER_NAME name;
        ARMIDRegisterIdx  index;
    } regs[] = {
        {WHvArm64RegisterIdAa64Pfr0El1, ID_AA64PFR0_EL1_IDX},   {WHvArm64RegisterIdAa64Pfr1El1, ID_AA64PFR1_EL1_IDX},
        {WHvArm64RegisterIdAa64Dfr0El1, ID_AA64DFR0_EL1_IDX},   {WHvArm64RegisterIdAa64Dfr1El1, ID_AA64DFR1_EL1_IDX},
        {WHvArm64RegisterIdAa64Isar0El1, ID_AA64ISAR0_EL1_IDX}, {WHvArm64RegisterIdAa64Isar1El1, ID_AA64ISAR1_EL1_IDX},
        {WHvArm64RegisterIdAa64Isar2El1, ID_AA64ISAR2_EL1_IDX}, {WHvArm64RegisterIdAa64Mmfr0El1, ID_AA64MMFR0_EL1_IDX},
        {WHvArm64RegisterIdAa64Mmfr1El1, ID_AA64MMFR1_EL1_IDX}, {WHvArm64RegisterIdAa64Mmfr2El1, ID_AA64MMFR2_EL1_IDX},
        {WHvArm64RegisterIdAa64Mmfr3El1, ID_AA64MMFR3_EL1_IDX},
    };
    WHV_PARTITION_HANDLE   partition;
    WHV_PARTITION_PROPERTY prop = {.ProcessorCount = 1};
    WHV_REGISTER_NAME      names[ARRAY_SIZE(regs)];
    WHV_REGISTER_VALUE     values[ARRAY_SIZE(regs)];
    bool                   ok;

    for (size_t i = 0; i < ARRAY_SIZE(regs); i++) { names[i] = regs[i].name; }

    if (FAILED(whp.WHvCreatePartition(&partition))) { return false; }
    ok = SUCCEEDED(whp.WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeProcessorCount, &prop, sizeof(prop)))
         && SUCCEEDED(whpx_set_ic(partition)) && SUCCEEDED(whp.WHvSetupPartition(partition))
         && SUCCEEDED(whp.WHvCreateVirtualProcessor(partition, 0, 0));
    if (ok) {
        ok = SUCCEEDED(whp.WHvGetVirtualProcessorRegisters(partition, 0, names, ARRAY_SIZE(regs), values));
        whp.WHvDeleteVirtualProcessor(partition, 0);
    }
    whp.WHvDeletePartition(partition);
    if (!ok) { return false; }

    memset(isar, 0, sizeof(*isar));
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++) { isar->idregs[regs[i].index] = values[i].Reg64; }
    // Only the FP/SIMD registers are synced.
    SET_IDREG(isar, ID_AA64PFR0, GET_IDREG(isar, ID_AA64PFR0) & ~R_ID_AA64PFR0_SVE_MASK);
    SET_IDREG(isar, ID_AA64PFR1, GET_IDREG(isar, ID_AA64PFR1) & ~R_ID_AA64PFR1_SME_MASK);
    return true;
}

void whpx_arm_set_cpu_features_from_host(ARMCPU* cpu)
{
    static ARMISARegisters isar;
    static bool            valid;

    if (!valid) {
        if (!whpx_enabled() || !whpx_arm_get_host_isar(&isar)) {
            cpu->host_cpu_probe_failed = true;
            return;
        }
        valid = true;
    }

    cpu->isar         = isar;
    cpu->env.features = (1ULL << ARM_FEATURE_V8) | (1ULL << ARM_FEATURE_NEON) | (1ULL << ARM_FEATURE_AARCH64)
                        | (1ULL << ARM_FEATURE_PMU) | (1ULL << ARM_FEATURE_GENERIC_TIMER);
    // SCTLR_EL1's RES1 bits, and SPAN.
    cpu->reset_sctlr = 0x30900180;
}

int whpx_arch_init(MachineState* ms)
{
    // The GXF shadow pool goes below the GIC.
    apple_virt_set_ops(&whpx_apple_virt_ops, whpx_gic_base);
    return 0;
}
