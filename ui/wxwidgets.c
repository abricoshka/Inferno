/*
 * wxWidgets display backend C Bridge.
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
#include "qemu/help-texts.h"
#include "qemu-main.h"
#include "ui/clipboard.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu-version.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "ui/wxwidgets-bridge.h"

typedef struct CKIUIState
{
    void*                 app;
    DisplayChangeListener dcl;
    QKbdState*            kbd;
} CKIUIState;

static void wxwidgets_gfx_switch(DisplayChangeListener* dcl, DisplaySurface* surface);
static void wxwidgets_gfx_update(DisplayChangeListener* dcl, int x, int y, int w, int h);
static void wxwidgets_gfx_refresh(DisplayChangeListener* dcl);
static void wxwidgets_mouse_set(DisplayChangeListener* dcl, int x, int y, bool on);
static void wxwidgets_cursor_define(DisplayChangeListener* dcl, QEMUCursor* cursor);

static const DisplayChangeListenerOps wxwidgets_dcl_ops = {
    .dpy_name          = "wxwidgets",
    .dpy_gfx_switch    = wxwidgets_gfx_switch,
    .dpy_gfx_update    = wxwidgets_gfx_update,
    .dpy_refresh       = wxwidgets_gfx_refresh,
    .dpy_mouse_set     = wxwidgets_mouse_set,
    .dpy_cursor_define = wxwidgets_cursor_define,
};

static CKIUIState wxwidgets_state = {
    .dcl.ops = &wxwidgets_dcl_ops,
};

static void wxwidgets_gfx_switch(DisplayChangeListener* dcl, DisplaySurface* surface)
{
    CKIUIState*     state = container_of(dcl, CKIUIState, dcl);
    pixman_image_t* image = surface->image;

    pixman_image_ref(image);
    CKIUISetSurface(state->app, image, surface_data(surface), surface_width(surface), surface_height(surface),
                    surface_stride(surface));
}

void CKIUIReleaseSurface(void* image) { pixman_image_unref((pixman_image_t*)image); }

static void wxwidgets_gfx_update(DisplayChangeListener* dcl, int x, int y, int w, int h)
{
    CKIUIState* state = container_of(dcl, CKIUIState, dcl);
    CKIUIInvalidate(state->app, x, y, w, h);
}

static void wxwidgets_gfx_refresh(DisplayChangeListener* dcl) { graphic_hw_update(dcl->con); }

static void wxwidgets_mouse_set(DisplayChangeListener* dcl, int x, int y, bool on)
{
    CKIUIState* state = container_of(dcl, CKIUIState, dcl);
    CKIUISetMousePosition(state->app, x, y, on);
}

static void wxwidgets_cursor_define(DisplayChangeListener* dcl, QEMUCursor* cursor)
{
    CKIUIState* state = container_of(dcl, CKIUIState, dcl);
    if (cursor == NULL) { CKIUISetCursor(state->app, 0, 0, 0, 0, NULL); }
    else {
        CKIUISetCursor(state->app, cursor->width, cursor->height, cursor->hot_x, cursor->hot_y, cursor->data);
    }
}

bool CKIUIConsoleIsGraphic(void)
{
    BQL_LOCK_GUARD();
    return wxwidgets_state.dcl.con != NULL && qemu_console_is_graphic(wxwidgets_state.dcl.con);
}

void CKIUIKeyEvent(const char* qcode, bool down)
{
    const int key = qapi_enum_parse(&QKeyCode_lookup, qcode, Q_KEY_CODE__MAX, NULL);
    if (key == Q_KEY_CODE__MAX) { return; }

    BQL_LOCK_GUARD();
    if (wxwidgets_state.kbd != NULL && qemu_console_is_graphic(wxwidgets_state.dcl.con)) {
        qkbd_state_key_event(wxwidgets_state.kbd, (QKeyCode)key, down);
    }
}

void CKIUIReleaseAllKeys(void)
{
    BQL_LOCK_GUARD();
    if (wxwidgets_state.kbd != NULL) { qkbd_state_lift_all_keys(wxwidgets_state.kbd); }
}

static void wxwidgets_put_keysym(int keysym)
{
    BQL_LOCK_GUARD();
    if (wxwidgets_state.dcl.con != NULL && !qemu_console_is_graphic(wxwidgets_state.dcl.con)) {
        qemu_text_console_put_keysym(QEMU_TEXT_CONSOLE(wxwidgets_state.dcl.con), keysym);
    }
}

void CKIUITextKeyEvent(CKIUITextKey key, bool ctrl)
{
    switch (key) {
        case CKIUITextKeyUp       : wxwidgets_put_keysym(ctrl ? QEMU_KEY_CTRL_UP : QEMU_KEY_UP); break;
        case CKIUITextKeyDown     : wxwidgets_put_keysym(ctrl ? QEMU_KEY_CTRL_DOWN : QEMU_KEY_DOWN); break;
        case CKIUITextKeyLeft     : wxwidgets_put_keysym(ctrl ? QEMU_KEY_CTRL_LEFT : QEMU_KEY_LEFT); break;
        case CKIUITextKeyRight    : wxwidgets_put_keysym(ctrl ? QEMU_KEY_CTRL_RIGHT : QEMU_KEY_RIGHT); break;
        case CKIUITextKeyHome     : wxwidgets_put_keysym(ctrl ? QEMU_KEY_CTRL_HOME : QEMU_KEY_HOME); break;
        case CKIUITextKeyEnd      : wxwidgets_put_keysym(ctrl ? QEMU_KEY_CTRL_END : QEMU_KEY_END); break;
        case CKIUITextKeyPageUp   : wxwidgets_put_keysym(ctrl ? QEMU_KEY_CTRL_PAGEUP : QEMU_KEY_PAGEUP); break;
        case CKIUITextKeyPageDown : wxwidgets_put_keysym(ctrl ? QEMU_KEY_CTRL_PAGEDOWN : QEMU_KEY_PAGEDOWN); break;
        case CKIUITextKeyDelete   : wxwidgets_put_keysym(QEMU_KEY_DELETE); break;
        case CKIUITextKeyBackspace: wxwidgets_put_keysym(QEMU_KEY_BACKSPACE); break;
        default                   : break;
    }
}

void CKIUITextCharEvent(int character)
{
    if (character > 0) { wxwidgets_put_keysym(character); }
}

void CKIUIPointerEvent(int x, int y, int width, int height)
{
    BQL_LOCK_GUARD();
    if (wxwidgets_state.dcl.con != NULL) {
        qemu_input_queue_abs(wxwidgets_state.dcl.con, INPUT_AXIS_X, x, 0, width - 1);
        qemu_input_queue_abs(wxwidgets_state.dcl.con, INPUT_AXIS_Y, y, 0, height - 1);
        qemu_input_event_sync();
    }
}

void CKIUIPointerRelativeEvent(int dx, int dy)
{
    BQL_LOCK_GUARD();
    if (wxwidgets_state.dcl.con != NULL) {
        qemu_input_queue_rel(wxwidgets_state.dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(wxwidgets_state.dcl.con, INPUT_AXIS_Y, dy);
        qemu_input_event_sync();
    }
}

static InputButton wxwidgets_input_button(CKIUIButton button)
{
    switch (button) {
        case CKIUIButtonLeft      : return INPUT_BUTTON_LEFT;
        case CKIUIButtonMiddle    : return INPUT_BUTTON_MIDDLE;
        case CKIUIButtonRight     : return INPUT_BUTTON_RIGHT;
        case CKIUIButtonWheelUp   : return INPUT_BUTTON_WHEEL_UP;
        case CKIUIButtonWheelDown : return INPUT_BUTTON_WHEEL_DOWN;
        case CKIUIButtonWheelLeft : return INPUT_BUTTON_WHEEL_LEFT;
        case CKIUIButtonWheelRight: return INPUT_BUTTON_WHEEL_RIGHT;
        default                   : return INPUT_BUTTON__MAX;
    }
}

void CKIUIButtonEvent(CKIUIButton button, bool down)
{
    const InputButton input = wxwidgets_input_button(button);
    if (input == INPUT_BUTTON__MAX) { return; }

    BQL_LOCK_GUARD();
    if (wxwidgets_state.dcl.con != NULL) {
        qemu_input_queue_btn(wxwidgets_state.dcl.con, input, down);
        qemu_input_event_sync();
    }
}

static void wxwidgets_update_mouse_mode(void)
{ CKIUISetMouseMode(wxwidgets_state.app, qemu_input_is_absolute(wxwidgets_state.dcl.con)); }

static void wxwidgets_mouse_mode_change(Notifier* notifier, void* data) { wxwidgets_update_mouse_mode(); }

static Notifier wxwidgets_mouse_mode_notifier = {.notify = wxwidgets_mouse_mode_change};

void CKIUIRequestQuit(void)
{
    BQL_LOCK_GUARD();
    shutdown_action = SHUTDOWN_ACTION_POWEROFF;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

void CKIUIPause(void)
{
    BQL_LOCK_GUARD();
    qmp_stop(NULL);
}

void CKIUIResume(void)
{
    BQL_LOCK_GUARD();
    qmp_cont(NULL);
}

void CKIUIReset(void)
{
    BQL_LOCK_GUARD();
    qmp_system_reset(NULL);
}

void CKIUIPowerDown(void)
{
    BQL_LOCK_GUARD();
    qmp_system_powerdown(NULL);
}

void CKIUISelectConsole(int index)
{
    if (index < 0) { return; }

    BQL_LOCK_GUARD();
    QemuConsole* con = qemu_console_lookup_by_index((unsigned int)index);
    if (con != NULL && con != wxwidgets_state.dcl.con) {
        unregister_displaychangelistener(&wxwidgets_state.dcl);
        qkbd_state_switch_console(wxwidgets_state.kbd, con);
        wxwidgets_state.dcl.con = con;
        register_displaychangelistener(&wxwidgets_state.dcl);
        wxwidgets_update_mouse_mode();
    }
}

void CKIUIEnumerateConsoles(CKIUIItemCallback callback, void* context)
{
    BQL_LOCK_GUARD();
    QemuConsole* con;
    for (unsigned int index = 0; (con = qemu_console_lookup_by_index(index)) != NULL; ++index) {
        g_autofree char* label = qemu_console_get_label(con);
        callback(context, label);
    }
}

void CKIUIEnumerateRemovableDevices(CKIUIItemCallback callback, void* context)
{
    BQL_LOCK_GUARD();
    BlockInfoList* list = qmp_query_block(NULL);
    for (BlockInfoList* current = list; current != NULL; current = current->next) {
        if (current->value->removable) { callback(context, current->value->device); }
    }
    qapi_free_BlockInfoList(list);
}

static bool wxwidgets_finish_error(Error* err, char* error, size_t errorSize)
{
    if (err == NULL) { return true; }
    if (error != NULL && errorSize > 0) { g_strlcpy(error, error_get_pretty(err), errorSize); }
    error_free(err);
    return false;
}

bool CKIUIEjectMedia(const char* device, char* error, size_t errorSize)
{
    BQL_LOCK_GUARD();
    Error* err = NULL;
    qmp_eject(device, NULL, false, false, &err);
    return wxwidgets_finish_error(err, error, errorSize);
}

bool CKIUIChangeMedia(const char* device, const char* file, char* error, size_t errorSize)
{
    BQL_LOCK_GUARD();
    Error* err = NULL;
    qmp_blockdev_change_medium(device, NULL, file, "raw", true, false, false, 0, &err);
    return wxwidgets_finish_error(err, error, errorSize);
}

void CKIUISetUIInfo(int width, int height, int widthMM, int heightMM, int refreshHz)
{
    BQL_LOCK_GUARD();
    QemuConsole* con = wxwidgets_state.dcl.con;
    if (con == NULL || !qemu_console_is_graphic(con)) { return; }

    QemuUIInfo info = {
        .width_mm  = widthMM,
        .height_mm = heightMM,
        .width     = width,
        .height    = height,
    };
    if (refreshHz > 0) {
        update_displaychangelistener(&wxwidgets_state.dcl, 1000 / refreshHz);
        info.refresh_rate = (int64_t)refreshHz * 1000;
    }
    dpy_set_ui_info(con, &info, true);
}

static void wxwidgets_clipboard_notify(Notifier* notifier, void* data);
static void wxwidgets_clipboard_request(QemuClipboardInfo* info, QemuClipboardType type);

static QemuClipboardPeer wxwidgets_clipboard_peer = {
    .name     = "wxwidgets",
    .notifier = {.notify = wxwidgets_clipboard_notify},
    .request  = wxwidgets_clipboard_request,
};

static bool               wxwidgets_clipboard_ready;
static QemuClipboardInfo* wxwidgets_host_info;
static char*              wxwidgets_host_text;
static size_t             wxwidgets_host_size;

static void wxwidgets_clipboard_notify(Notifier* notifier, void* data)
{
    QemuClipboardNotify* notify = data;
    QemuClipboardInfo*   info;

    if (notify->type != QEMU_CLIPBOARD_UPDATE_INFO) { return; }

    info = notify->info;
    if (info->owner == &wxwidgets_clipboard_peer || info->selection != QEMU_CLIPBOARD_SELECTION_CLIPBOARD) { return; }
    if (!info->types[QEMU_CLIPBOARD_TYPE_TEXT].available) { return; }

    if (info->types[QEMU_CLIPBOARD_TYPE_TEXT].data == NULL) {
        qemu_clipboard_request(info, QEMU_CLIPBOARD_TYPE_TEXT);
        return;
    }
    if (info->types[QEMU_CLIPBOARD_TYPE_TEXT].size == 0) { return; }

    CKIUISetClipboardText(wxwidgets_state.app, info->types[QEMU_CLIPBOARD_TYPE_TEXT].data,
                          info->types[QEMU_CLIPBOARD_TYPE_TEXT].size);
}

static void wxwidgets_clipboard_request(QemuClipboardInfo* info, QemuClipboardType type)
{
    if (type != QEMU_CLIPBOARD_TYPE_TEXT || info != wxwidgets_host_info || wxwidgets_host_text == NULL) { return; }
    qemu_clipboard_set_data(&wxwidgets_clipboard_peer, info, type, wxwidgets_host_size, wxwidgets_host_text, true);
}

bool CKIUIClipboardChanged(const char* text, size_t size)
{
    if (!wxwidgets_clipboard_ready) { return false; }

    BQL_LOCK_GUARD();

    g_free(wxwidgets_host_text);
    wxwidgets_host_text = size > 0 ? g_memdup2(text, size) : NULL;
    wxwidgets_host_size = size;

    qemu_clipboard_info_unref(wxwidgets_host_info);
    wxwidgets_host_info = qemu_clipboard_info_new(&wxwidgets_clipboard_peer, QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
    wxwidgets_host_info->types[QEMU_CLIPBOARD_TYPE_TEXT].available = size > 0;
    qemu_clipboard_update(wxwidgets_host_info);

    return true;
}

bool CKIUIGetIconPath(char* buffer, size_t size)
{
    g_autofree char* path = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/512x512/apps/inferno.png");
    if (path == NULL) { return false; }
    g_strlcpy(buffer, path, size);
    return true;
}

const char* CKIUIVersion(void) { return QEMU_FULL_VERSION; }

const char* CKIUICopyright(void) { return QEMU_COPYRIGHT; }

static void wxwidgets_shutdown(void) { CKIUIShutdown(wxwidgets_state.app); }

#if defined(CONFIG_DARWIN) || defined(CONFIG_WIN32)
static int wxwidgets_main(void)
{
    CKIUIRun(wxwidgets_state.app);
    assert_not_reached();
}
#endif

static void wxwidgets_display_init(DisplayState* ds, DisplayOptions* opts)
{
    CKIUIOptions options = {
        .name              = qemu_name,
        .fullScreen        = opts->has_full_screen && opts->full_screen,
        .showCursor        = !opts->has_show_cursor || opts->show_cursor,
        .zoomToFit         = !opts->u.wxwidgets.has_zoom_to_fit || opts->u.wxwidgets.zoom_to_fit,
        .zoomInterpolation = !opts->u.wxwidgets.has_zoom_interpolation || opts->u.wxwidgets.zoom_interpolation,
    };

    wxwidgets_state.app     = CKIUICreate(&options);
    wxwidgets_state.dcl.con = qemu_console_lookup_default();
    wxwidgets_state.kbd     = qkbd_state_init(wxwidgets_state.dcl.con);

    register_displaychangelistener(&wxwidgets_state.dcl);
    qemu_add_mouse_mode_change_notifier(&wxwidgets_mouse_mode_notifier);
    wxwidgets_update_mouse_mode();

    qemu_clipboard_peer_register(&wxwidgets_clipboard_peer);
    wxwidgets_clipboard_ready = true;

    atexit(wxwidgets_shutdown);
#if defined(CONFIG_DARWIN) || defined(CONFIG_WIN32)
    qemu_main = wxwidgets_main;
#endif
}

static QemuDisplay qemu_display_wxwidgets = {
    .type = DISPLAY_TYPE_WXWIDGETS,
    .init = wxwidgets_display_init,
};

static void register_wxwidgets(void) { qemu_display_register(&qemu_display_wxwidgets); }

type_init(register_wxwidgets);
