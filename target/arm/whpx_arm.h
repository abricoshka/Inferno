/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) support
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#pragma once

#include "target/arm/cpu-qom.h"

/// The handler of a vCPU's inbound IRQ and FIQ lines, which the hypervisor's GIC raises.
void whpx_arm_set_irq(void* opaque, int irq, int level);
void whpx_arm_set_cpu_features_from_host(ARMCPU* cpu);
