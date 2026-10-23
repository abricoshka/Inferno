/*
 * Apple A7IOP Core.
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
#include "hw/misc/a7iop/core.h"
#include "hw/misc/a7iop/mailbox/core.h"
#include "hw/misc/a7iop/private.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/lockable.h"

bool apple_a7iop_send_ap(AppleA7IOP* a7iop, const AppleA7IOPMessage* message)
{ return apple_a7iop_mailbox_send_ap(a7iop->iop_mailbox, message); }

bool apple_a7iop_recv_ap(AppleA7IOP* a7iop, AppleA7IOPMessage* message)
{ return apple_a7iop_mailbox_recv_ap(a7iop->iop_mailbox, message); }

bool apple_a7iop_send_iop(AppleA7IOP* a7iop, const AppleA7IOPMessage* message)
{ return apple_a7iop_mailbox_send_iop(a7iop->ap_mailbox, message); }

bool apple_a7iop_recv_iop(AppleA7IOP* a7iop, AppleA7IOPMessage* message)
{ return apple_a7iop_mailbox_recv_iop(a7iop->ap_mailbox, message); }

void apple_a7iop_cpu_start(AppleA7IOP* s, bool wake)
{
    // Already awake - do nothing.
    // Skip check if Secure Enclave Processor.
    if ((apple_a7iop_get_cpu_ctrl(s) & SEP_BOOT_MONITOR_RUN) == 0
        && (apple_a7iop_get_cpu_status(s) & CPU_STATUS_IDLE) == 0)
    {
        return;
    }

    apple_a7iop_set_cpu_status(s, apple_a7iop_get_cpu_status(s) & ~CPU_STATUS_IDLE);

    if (s->ops == NULL) { return; }

    if (wake) {
        if (s->ops->wakeup != NULL) { s->ops->wakeup(s); }
    }
    else if (s->ops->start != NULL) {
        s->ops->start(s);
    }
}

uint32_t apple_a7iop_get_cpu_status(AppleA7IOP* s)
{
    QEMU_LOCK_GUARD(&s->lock);
    return s->cpu_status;
}

void apple_a7iop_set_cpu_status(AppleA7IOP* s, uint32_t value)
{
    QEMU_LOCK_GUARD(&s->lock);
    s->cpu_status = value;
}

uint32_t apple_a7iop_get_cpu_ctrl(AppleA7IOP* s)
{
    QEMU_LOCK_GUARD(&s->lock);
    return s->cpu_ctrl;
}

void apple_a7iop_set_cpu_ctrl(AppleA7IOP* s, uint32_t value)
{
    WITH_QEMU_LOCK_GUARD(&s->lock) { s->cpu_ctrl = value; }
    if ((value & (CPU_CTRL_RUN | SEP_BOOT_MONITOR_RUN)) != 0) { apple_a7iop_cpu_start(s, false); }
}

static void apple_a7iop_reset_enter(Object* obj, ResetType type)
{
    AppleA7IOP* s = APPLE_A7IOP(obj);

    QEMU_LOCK_GUARD(&s->lock);

    s->cpu_status = CPU_STATUS_IDLE;
    s->cpu_ctrl   = 0;
}

static void apple_a7iop_realize(DeviceState* dev, Error** errp)
{
    AppleA7IOP* s = APPLE_A7IOP(dev);
    char        name[32];

    if (s->mmio_size == 0) {
        error_setg(errp, "%s: mmio-size is not set", s->role);
        return;
    }

    snprintf(name, sizeof(name), "%s-iop", s->role);
    s->iop_mailbox = apple_a7iop_mailbox_new(name, s->version, NULL, NULL, s, s->handle_messages);
    object_property_add_child(OBJECT(dev), "iop-mailbox", OBJECT(s->iop_mailbox));

    snprintf(name, sizeof(name), "%s-ap", s->role);
    s->ap_mailbox              = apple_a7iop_mailbox_new(name, s->version, s->iop_mailbox, NULL, NULL, NULL);
    s->iop_mailbox->ap_mailbox = s->ap_mailbox;
    object_property_add_child(OBJECT(dev), "ap-mailbox", OBJECT(s->ap_mailbox));

    switch (s->version) {
        case APPLE_A7IOP_V2: apple_a7iop_init_mmio_v2(s, s->mmio_size); break;
        case APPLE_A7IOP_V4: apple_a7iop_init_mmio_v4(s, s->mmio_size); break;
        default            : error_setg(errp, "%s: unknown A7IOP version %u", s->role, s->version); return;
    }

    sysbus_pass_irq(SYS_BUS_DEVICE(dev), SYS_BUS_DEVICE(s->ap_mailbox));

    if (!sysbus_realize(SYS_BUS_DEVICE(s->iop_mailbox), errp)) { return; }
    sysbus_realize(SYS_BUS_DEVICE(s->ap_mailbox), errp);
}

static void apple_a7iop_init(Object* obj)
{
    AppleA7IOP* s = APPLE_A7IOP(obj);

    qemu_mutex_init(&s->lock);
}

static const Property apple_a7iop_properties[] = {
    DEFINE_PROP_UINT32("version", AppleA7IOP, version, APPLE_A7IOP_V4),
    DEFINE_PROP_UINT64("mmio-size", AppleA7IOP, mmio_size, 0),
};

static void apple_a7iop_unrealize(DeviceState* dev)
{
    AppleA7IOP* s;

    s = APPLE_A7IOP(dev);

    qdev_unrealize(DEVICE(s->iop_mailbox));
    qdev_unrealize(DEVICE(s->ap_mailbox));
}

static void apple_a7iop_class_init(ObjectClass* oc, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(oc);
    DeviceClass*     dc = DEVICE_CLASS(oc);

    rc->phases.enter = apple_a7iop_reset_enter;

    dc->realize   = apple_a7iop_realize;
    dc->unrealize = apple_a7iop_unrealize;
    device_class_set_props(dc, apple_a7iop_properties);
    dc->desc = "Apple A7IOP";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(AppleA7IOP, apple_a7iop, APPLE_A7IOP, SYS_BUS_DEVICE)

void apple_a7iop_connect_irqs(AppleA7IOP* a7iop, DeviceState* intc, uint32_t irq)
{
    SysBusDevice* sbd = SYS_BUS_DEVICE(a7iop);

    sysbus_connect_irq(sbd, APPLE_A7IOP_IRQ_IOP_EMPTY, qdev_get_gpio_in(intc, irq));
    sysbus_connect_irq(sbd, APPLE_A7IOP_IRQ_IOP_NONEMPTY, qdev_get_gpio_in(intc, irq + 1));
    sysbus_connect_irq(sbd, APPLE_A7IOP_IRQ_AP_EMPTY, qdev_get_gpio_in(intc, irq + 2));
    sysbus_connect_irq(sbd, APPLE_A7IOP_IRQ_AP_NONEMPTY, qdev_get_gpio_in(intc, irq + 3));
}
