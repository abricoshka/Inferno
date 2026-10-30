/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) stubs
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "whpx_arm.h"

void whpx_arm_set_irq(void* opaque, int irq, int level) { assert_not_reached(); }
