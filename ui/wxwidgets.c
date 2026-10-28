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

/*
 * display_init runs with the BQL already held by this thread, while input
 * callbacks from the UI thread run without it. Only take it when needed.
 */
static bool wxwidgets_bql_enter(void)
{
    if (bql_locked()) { return false; }
    bql_lock();
    return true;
}

static void wxwidgets_bql_leave(bool taken)
{
    if (taken) { bql_unlock(); }
}

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

    /*
     * The DisplaySurface is freed as soon as this callback returns, so keep
     * the underlying pixman image (and thereby the pixel data) alive until the
     * UI thread is done with it. The UI hands the reference back through
     * CKIUIReleaseSurface.
     */
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
    const bool taken   = wxwidgets_bql_enter();
    const bool graphic = wxwidgets_state.dcl.con != NULL && qemu_console_is_graphic(wxwidgets_state.dcl.con);
    wxwidgets_bql_leave(taken);
    return graphic;
}

void CKIUIKeyEvent(const char* qcode, bool down)
{
    const int key = qapi_enum_parse(&QKeyCode_lookup, qcode, Q_KEY_CODE__MAX, NULL);
    if (key == Q_KEY_CODE__MAX) { return; }

    const bool taken = wxwidgets_bql_enter();
    if (wxwidgets_state.kbd != NULL && qemu_console_is_graphic(wxwidgets_state.dcl.con)) {
        qkbd_state_key_event(wxwidgets_state.kbd, (QKeyCode)key, down);
    }
    wxwidgets_bql_leave(taken);
}

/*
 * Makes the guest think all held keys were released. This prevents stuck keys
 * since key-up events are not delivered after the window loses focus.
 */
void CKIUIReleaseAllKeys(void)
{
    const bool taken = wxwidgets_bql_enter();
    if (wxwidgets_state.kbd != NULL) { qkbd_state_lift_all_keys(wxwidgets_state.kbd); }
    wxwidgets_bql_leave(taken);
}

void CKIUITextKeyEvent(CKIUITextKey key, bool ctrl)
{
    static const int plain[CKIUITextKeyCount] = {
        [CKIUITextKeyUp] = QEMU_KEY_UP,         [CKIUITextKeyDown] = QEMU_KEY_DOWN,
        [CKIUITextKeyLeft] = QEMU_KEY_LEFT,     [CKIUITextKeyRight] = QEMU_KEY_RIGHT,
        [CKIUITextKeyHome] = QEMU_KEY_HOME,     [CKIUITextKeyEnd] = QEMU_KEY_END,
        [CKIUITextKeyPageUp] = QEMU_KEY_PAGEUP, [CKIUITextKeyPageDown] = QEMU_KEY_PAGEDOWN,
        [CKIUITextKeyDelete] = QEMU_KEY_DELETE, [CKIUITextKeyBackspace] = QEMU_KEY_BACKSPACE,
    };
    static const int withControl[CKIUITextKeyCount] = {
        [CKIUITextKeyUp] = QEMU_KEY_CTRL_UP,         [CKIUITextKeyDown] = QEMU_KEY_CTRL_DOWN,
        [CKIUITextKeyLeft] = QEMU_KEY_CTRL_LEFT,     [CKIUITextKeyRight] = QEMU_KEY_CTRL_RIGHT,
        [CKIUITextKeyHome] = QEMU_KEY_CTRL_HOME,     [CKIUITextKeyEnd] = QEMU_KEY_CTRL_END,
        [CKIUITextKeyPageUp] = QEMU_KEY_CTRL_PAGEUP, [CKIUITextKeyPageDown] = QEMU_KEY_CTRL_PAGEDOWN,
    };

    if ((int)key < 0 || key >= CKIUITextKeyCount) { return; }
    int keysym = ctrl ? withControl[key] : 0;
    if (keysym == 0) { keysym = plain[key]; }
    if (keysym == 0) { return; }

    const bool taken = wxwidgets_bql_enter();
    if (wxwidgets_state.dcl.con != NULL && !qemu_console_is_graphic(wxwidgets_state.dcl.con)) {
        qemu_text_console_put_keysym(QEMU_TEXT_CONSOLE(wxwidgets_state.dcl.con), keysym);
    }
    wxwidgets_bql_leave(taken);
}

void CKIUITextCharEvent(int character)
{
    if (character <= 0) { return; }

    const bool taken = wxwidgets_bql_enter();
    if (wxwidgets_state.dcl.con != NULL && !qemu_console_is_graphic(wxwidgets_state.dcl.con)) {
        qemu_text_console_put_keysym(QEMU_TEXT_CONSOLE(wxwidgets_state.dcl.con), character);
    }
    wxwidgets_bql_leave(taken);
}

void CKIUIPointerEvent(int x, int y, int width, int height)
{
    const bool taken = wxwidgets_bql_enter();
    if (wxwidgets_state.dcl.con != NULL) {
        qemu_input_queue_abs(wxwidgets_state.dcl.con, INPUT_AXIS_X, x, 0, width - 1);
        qemu_input_queue_abs(wxwidgets_state.dcl.con, INPUT_AXIS_Y, y, 0, height - 1);
        qemu_input_event_sync();
    }
    wxwidgets_bql_leave(taken);
}

void CKIUIPointerRelativeEvent(int dx, int dy)
{
    const bool taken = wxwidgets_bql_enter();
    if (wxwidgets_state.dcl.con != NULL) {
        qemu_input_queue_rel(wxwidgets_state.dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(wxwidgets_state.dcl.con, INPUT_AXIS_Y, dy);
        qemu_input_event_sync();
    }
    wxwidgets_bql_leave(taken);
}

void CKIUIButtonEvent(CKIUIButton button, bool down)
{
    static const InputButton buttons[] = {
        [CKIUIButtonLeft]       = INPUT_BUTTON_LEFT,
        [CKIUIButtonMiddle]     = INPUT_BUTTON_MIDDLE,
        [CKIUIButtonRight]      = INPUT_BUTTON_RIGHT,
        [CKIUIButtonWheelUp]    = INPUT_BUTTON_WHEEL_UP,
        [CKIUIButtonWheelDown]  = INPUT_BUTTON_WHEEL_DOWN,
        [CKIUIButtonWheelLeft]  = INPUT_BUTTON_WHEEL_LEFT,
        [CKIUIButtonWheelRight] = INPUT_BUTTON_WHEEL_RIGHT,
    };
    if ((int)button < 0 || (size_t)button >= ARRAY_SIZE(buttons)) { return; }

    const bool taken = wxwidgets_bql_enter();
    if (wxwidgets_state.dcl.con != NULL) {
        qemu_input_queue_btn(wxwidgets_state.dcl.con, buttons[button], down);
        qemu_input_event_sync();
    }
    wxwidgets_bql_leave(taken);
}

static void wxwidgets_mouse_mode_change(Notifier* notifier, void* data)
{ CKIUISetMouseMode(wxwidgets_state.app, qemu_input_is_absolute(wxwidgets_state.dcl.con)); }

static Notifier wxwidgets_mouse_mode_notifier = {.notify = wxwidgets_mouse_mode_change};

void CKIUIRequestQuit(void)
{
    const bool taken = wxwidgets_bql_enter();
    shutdown_action  = SHUTDOWN_ACTION_POWEROFF;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
    wxwidgets_bql_leave(taken);
}

void CKIUIPause(void)
{
    const bool taken = wxwidgets_bql_enter();
    qmp_stop(NULL);
    wxwidgets_bql_leave(taken);
}

void CKIUIResume(void)
{
    const bool taken = wxwidgets_bql_enter();
    qmp_cont(NULL);
    wxwidgets_bql_leave(taken);
}

void CKIUIReset(void)
{
    const bool taken = wxwidgets_bql_enter();
    qmp_system_reset(NULL);
    wxwidgets_bql_leave(taken);
}

void CKIUIPowerDown(void)
{
    const bool taken = wxwidgets_bql_enter();
    qmp_system_powerdown(NULL);
    wxwidgets_bql_leave(taken);
}

void CKIUISelectConsole(int index)
{
    if (index < 0) { return; }

    const bool   taken = wxwidgets_bql_enter();
    QemuConsole* con   = qemu_console_lookup_by_index((unsigned int)index);
    if (con != NULL && con != wxwidgets_state.dcl.con) {
        unregister_displaychangelistener(&wxwidgets_state.dcl);
        qkbd_state_switch_console(wxwidgets_state.kbd, con);
        wxwidgets_state.dcl.con = con;
        register_displaychangelistener(&wxwidgets_state.dcl);
        CKIUISetMouseMode(wxwidgets_state.app, qemu_input_is_absolute(con));
    }
    wxwidgets_bql_leave(taken);
}

void CKIUIEnumerateConsoles(CKIUIItemCallback callback, void* context)
{
    const bool taken = wxwidgets_bql_enter();
    for (int index = 0;; ++index) {
        QemuConsole* con = qemu_console_lookup_by_index((unsigned int)index);
        if (con == NULL) { break; }
        g_autofree char* label = qemu_console_get_label(con);
        callback(context, index, label);
    }
    wxwidgets_bql_leave(taken);
}

static bool wxwidgets_finish_error(Error* err, char* error, size_t errorSize)
{
    if (err == NULL) { return true; }
    if (error != NULL && errorSize > 0) { g_strlcpy(error, error_get_pretty(err), errorSize); }
    error_free(err);
    return false;
}

void CKIUIEnumerateRemovableDevices(CKIUIItemCallback callback, void* context)
{
    const bool     taken   = wxwidgets_bql_enter();
    BlockInfoList* list    = qmp_query_block(NULL);
    BlockInfoList* current = list;
    int            index   = 0;

    while (current != NULL) {
        if (current->value->removable) { callback(context, index++, current->value->device); }
        current = current->next;
    }
    qapi_free_BlockInfoList(list);
    wxwidgets_bql_leave(taken);
}

bool CKIUIEjectMedia(const char* device, char* error, size_t errorSize)
{
    Error*     err   = NULL;
    const bool taken = wxwidgets_bql_enter();
    qmp_eject(device, NULL, false, false, &err);
    wxwidgets_bql_leave(taken);
    return wxwidgets_finish_error(err, error, errorSize);
}

bool CKIUIChangeMedia(const char* device, const char* file, char* error, size_t errorSize)
{
    Error*     err   = NULL;
    const bool taken = wxwidgets_bql_enter();
    qmp_blockdev_change_medium(device, NULL, file, "raw", true, false, false, 0, &err);
    wxwidgets_bql_leave(taken);
    return wxwidgets_finish_error(err, error, errorSize);
}

void CKIUISetUIInfo(int width, int height, int widthMM, int heightMM, int refreshHz)
{
    const bool   taken = wxwidgets_bql_enter();
    QemuConsole* con   = wxwidgets_state.dcl.con;

    if (con != NULL && qemu_console_is_graphic(con)) {
        QemuUIInfo info = {0};

        if (refreshHz > 0) {
            update_displaychangelistener(&wxwidgets_state.dcl, 1000 / refreshHz);
            info.refresh_rate = (int64_t)refreshHz * 1000;
        }

        info.width_mm  = widthMM;
        info.height_mm = heightMM;
        info.xoff      = 0;
        info.yoff      = 0;
        info.width     = width;
        info.height    = height;

        dpy_set_ui_info(con, &info, true);
    }
    wxwidgets_bql_leave(taken);
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

/* Guest -> host. Called with the BQL held. */
static void wxwidgets_clipboard_notify(Notifier* notifier, void* data)
{
    QemuClipboardNotify* notify = data;
    QemuClipboardInfo*   info;

    if (notify->type != QEMU_CLIPBOARD_UPDATE_INFO) { return; }

    info = notify->info;
    if (info->owner == &wxwidgets_clipboard_peer || info->selection != QEMU_CLIPBOARD_SELECTION_CLIPBOARD) { return; }
    if (!info->types[QEMU_CLIPBOARD_TYPE_TEXT].available) { return; }

    if (info->types[QEMU_CLIPBOARD_TYPE_TEXT].data == NULL) {
        /* The data arrives later through another UPDATE_INFO notification. */
        qemu_clipboard_request(info, QEMU_CLIPBOARD_TYPE_TEXT);
        return;
    }
    if (info->types[QEMU_CLIPBOARD_TYPE_TEXT].size == 0) { return; }

    CKIUISetClipboardText(wxwidgets_state.app, info->types[QEMU_CLIPBOARD_TYPE_TEXT].data,
                          info->types[QEMU_CLIPBOARD_TYPE_TEXT].size);
}

/* The guest asks for the data of the clipboard we own. Called with the BQL held. */
static void wxwidgets_clipboard_request(QemuClipboardInfo* info, QemuClipboardType type)
{
    if (type != QEMU_CLIPBOARD_TYPE_TEXT || info != wxwidgets_host_info || wxwidgets_host_text == NULL) { return; }
    qemu_clipboard_set_data(&wxwidgets_clipboard_peer, info, type, wxwidgets_host_size, wxwidgets_host_text, true);
}

bool CKIUIClipboardChanged(const char* text, size_t size)
{
    if (!wxwidgets_clipboard_ready) { return false; }

    const bool taken = wxwidgets_bql_enter();

    g_free(wxwidgets_host_text);
    wxwidgets_host_text = NULL;
    wxwidgets_host_size = 0;
    if (size > 0) {
        wxwidgets_host_text = g_malloc(size);
        memcpy(wxwidgets_host_text, text, size);
        wxwidgets_host_size = size;
    }

    qemu_clipboard_info_unref(wxwidgets_host_info);
    wxwidgets_host_info = qemu_clipboard_info_new(&wxwidgets_clipboard_peer, QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
    if (size > 0) { wxwidgets_host_info->types[QEMU_CLIPBOARD_TYPE_TEXT].available = true; }
    qemu_clipboard_update(wxwidgets_host_info);

    wxwidgets_bql_leave(taken);
    return true;
}

bool CKIUIGetIconPath(char* buffer, size_t size)
{
    char* path = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/512x512/apps/inferno.png");
    if (path == NULL) { return false; }
    g_strlcpy(buffer, path, size);
    g_free(path);
    return true;
}

const char* CKIUIVersion(void) { return QEMU_FULL_VERSION; }

const char* CKIUICopyright(void) { return QEMU_COPYRIGHT; }

static void wxwidgets_shutdown(void) { CKIUIShutdown(wxwidgets_state.app); }

static int wxwidgets_main(void)
{
    CKIUIRun(wxwidgets_state.app);
    assert_not_reached();
}

static void wxwidgets_display_init(DisplayState* ds, DisplayOptions* opts)
{
    CKIUIOptions options = {
        .name              = qemu_name,
        .fullScreen        = opts->has_full_screen && opts->full_screen,
        .showCursor        = !opts->has_show_cursor || opts->show_cursor,
        .zoomToFit         = !opts->u.wxwidgets.has_zoom_to_fit || opts->u.wxwidgets.zoom_to_fit,
        .zoomInterpolation = !opts->u.wxwidgets.has_zoom_interpolation || opts->u.wxwidgets.zoom_interpolation,
    };

    /* Builds the window and menus; the BQL is held by this thread, see wxwidgets_bql_enter. */
    wxwidgets_state.app     = CKIUICreate(&options);
    wxwidgets_state.dcl.con = qemu_console_lookup_default();
    wxwidgets_state.kbd     = qkbd_state_init(wxwidgets_state.dcl.con);

    register_displaychangelistener(&wxwidgets_state.dcl);
    qemu_add_mouse_mode_change_notifier(&wxwidgets_mouse_mode_notifier);
    CKIUISetMouseMode(wxwidgets_state.app, qemu_input_is_absolute(wxwidgets_state.dcl.con));

    qemu_clipboard_peer_register(&wxwidgets_clipboard_peer);
    wxwidgets_clipboard_ready = true;

    atexit(wxwidgets_shutdown);
    qemu_main = wxwidgets_main;
}

static QemuDisplay qemu_display_wxwidgets = {
    .type = DISPLAY_TYPE_WXWIDGETS,
    .init = wxwidgets_display_init,
};

static void register_wxwidgets(void) { qemu_display_register(&qemu_display_wxwidgets); }

type_init(register_wxwidgets);
