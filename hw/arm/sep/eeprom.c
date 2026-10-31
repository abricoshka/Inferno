/*
 * Apple SEP EEPROM.
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

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/sep/private.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "system/runstate.h"

#define SEP_EEPROM_SIZE       (64 * KiB)
#define SEP_EEPROM_ADDR_BYTES (2)

struct AppleSEPEEPROMState
{
    I2CSlave parent_obj;

    BlockBackend*       blk;
    QEMUBH*             flush_bh;
    VMChangeStateEntry* vmstate_entry;
    QEMUIOVector        flush_qiov;
    bool                dirty;
    bool                flush_in_flight;
    uint16_t            cur;
    uint8_t             addr_bytes;
    uint8_t             mem[SEP_EEPROM_SIZE];
    uint8_t             flush_buf[SEP_EEPROM_SIZE];
};

static void sep_eeprom_flush_done(void* opaque, int ret)
{
    AppleSEPEEPROMState* s = opaque;

    s->flush_in_flight = false;
    if (ret < 0) { error_report("apple_sep_eeprom: writeback failed: %s", strerror(-ret)); }

    /* Written again while the flush was in flight. */
    if (s->dirty) { qemu_bh_schedule(s->flush_bh); }
}

static void sep_eeprom_flush_bh(void* opaque)
{
    AppleSEPEEPROMState* s = opaque;

    if (s->flush_in_flight || !s->dirty) { return; }

    s->dirty           = false;
    s->flush_in_flight = true;
    memcpy(s->flush_buf, s->mem, sizeof(s->mem));
    blk_aio_pwritev(s->blk, 0, &s->flush_qiov, 0, sep_eeprom_flush_done, s);
}

static void sep_eeprom_flush_sync(AppleSEPEEPROMState* s)
{
    int ret;

    if (s->blk == NULL) { return; }

    if (s->flush_in_flight) { blk_drain(s->blk); }
    if (!s->dirty) { return; }

    s->dirty = false;
    ret      = blk_pwrite(s->blk, 0, sizeof(s->mem), s->mem, 0);
    if (ret < 0) { error_report("apple_sep_eeprom: writeback failed: %s", strerror(-ret)); }
}

static void sep_eeprom_vm_state_change(void* opaque, bool running, RunState state)
{
    if (!running) { sep_eeprom_flush_sync(opaque); }
}

static int apple_sep_eeprom_event(I2CSlave* i2c, enum i2c_event event)
{
    AppleSEPEEPROMState* s = APPLE_SEP_EEPROM(i2c);

    switch (event) {
        case I2C_START_SEND:
        case I2C_FINISH    : s->addr_bytes = 0; break;
        case I2C_START_RECV:
        case I2C_NACK      : break;
        default            : return -1;
    }

    if (s->dirty && s->blk != NULL) { qemu_bh_schedule(s->flush_bh); }

    return 0;
}

static uint8_t apple_sep_eeprom_rx(I2CSlave* i2c)
{
    AppleSEPEEPROMState* s = APPLE_SEP_EEPROM(i2c);

    if (s->addr_bytes != 0 && s->addr_bytes < SEP_EEPROM_ADDR_BYTES) { return 0xFF; }

    return s->mem[s->cur++];
}

static int apple_sep_eeprom_tx(I2CSlave* i2c, uint8_t data)
{
    AppleSEPEEPROMState* s = APPLE_SEP_EEPROM(i2c);

    if (s->addr_bytes < SEP_EEPROM_ADDR_BYTES) {
        s->cur         = (s->cur << 8) | data;
        s->addr_bytes += 1;
        return 0;
    }

    s->mem[s->cur++] = data;
    s->dirty         = true;

    return 0;
}

static void apple_sep_eeprom_reset_enter(Object* obj, ResetType type)
{
    AppleSEPEEPROMState* s = APPLE_SEP_EEPROM(obj);

    s->cur        = 0;
    s->addr_bytes = 0;
}

static void apple_sep_eeprom_realize(DeviceState* dev, Error** errp)
{
    AppleSEPEEPROMState* s = APPLE_SEP_EEPROM(dev);
    int64_t              len;
    int                  ret;

    if (s->blk == NULL) { return; }

    len = blk_getlength(s->blk);
    if (len < 0) {
        error_setg_errno(errp, -len, "apple_sep_eeprom: cannot determine backing size");
        return;
    }
    if (len != SEP_EEPROM_SIZE) {
        error_setg(errp, "apple_sep_eeprom: backing is 0x%" PRIx64 " bytes, want 0x%x", (uint64_t)len, SEP_EEPROM_SIZE);
        return;
    }

    if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE, BLK_PERM_ALL, errp) < 0) { return; }

    ret = blk_pread(s->blk, 0, sizeof(s->mem), s->mem, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "apple_sep_eeprom: cannot read backing");
        return;
    }

    qemu_iovec_init_buf(&s->flush_qiov, s->flush_buf, sizeof(s->flush_buf));
    s->flush_bh      = qemu_bh_new(sep_eeprom_flush_bh, s);
    s->vmstate_entry = qemu_add_vm_change_state_handler(sep_eeprom_vm_state_change, s);
}

static void apple_sep_eeprom_unrealize(DeviceState* dev)
{
    AppleSEPEEPROMState* s = APPLE_SEP_EEPROM(dev);

    if (s->vmstate_entry != NULL) {
        qemu_del_vm_change_state_handler(s->vmstate_entry);
        s->vmstate_entry = NULL;
    }
    if (s->flush_bh != NULL) {
        sep_eeprom_flush_sync(s);
        qemu_bh_delete(s->flush_bh);
        s->flush_bh = NULL;
    }
}

AppleSEPEEPROMState* apple_sep_eeprom_create(AppleI2CState* i2c, uint8_t addr, BlockBackend* blk)
{
    AppleSEPEEPROMState* s;

    s = APPLE_SEP_EEPROM(i2c_slave_new(TYPE_APPLE_SEP_EEPROM, addr));

    qdev_prop_set_drive_err(DEVICE(s), "drive", blk, &error_fatal);
    i2c_slave_realize_and_unref(I2C_SLAVE(s), i2c->bus, &error_fatal);

    return s;
}

static const Property apple_sep_eeprom_props[] = {
    DEFINE_PROP_DRIVE("drive", AppleSEPEEPROMState, blk),
};

static void apple_sep_eeprom_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);
    I2CSlaveClass*   c  = I2C_SLAVE_CLASS(klass);

    rc->phases.enter = apple_sep_eeprom_reset_enter;

    dc->desc      = "Apple SEP EEPROM";
    dc->realize   = apple_sep_eeprom_realize;
    dc->unrealize = apple_sep_eeprom_unrealize;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    c->event = apple_sep_eeprom_event;
    c->recv  = apple_sep_eeprom_rx;
    c->send  = apple_sep_eeprom_tx;

    device_class_set_props(dc, apple_sep_eeprom_props);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPEEPROMState, apple_sep_eeprom, APPLE_SEP_EEPROM, I2C_SLAVE)
