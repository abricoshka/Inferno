/*
 * Apple iPhone 11 Buttons
 *
 * Copyright (c) 2025-2026 Christian Inci (chris-pcguy).
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/input/buttons.h"
#include "hw/misc/smc.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "qemu/lockable.h"
#include "system/runstate.h"
#include "ui/input.h"

struct AppleButtonsState
{
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuMutex mutex;
    AppleSMC* smc;
    uint16_t  states;
};

static void apple_buttons_handle_event(DeviceState* dev, QemuConsole* src, InputEvent* evt)
{
    AppleButtonsState* buttons = APPLE_BUTTONS(dev);
    InputKeyEvent*     key     = evt->u.key.data;
    int                qcode;
    AppleSMCHIDButton  button;

    QEMU_LOCK_GUARD(&buttons->mutex);

    AppleSMC* smc = buttons->smc;

    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, NULL);

    qcode = qemu_input_key_value_to_qcode(key->key);

    switch (qcode) {
        case Q_KEY_CODE_F1: button = SMC_HID_BUTTON_FORCE_SHUTDOWN; break;
        case Q_KEY_CODE_F2:
            if (key->down) {
                button = SMC_HID_BUTTON_RINGER;
                if (buttons->states & BIT32(button)) { buttons->states &= ~BIT32(button); }
                else {
                    buttons->states |= BIT32(button);
                }
                apple_smc_send_hid_button(smc, button, (buttons->states & BIT32(button)) != 0);
            }
            return;
        case Q_KEY_CODE_F3 : button = SMC_HID_BUTTON_VOL_DOWN; break;
        case Q_KEY_CODE_F4 : button = SMC_HID_BUTTON_VOL_UP; break;
        case Q_KEY_CODE_F5 : button = SMC_HID_BUTTON_HOLD; break;
        case Q_KEY_CODE_F6 : button = SMC_HID_BUTTON_MENU; break;
        case Q_KEY_CODE_F7 : button = SMC_HID_BUTTON_HELP; break;
        case Q_KEY_CODE_F8 : button = SMC_HID_BUTTON_HELP_DOUBLE; break;
        case Q_KEY_CODE_F9 : button = SMC_HID_BUTTON_HALL_EFFECT_1; break;
        case Q_KEY_CODE_F10: button = SMC_HID_BUTTON_HALL_EFFECT; break;
        default            : return;
    }

    if (((buttons->states & BIT32(button)) != 0) != key->down) {
        if (key->down) { buttons->states |= BIT32(button); }
        else {
            buttons->states &= ~BIT32(button);
        }
        apple_smc_send_hid_button(smc, button, key->down);
    }
}

#define BUTTON_READER(_btn, _enum)                                                                                   \
    static SMCResult apple_buttons_smc_read_##_btn(SMCKey* key, SMCKeyData* data, const void* in, uint8_t in_length) \
    {                                                                                                                \
        AppleButtonsState* s = key->opaque;                                                                          \
                                                                                                                     \
        stl_le_p(data->data, (s->states & BIT32(SMC_HID_BUTTON_##_enum)) != 0);                                      \
                                                                                                                     \
        return SMC_RESULT_SUCCESS;                                                                                   \
    }

BUTTON_READER(vol_up, VOL_UP);
BUTTON_READER(vol_down, VOL_DOWN);
BUTTON_READER(hold, HOLD);
BUTTON_READER(ringer, RINGER);

static void apple_buttons_reset_enter(Object* obj, ResetType type)
{
    AppleButtonsState* s = APPLE_BUTTONS(obj);
    QEMU_LOCK_GUARD(&s->mutex);
    s->states = 0;
}

static const QemuInputHandler apple_buttons_handler = {
    .name  = "Apple Buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = apple_buttons_handle_event,
};

static void apple_buttons_realize(DeviceState* dev, Error** errp)
{
    AppleButtonsState* buttons = APPLE_BUTTONS(dev);
    AppleSMC*          smc     = buttons->smc;
    apple_smc_add_key_func(smc, 'bVUP', 4, SMC_KEY_TYPE_UINT32, SMC_ATTR_LE, buttons, apple_buttons_smc_read_vol_up,
                           NULL);
    apple_smc_add_key_func(smc, 'bVDN', 4, SMC_KEY_TYPE_UINT32, SMC_ATTR_LE, buttons, apple_buttons_smc_read_vol_down,
                           NULL);
    apple_smc_add_key_func(smc, 'bHLD', 4, SMC_KEY_TYPE_UINT32, SMC_ATTR_LE, buttons, apple_buttons_smc_read_hold,
                           NULL);
    apple_smc_add_key_func(smc, 'bRIN', 4, SMC_KEY_TYPE_UINT32, SMC_ATTR_LE, buttons, apple_buttons_smc_read_ringer,
                           NULL);
    bool powered_by_hold_btn = true;
    apple_smc_add_key(smc, 'bPHD', 1, SMC_KEY_TYPE_FLAG, SMC_ATTR_R, &powered_by_hold_btn);

    QemuInputHandlerState* input_handler = qemu_input_handler_register(dev, &apple_buttons_handler);
    qemu_input_handler_activate(input_handler);
    qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_OTHER, true);
}

static void apple_buttons_init(Object* obj)
{
    AppleButtonsState* buttons = APPLE_BUTTONS(obj);
    qemu_mutex_init(&buttons->mutex);
    object_property_add_link(obj, "smc", TYPE_APPLE_SMC_IOP, (Object**)&buttons->smc,
                             qdev_prop_allow_set_link_before_realize, OBJ_PROP_LINK_STRONG);
}

static void apple_buttons_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*     dc = DEVICE_CLASS(klass);
    ResettableClass* rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = apple_buttons_reset_enter;

    dc->realize = apple_buttons_realize;
    dc->desc    = "Apple Buttons";
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(AppleButtonsState, apple_buttons, APPLE_BUTTONS, SYS_BUS_DEVICE);
