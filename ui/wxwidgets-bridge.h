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

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct CKIUIOptions
    {
        const char* name;    // qemu_name, may be NULL
        bool        fullScreen;
        bool        showCursor;
        bool        zoomToFit;
        bool        zoomInterpolation;
    } CKIUIOptions;

    typedef enum CKIUIButton
    {
        CKIUIButtonLeft,
        CKIUIButtonMiddle,
        CKIUIButtonRight,
        CKIUIButtonWheelUp,
        CKIUIButtonWheelDown,
        CKIUIButtonWheelLeft,
        CKIUIButtonWheelRight,
    } CKIUIButton;

    typedef enum CKIUITextKey
    {
        CKIUITextKeyUp,
        CKIUITextKeyDown,
        CKIUITextKeyLeft,
        CKIUITextKeyRight,
        CKIUITextKeyHome,
        CKIUITextKeyEnd,
        CKIUITextKeyPageUp,
        CKIUITextKeyPageDown,
        CKIUITextKeyDelete,
        CKIUITextKeyBackspace,
    } CKIUITextKey;

    typedef void (*CKIUIItemCallback)(void* context, const char* label);

    /* ---- Implemented by the C++ side (wxwidgets.cpp), called from QEMU ---- */

    void* CKIUICreate(const CKIUIOptions* options);
    int   CKIUIRun(void* app);
    void  CKIUIShutdown(void* app);

    /*
     * Takes ownership of one reference to `image` (a pixman_image_t*), which keeps `data` alive.
     * The UI side must hand it back with CKIUIReleaseSurface once it stops using `data`.
     */
    void CKIUISetSurface(void* app, void* image, const void* data, int width, int height, int stride);
    void CKIUIInvalidate(void* app, int x, int y, int width, int height);
    /* ARGB32 (straight alpha) pixels, copied by the callee. data == NULL clears the cursor. */
    void CKIUISetCursor(void* app, int width, int height, int hotX, int hotY, const uint32_t* data);
    void CKIUISetMousePosition(void* app, int x, int y, bool on);
    void CKIUISetMouseMode(void* app, bool absolute);
    /* UTF-8, not necessarily NUL terminated, copied by the callee. */
    void CKIUISetClipboardText(void* app, const char* text, size_t size);

    /* ---- Implemented by the C side (wxwidgets.c), called from the UI ---- */

    void CKIUIReleaseSurface(void* image);

    void CKIUIKeyEvent(const char* qcode, bool down);
    void CKIUIReleaseAllKeys(void);
    void CKIUITextKeyEvent(CKIUITextKey key, bool ctrl);
    void CKIUITextCharEvent(int character);
    bool CKIUIConsoleIsGraphic(void);

    void CKIUIPointerEvent(int x, int y, int width, int height);
    void CKIUIPointerRelativeEvent(int dx, int dy);
    void CKIUIButtonEvent(CKIUIButton button, bool down);

    void CKIUIRequestQuit(void);
    void CKIUIPause(void);
    void CKIUIResume(void);
    void CKIUIReset(void);
    void CKIUIPowerDown(void);

    void CKIUISelectConsole(int index);
    void CKIUIEnumerateConsoles(CKIUIItemCallback callback, void* context);
    void CKIUIEnumerateRemovableDevices(CKIUIItemCallback callback, void* context);
    bool CKIUIEjectMedia(const char* device, char* error, size_t errorSize);
    bool CKIUIChangeMedia(const char* device, const char* file, char* error, size_t errorSize);

    /* refreshHz == 0 means unknown. Sizes are in pixels. */
    void CKIUISetUIInfo(int width, int height, int widthMM, int heightMM, int refreshHz);

    /* Returns false if the clipboard peer is not ready yet (nothing was sent). */
    bool CKIUIClipboardChanged(const char* text, size_t size);

    bool        CKIUIGetIconPath(char* buffer, size_t size);
    const char* CKIUIVersion(void);
    const char* CKIUICopyright(void);
#ifdef __cplusplus
}
#endif
