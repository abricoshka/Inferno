/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2017
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/main-loop.h"
#include "accel/accel-cpu-ops.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "system/cpus.h"
#include "system/whpx.h"
#include "system/whpx_int.h"

static void do_whpx_cpu_synchronize_state(CPUState* cpu, run_on_cpu_data arg)
{
    if (!cpu->vcpu_dirty) {
        whpx_arch_get_registers(cpu);
        cpu->vcpu_dirty = true;
    }
}

static void whpx_cpu_synchronize_state(CPUState* cpu)
{
    if (!cpu->vcpu_dirty) { run_on_cpu(cpu, do_whpx_cpu_synchronize_state, RUN_ON_CPU_NULL); }
}

static void do_whpx_cpu_synchronize_post_reset(CPUState* cpu, run_on_cpu_data arg)
{
    // QEMU's state is the reference; it's put on the next entry.
    cpu->vcpu_dirty   = true;
    cpu->accel->reset = true;
}

static void whpx_cpu_synchronize_post_reset(CPUState* cpu)
{ run_on_cpu(cpu, do_whpx_cpu_synchronize_post_reset, RUN_ON_CPU_NULL); }

static void whpx_kick_vcpu_thread(CPUState* cpu)
{ whp.WHvCancelRunVirtualProcessor(whpx_partition, cpu->cpu_index, 0); }

static void* whpx_cpu_thread_fn(void* arg)
{
    CPUState* cpu = arg;
    int       r;

    rcu_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    current_cpu    = cpu;

    cpu->accel        = g_new0(AccelCPUState, 1);
    cpu->vcpu_dirty   = true;
    cpu->accel->reset = true;
    assert_whpx_ok(whp.WHvCreateVirtualProcessor(whpx_partition, cpu->cpu_index, 0));
    whpx_arch_init_vcpu(cpu);

    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        qemu_process_cpu_events(cpu);
        if (cpu_can_run(cpu)) {
            r = whpx_arch_vcpu_exec(cpu);
            if (r == EXCP_DEBUG) { cpu_handle_guest_debug(cpu); }
        }
    }
    while (!cpu->unplug || cpu_can_run(cpu));

    whpx_arch_vcpu_destroy(cpu);
    assert_whpx_ok(whp.WHvDeleteVirtualProcessor(whpx_partition, cpu->cpu_index));
    g_free(cpu->accel);
    cpu->accel = NULL;

    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

static void whpx_start_vcpu_thread(CPUState* cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/WHPX", cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, whpx_cpu_thread_fn, cpu, QEMU_THREAD_JOINABLE);
}

static void whpx_accel_ops_class_init(ObjectClass* oc, const void* data)
{
    AccelOpsClass* ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = whpx_start_vcpu_thread;
    ops->kick_vcpu_thread   = whpx_kick_vcpu_thread;
    ops->handle_interrupt   = generic_handle_interrupt;

    ops->synchronize_post_reset = whpx_cpu_synchronize_post_reset;
    ops->synchronize_post_init  = whpx_cpu_synchronize_post_reset;
    ops->synchronize_state      = whpx_cpu_synchronize_state;
}

static const TypeInfo whpx_accel_ops_type = {
    .name = ACCEL_OPS_NAME("whpx"),

    .parent     = TYPE_ACCEL_OPS,
    .class_init = whpx_accel_ops_class_init,
    .abstract   = true,
};

DEFINE_TYPE(whpx_accel_ops_type)
