/*
 * wxWidgets display backend.
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

#include "ui/wxwidgets-bridge.h"
#include <wx/wx.h>
#include <wx/clipbrd.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/mstream.h>
#include <wx/rawbmp.h>
#include <wx/stdpaths.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__WXOSX_COCOA__)
    #include <CoreGraphics/CGGeometry.h>
    #include <objc/message.h>
    #include <objc/runtime.h>
#endif

namespace
{

    struct CharacterKey
    {
        int         plain;
        int         shifted;
        const char* qcode;
    };

    constexpr CharacterKey characterKeys[] = {
        {'1', '!', "1"},
        {'2', '@', "2"},
        {'3', '#', "3"},
        {'4', '$', "4"},
        {'5', '%', "5"},
        {'6', '^', "6"},
        {'7', '&', "7"},
        {'8', '*', "8"},
        {'9', '(', "9"},
        {'0', ')', "0"},
        {'-', '_', "minus"},
        {'=', '+', "equal"},
        {'[', '{', "bracket_left"},
        {']', '}', "bracket_right"},
        {';', ':', "semicolon"},
        {'\'', '"', "apostrophe"},
        {'`', '~', "grave_accent"},
        {'\\', '|', "backslash"},
        {',', '<', "comma"},
        {'.', '>', "dot"},
        {'/', '?', "slash"},
    };

    const std::unordered_map<int, const char*>& namedKeys()
    {
        /*
         * Note: off macOS, WXK_CONTROL and WXK_RAW_CONTROL are the same value. The first entry wins
         * (initializer-list insertion does not overwrite), so "ctrl" is correct there, and on macOS
         * WXK_CONTROL (Command) becomes meta_l.
         */
        static const std::unordered_map<int, const char*> keys{
            {WXK_BACK, "backspace"},
            {WXK_TAB, "tab"},
            {WXK_RETURN, "ret"},
            {WXK_ESCAPE, "esc"},
            {WXK_SPACE, "spc"},
            {WXK_DELETE, "delete"},
            {WXK_INSERT, "insert"},
            {WXK_HOME, "home"},
            {WXK_END, "end"},
            {WXK_PAGEUP, "pgup"},
            {WXK_PAGEDOWN, "pgdn"},
            {WXK_LEFT, "left"},
            {WXK_RIGHT, "right"},
            {WXK_UP, "up"},
            {WXK_DOWN, "down"},
            {WXK_SHIFT, "shift"},
            {WXK_ALT, "alt"},
            {WXK_RAW_CONTROL, "ctrl"},
            {WXK_CONTROL, "meta_l"},
            {WXK_WINDOWS_LEFT, "meta_l"},
            {WXK_WINDOWS_RIGHT, "meta_r"},
            {WXK_WINDOWS_MENU, "menu"},
            {WXK_CAPITAL, "caps_lock"},
            {WXK_NUMLOCK, "num_lock"},
            {WXK_SCROLL, "scroll_lock"},
            {WXK_PAUSE, "pause"},
            {WXK_PRINT, "print"},
            {WXK_F13, "print"},
            {WXK_F14, "scroll_lock"},
            {WXK_F15, "pause"},
            {WXK_NUMPAD_ADD, "kp_add"},
            {WXK_NUMPAD_SUBTRACT, "kp_subtract"},
            {WXK_NUMPAD_MULTIPLY, "kp_multiply"},
            {WXK_NUMPAD_DIVIDE, "kp_divide"},
            {WXK_NUMPAD_DECIMAL, "kp_decimal"},
            {WXK_NUMPAD_ENTER, "kp_enter"},
            {WXK_NUMPAD_EQUAL, "kp_equals"},
            /* Numpad keys as reported while NumLock is off. */
            {WXK_NUMPAD_INSERT, "kp_0"},
            {WXK_NUMPAD_END, "kp_1"},
            {WXK_NUMPAD_DOWN, "kp_2"},
            {WXK_NUMPAD_PAGEDOWN, "kp_3"},
            {WXK_NUMPAD_LEFT, "kp_4"},
            {WXK_NUMPAD_BEGIN, "kp_5"},
            {WXK_NUMPAD_RIGHT, "kp_6"},
            {WXK_NUMPAD_HOME, "kp_7"},
            {WXK_NUMPAD_UP, "kp_8"},
            {WXK_NUMPAD_PAGEUP, "kp_9"},
            {WXK_NUMPAD_DELETE, "kp_decimal"},
        };
        return keys;
    }

    std::string qcodeName(int keyCode)
    {
        if (keyCode >= 'A' && keyCode <= 'Z') { return std::string(1, static_cast<char>(keyCode - 'A' + 'a')); }
        if (keyCode >= WXK_F1 && keyCode <= WXK_F12) { return "f" + std::to_string(keyCode - WXK_F1 + 1); }
        if (keyCode >= WXK_NUMPAD0 && keyCode <= WXK_NUMPAD9) { return "kp_" + std::to_string(keyCode - WXK_NUMPAD0); }
        for (const auto& key : characterKeys) {
            if (keyCode == key.plain || keyCode == key.shifted) { return key.qcode; }
        }
        const auto found = namedKeys().find(keyCode);
        return found == namedKeys().end() ? std::string() : std::string(found->second);
    }

    bool textKeyFor(int keyCode, CKIUITextKey& out)
    {
        switch (keyCode) {
            case WXK_UP      : out = CKIUITextKeyUp; return true;
            case WXK_DOWN    : out = CKIUITextKeyDown; return true;
            case WXK_LEFT    : out = CKIUITextKeyLeft; return true;
            case WXK_RIGHT   : out = CKIUITextKeyRight; return true;
            case WXK_HOME    : out = CKIUITextKeyHome; return true;
            case WXK_END     : out = CKIUITextKeyEnd; return true;
            case WXK_PAGEUP  : out = CKIUITextKeyPageUp; return true;
            case WXK_PAGEDOWN: out = CKIUITextKeyPageDown; return true;
            case WXK_DELETE  : out = CKIUITextKeyDelete; return true;
            case WXK_BACK    : out = CKIUITextKeyBackspace; return true;
            default          : return false;
        }
    }

    wxSize fitAspect(const wxSize& max, const wxSize& aspect)
    {
        if (aspect.x <= 0 || aspect.y <= 0 || max.x <= 0 || max.y <= 0) { return max; }
        const long long scaledWidth  = static_cast<long long>(aspect.x) * max.y;
        const long long scaledHeight = static_cast<long long>(aspect.y) * max.x;
        if (scaledWidth < scaledHeight) { return wxSize(wxRound(static_cast<double>(scaledWidth) / aspect.y), max.y); }
        return wxSize(max.x, wxRound(static_cast<double>(scaledHeight) / aspect.x));
    }

    using ItemVisitor = std::function<void(int, const char*)>;

    void visitItem(void* context, int index, const char* label) { (*static_cast<ItemVisitor*>(context))(index, label); }

    wxString escapeMenuLabel(const wxString& label)
    {
        wxString escaped = label;
        escaped.Replace("&", "&&");
        return escaped;
    }

#if defined(__WXOSX_COCOA__)
    template<typename Result, typename... Args>
    Result objcSend(void* target, const char* selector, Args... args)
    {
        using Function = Result (*)(void*, SEL, Args...);
        return reinterpret_cast<Function>(objc_msgSend)(target, sel_registerName(selector), args...);
    }

    void* objcClass(const char* name) { return reinterpret_cast<void*>(objc_getClass(name)); }

    /* Equivalent of -[NSWindow setContentAspectRatio:], so that macOS keeps the ratio while zooming ("Fill"). */
    bool setNativeAspectRatio(wxTopLevelWindow* window, const wxSize& aspect)
    {
        if (aspect.x <= 0 || aspect.y <= 0) { return false; }
        void* nsWindow = (void*)window->GetWXWindow();
        if (nsWindow == nullptr) { return false; }
        objcSend<void>(nsWindow, "setContentAspectRatio:",
                       CGSizeMake(static_cast<CGFloat>(aspect.x), static_cast<CGFloat>(aspect.y)));
        return true;
    }

    void setDockIcon(const wxImage& image)
    {
        wxMemoryOutputStream stream;
        if (!image.SaveFile(stream, wxBITMAP_TYPE_PNG)) { return; }
        std::vector<unsigned char> bytes(stream.GetSize());
        if (bytes.empty()) { return; }
        stream.CopyTo(bytes.data(), bytes.size());

        void* data =
            objcSend<void*>(objcClass("NSData"), "dataWithBytes:length:", static_cast<const void*>(bytes.data()),
                            static_cast<unsigned long>(bytes.size()));
        if (data == nullptr) { return; }
        void* nsImage = objcSend<void*>(objcSend<void*>(objcClass("NSImage"), "alloc"), "initWithData:", data);
        if (nsImage == nullptr) { return; }
        void* application = objcSend<void*>(objcClass("NSApplication"), "sharedApplication");
        objcSend<void>(application, "setApplicationIconImage:", nsImage);
        objcSend<void>(nsImage, "release");
    }
#endif

    /* Port of the macOS-style squircle app icon used by the Cocoa frontend. */
    wxImage makeSquircleIcon(const wxImage& source)
    {
        const int    side        = 1024;
        const double margin      = side * 0.10;
        const double visible     = side - margin * 2;
        const double glyphMargin = visible * 0.08;
        const double glyphSize   = visible - glyphMargin * 2;
        const double scale       = std::min(glyphSize / source.GetWidth(), glyphSize / source.GetHeight());
        const int    drawWidth   = std::max(1, wxRound(source.GetWidth() * scale));
        const int    drawHeight  = std::max(1, wxRound(source.GetHeight() * scale));
        const int    glyphX      = wxRound(side / 2.0 - drawWidth / 2.0);
        const int    glyphY      = wxRound(side / 2.0 - drawHeight / 2.0);

        const wxImage glyph = source.Scale(drawWidth, drawHeight, wxIMAGE_QUALITY_HIGH);

        wxImage out(side, side, false);
        out.InitAlpha();
        unsigned char*       rgb    = out.GetData();
        unsigned char*       alpha  = out.GetAlpha();
        const unsigned char* gRgb   = glyph.GetData();
        const unsigned char* gAlpha = glyph.HasAlpha() ? glyph.GetAlpha() : nullptr;

        const double radius        = visible / 2.0;
        const double centre        = side / 2.0;
        const double strokeHalf    = side * 0.01 / 2.0;
        const double background[3] = {0.17 * 255.0, 0.17 * 255.0, 0.18 * 255.0};

        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const int    index = y * side + x;
                const double au    = std::fabs((x + 0.5 - centre) / radius);
                const double av    = std::fabs((y + 0.5 - centre) / radius);

                /* Superellipse |u|^5 + |v|^5 = 1 with a first-order signed distance for anti-aliasing. */
                const double au2   = au * au;
                const double av2   = av * av;
                const double f     = au2 * au2 * au + av2 * av2 * av;
                const double grad  = 5.0 / radius * std::sqrt(au2 * au2 * au2 * au2 + av2 * av2 * av2 * av2);
                const double dist  = grad < 1e-12 ? -1e9 : (f - 1.0) / grad;
                const double cover = std::min(1.0, std::max(0.0, 0.5 - dist));
                const double ring  = 0.18 * std::min(1.0, std::max(0.0, strokeHalf + 0.5 - std::fabs(dist)));

                if (cover <= 0.0 && ring <= 0.0) {
                    rgb[index * 3 + 0] = rgb[index * 3 + 1] = rgb[index * 3 + 2] = 0;
                    alpha[index]                                                 = 0;
                    continue;
                }

                double colour[3] = {background[0], background[1], background[2]};
                if (x >= glyphX && x < glyphX + drawWidth && y >= glyphY && y < glyphY + drawHeight) {
                    const int    gIndex = (y - glyphY) * drawWidth + (x - glyphX);
                    const double sa     = gAlpha != nullptr ? gAlpha[gIndex] / 255.0 : 1.0;
                    for (int c = 0; c < 3; ++c) { colour[c] = gRgb[gIndex * 3 + c] * sa + background[c] * (1.0 - sa); }
                }

                double premultiplied[3];
                for (int c = 0; c < 3; ++c) { premultiplied[c] = colour[c] * cover * (1.0 - ring) + 255.0 * ring; }
                const double outAlpha = cover * (1.0 - ring) + ring;
                for (int c = 0; c < 3; ++c) {
                    const double value = outAlpha > 0.0 ? premultiplied[c] / outAlpha : 0.0;
                    rgb[index * 3 + c] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, std::round(value))));
                }
                alpha[index] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, std::round(outAlpha * 255.0))));
            }
        }
        return out;
    }

}    // namespace

class CKIUICanvas : public wxWindow
{
public:
    explicit CKIUICanvas(wxWindow* parent, bool hideCursor) :
        wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS),
        m_hideCursor(hideCursor)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(160, 120)));
#if defined(__WXMSW__)
        SetDoubleBuffered(true);
#endif

        Bind(wxEVT_PAINT, &CKIUICanvas::onPaint, this);
        Bind(wxEVT_KEY_DOWN, &CKIUICanvas::onKeyDown, this);
        Bind(wxEVT_KEY_UP, [this](wxKeyEvent& event) { sendKey(event, false); });
        Bind(wxEVT_CHAR, &CKIUICanvas::onChar, this);
        Bind(wxEVT_KILL_FOCUS, &CKIUICanvas::onKillFocus, this);
        Bind(wxEVT_MOTION, &CKIUICanvas::onMotion, this);
        Bind(wxEVT_ENTER_WINDOW, &CKIUICanvas::onEnter, this);
        Bind(wxEVT_LEAVE_WINDOW, &CKIUICanvas::onLeave, this);
        Bind(wxEVT_MOUSEWHEEL, &CKIUICanvas::onWheel, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { ungrab(); });
        bindButton(wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_LEFT_DCLICK, CKIUIButtonLeft);
        bindButton(wxEVT_MIDDLE_DOWN, wxEVT_MIDDLE_UP, wxEVT_MIDDLE_DCLICK, CKIUIButtonMiddle);
        bindButton(wxEVT_RIGHT_DOWN, wxEVT_RIGHT_UP, wxEVT_RIGHT_DCLICK, CKIUIButtonRight);
    }

    struct Framebuffer
    {
        void*       image{nullptr};    // pixman_image_t* reference owned by us
        const void* data{nullptr};
        int         width{0};
        int         height{0};
        int         stride{0};
    };

    struct CursorImage
    {
        int                   width{0};
        int                   height{0};
        int                   hotX{0};
        int                   hotY{0};
        std::vector<uint32_t> pixels;
    };

    std::function<void(const wxSize&)> onModeChange;
    std::function<void(bool)>          onGrabChange;

    void setSurface(Framebuffer framebuffer)
    {
        void* previous;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            previous      = m_framebuffer.image;
            m_framebuffer = framebuffer;
            m_hasDirty    = false;
        }
        if (previous != nullptr) { CKIUIReleaseSurface(previous); }
        CallAfter([this] { rebuild(); });
    }

    void invalidate(int x, int y, int w, int h)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const wxRect                rect(x, y, w, h);
            if (m_hasDirty) { m_dirty.Union(rect); }
            else {
                m_dirty    = rect;
                m_hasDirty = true;
            }
            if (m_updateQueued) { return; }
            m_updateQueued = true;
        }
        CallAfter([this] { flush(); });
    }

    void setCursor(const std::shared_ptr<CursorImage>& image)
    {
        CallAfter([this, image] { applyCursor(image); });
    }

    void setMousePosition(int x, int y, bool on)
    {
        CallAfter(
            [this, x, y, on]
            {
                const wxRect before = cursorBounds();
                m_mouseX            = x;
                m_mouseY            = y;
                m_mouseOn           = on;
                refreshRects(before, cursorBounds());
            });
    }

    void setMouseMode(bool absolute)
    {
        CallAfter([this, absolute] { applyMouseMode(absolute); });
    }

    void setPaused(bool paused)
    {
        m_paused = paused;
        Refresh(false);
    }

    void setInterpolation(bool smooth)
    {
        m_smooth = smooth;
        Refresh(false);
    }

    void releaseInput()
    {
        releaseKeys();
        ungrab();
    }

    void grabInput() { grab(); }
    void ungrabInput() { ungrab(); }

private:
    struct Placement
    {
        double ratio;
        double x;
        double y;
    };

    Placement placement() const
    {
        const wxSize client = GetClientSize();
        const wxSize source = m_bitmap.IsOk() ? m_bitmap.GetLogicalSize() : wxSize(0, 0);
        if (source.x <= 0 || source.y <= 0 || client.x <= 0 || client.y <= 0) { return {0, 0, 0}; }
        const double ratio =
            std::min(static_cast<double>(client.x) / source.x, static_cast<double>(client.y) / source.y);
        return {ratio, (client.x - source.x * ratio) / 2, (client.y - source.y * ratio) / 2};
    }

    static int scaleToRange(double fraction, int extent)
    { return std::max(0, std::min(extent - 1, static_cast<int>(fraction * extent))); }

    void rebuild()
    {
        wxSize size;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size = wxSize(m_framebuffer.width, m_framebuffer.height);
        }
        if (size.x == 0 || size.y == 0) { return; }

        const double scale = GetContentScaleFactor();
        m_bitmap.Create(size.x, size.y, 32);
        m_bitmap.SetScaleFactor(scale);
        upload(wxRect(0, 0, size.x, size.y));

        const wxSize logical(wxRound(size.x / scale), wxRound(size.y / scale));
        if (onModeChange) { onModeChange(logical); }
        Refresh(false);
    }

    void flush()
    {
        wxRect area;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_updateQueued = false;
            if (!m_hasDirty) { return; }
            area       = m_dirty;
            m_hasDirty = false;
        }
        upload(area);
        Refresh(false);
    }

    void upload(const wxRect& area)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bitmap.IsOk() || m_framebuffer.data == nullptr) { return; }

        const wxRect bounds(0, 0, std::min(m_bitmap.GetWidth(), m_framebuffer.width),
                            std::min(m_bitmap.GetHeight(), m_framebuffer.height));
        const wxRect clipped = area.Intersect(bounds);
        if (clipped.IsEmpty()) { return; }

        wxAlphaPixelData pixels(m_bitmap, clipped);
        if (!pixels) { return; }

        wxAlphaPixelData::Iterator rowStart(pixels);
        for (int y = 0; y < clipped.height; ++y) {
            const auto* source =
                reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m_framebuffer.data)
                                                  + static_cast<size_t>(clipped.y + y) * m_framebuffer.stride)
                + clipped.x;
            wxAlphaPixelData::Iterator pixel = rowStart;
            for (int x = 0; x < clipped.width; ++x, ++pixel) {
                const uint32_t value = source[x];
                pixel.Red()          = static_cast<uint8_t>(value >> 16);
                pixel.Green()        = static_cast<uint8_t>(value >> 8);
                pixel.Blue()         = static_cast<uint8_t>(value);
                pixel.Alpha()        = 255;
            }
            rowStart.OffsetY(pixels, 1);
        }
    }

    void applyCursor(const std::shared_ptr<CursorImage>& image)
    {
        const wxRect before = cursorBounds();

        m_cursor       = image;
        m_cursorBitmap = wxBitmap();
        if (image && image->width > 0 && image->height > 0
            && image->pixels.size() >= static_cast<size_t>(image->width) * image->height)
        {
            wxImage converted(image->width, image->height, false);
            converted.InitAlpha();
            unsigned char* rgb   = converted.GetData();
            unsigned char* alpha = converted.GetAlpha();
            const size_t   count = static_cast<size_t>(image->width) * image->height;
            for (size_t i = 0; i < count; ++i) {
                const uint32_t value = image->pixels[i];
                alpha[i]             = static_cast<unsigned char>(value >> 24);
                rgb[i * 3 + 0]       = static_cast<unsigned char>(value >> 16);
                rgb[i * 3 + 1]       = static_cast<unsigned char>(value >> 8);
                rgb[i * 3 + 2]       = static_cast<unsigned char>(value);
            }
            m_cursorBitmap = wxBitmap(converted, 32);
        }
        refreshRects(before, cursorBounds());
    }

    struct CursorGeometry
    { double x, y, width, height; };

    bool cursorGeometry(CursorGeometry& out) const
    {
        if (!m_mouseOn || !m_cursor || !m_cursorBitmap.IsOk() || !m_bitmap.IsOk()) { return false; }
        const Placement place = placement();
        if (place.ratio <= 0) { return false; }
        const double k = place.ratio / m_bitmap.GetScaleFactor();
        out            = {place.x + (m_mouseX - m_cursor->hotX) * k, place.y + (m_mouseY - m_cursor->hotY) * k,
                          m_cursor->width * k, m_cursor->height * k};
        return true;
    }

    wxRect cursorBounds() const
    {
        CursorGeometry geometry;
        if (!cursorGeometry(geometry)) { return wxRect(); }
        wxRect rect(static_cast<int>(std::floor(geometry.x)), static_cast<int>(std::floor(geometry.y)),
                    static_cast<int>(std::ceil(geometry.width)) + 1, static_cast<int>(std::ceil(geometry.height)) + 1);
        rect.Inflate(2);
        return rect;
    }

    void refreshRects(const wxRect& before, const wxRect& after)
    {
        if (!before.IsEmpty()) { RefreshRect(before, false); }
        if (!after.IsEmpty()) { RefreshRect(after, false); }
    }

    void onPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        if (!m_bitmap.IsOk()) { return; }

        const Placement place = placement();
        if (place.ratio <= 0) { return; }

        const wxSize                       source = m_bitmap.GetLogicalSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) {
            dc.SetUserScale(place.ratio, place.ratio);
            dc.DrawBitmap(m_bitmap, wxRound(place.x / place.ratio), wxRound(place.y / place.ratio));
            return;
        }

        gc->SetInterpolationQuality(m_smooth ? wxINTERPOLATION_FAST : wxINTERPOLATION_NONE);
        gc->DrawBitmap(m_bitmap, place.x, place.y, source.x * place.ratio, source.y * place.ratio);

        CursorGeometry cursor;
        if (cursorGeometry(cursor)) { gc->DrawBitmap(m_cursorBitmap, cursor.x, cursor.y, cursor.width, cursor.height); }

        if (m_paused) {
            wxFont font = GetFont();
            font.SetPointSize(48);
            gc->SetFont(font, *wxBLACK);
            const wxString text      = "Paused";
            wxDouble       textWidth = 0, textHeight = 0;
            gc->GetTextExtent(text, &textWidth, &textHeight);
            const double padding = 10;
            const double boxX    = (GetClientSize().x - textWidth) / 2 - padding;
            const double boxY    = textHeight * 0.5;
            gc->SetPen(*wxBLACK_PEN);
            gc->SetBrush(*wxWHITE_BRUSH);
            gc->DrawRectangle(boxX, boxY, textWidth + padding * 2, textHeight + padding * 2);
            gc->DrawText(text, boxX + padding, boxY + padding);
        }
    }

    bool handleHotkey(const wxKeyEvent& event)
    {
        /* Ctrl+Alt+1..9 selects a console and Ctrl+Alt+G releases the mouse, as in the Cocoa frontend. */
        if (!(event.RawControlDown() && event.AltDown())) { return false; }
        const int code = event.GetKeyCode();
        if (code >= '1' && code <= '9') {
            CKIUISelectConsole(code - '1');
            return true;
        }
        if (code == 'G') {
            ungrab();
            return true;
        }
        return false;
    }

    void onKeyDown(wxKeyEvent& event)
    {
        if (handleHotkey(event)) { return; }

        if (!CKIUIConsoleIsGraphic()) {
            CKIUITextKey key;
            if (textKeyFor(event.GetKeyCode(), key)) {
                CKIUITextKeyEvent(key, event.RawControlDown());
                return;
            }
            event.Skip();    // let wxEVT_CHAR deliver the character
            return;
        }
        sendKey(event, true);
    }

    void onChar(wxKeyEvent& event)
    {
        if (CKIUIConsoleIsGraphic()) { return; }

        int character = static_cast<int>(event.GetUnicodeKey());
        if (character == WXK_NONE) {
            character = event.GetKeyCode();
            if (character <= 0 || character >= WXK_START) { return; }
        }
        if (event.RawControlDown()
            && ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')))
        {
            character &= 0x1f;
        }
        CKIUITextCharEvent(character);
    }

    void sendKey(const wxKeyEvent& event, bool down)
    {
        const std::string name = qcodeName(event.GetKeyCode());
        if (name.empty()) { return; }

        if (down) { m_pressedKeys.insert(name); }
        else if (m_pressedKeys.erase(name) == 0) {
            return;
        }
        CKIUIKeyEvent(name.c_str(), down);
    }

    void releaseKeys()
    {
        m_pressedKeys.clear();
        CKIUIReleaseAllKeys();
    }

    void onKillFocus(wxFocusEvent& event)
    {
        releaseInput();
        event.Skip();
    }

    bool windowIsActive()
    {
        auto window = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
        return window != nullptr && window->IsActive();
    }

    void grab()
    {
        if (m_grabbed) { return; }
        m_grabbed = true;
        if (m_hideCursor) { SetCursor(wxCursor(wxCURSOR_BLANK)); }
        if (!m_absolute) { beginRelative(); }
        updateCapture();
        if (onGrabChange) { onGrabChange(true); }
    }

    void ungrab()
    {
        const bool wasGrabbed = m_grabbed;
        m_grabbed             = false;
        m_warping             = false;
        releaseButtons();
        if (wasGrabbed && m_hideCursor) { SetCursor(wxNullCursor); }
        updateCapture();
        if (wasGrabbed && onGrabChange) { onGrabChange(false); }
    }

    /* The mouse needs to be captured while a button is held, and for the whole grab in relative mode. */
    void updateCapture()
    {
        const bool needed = !m_heldButtons.empty() || (m_grabbed && !m_absolute);
        if (needed && !HasCapture()) { CaptureMouse(); }
        else if (!needed && HasCapture()) {
            ReleaseMouse();
        }
    }

    void applyMouseMode(bool absolute)
    {
        if (m_absolute == absolute) { return; }
        m_absolute = absolute;
        if (!m_grabbed) { return; }
        if (m_absolute) { ungrab(); }
        else {
            beginRelative();
            updateCapture();
        }
    }

    wxPoint centrePoint() const
    {
        const wxSize client = GetClientSize();
        return wxPoint(client.x / 2, client.y / 2);
    }

    void beginRelative()
    {
        m_lastPosition = centrePoint();
        recentre();
    }

    void recentre()
    {
        const wxPoint centre = centrePoint();
        m_warpTarget         = centre;
        m_warping            = true;
        m_warpIgnored        = 0;
        WarpPointer(centre.x, centre.y);
    }

    void relativeMove(const wxPoint& position)
    {
        if (m_warping) {
            /* Swallow motion events until the pointer warp is reported back. */
            if (position == m_warpTarget || ++m_warpIgnored >= 8) {
                m_warping      = false;
                m_warpIgnored  = 0;
                m_lastPosition = position;
            }
            return;
        }

        const wxPoint delta = position - m_lastPosition;
        m_lastPosition      = position;
        if (delta.x != 0 || delta.y != 0) { CKIUIPointerRelativeEvent(delta.x, delta.y); }

        const wxSize client = GetClientSize();
        if (std::abs(position.x - client.x / 2) > client.x / 4 || std::abs(position.y - client.y / 2) > client.y / 4) {
            recentre();
        }
    }

    void movePointer(const wxPoint& position)
    {
        if (!m_bitmap.IsOk()) { return; }

        const Placement place = placement();
        if (place.ratio <= 0) { return; }

        const wxSize source = m_bitmap.GetLogicalSize();
        const int    width  = m_bitmap.GetWidth();
        const int    height = m_bitmap.GetHeight();
        const double fx     = (position.x - place.x) / (source.x * place.ratio);
        const double fy     = (position.y - place.y) / (source.y * place.ratio);
        CKIUIPointerEvent(scaleToRange(fx, width), scaleToRange(fy, height), width, height);
    }

    void onMotion(wxMouseEvent& event)
    {
        if (m_absolute) {
            if (!m_grabbed) {
                if (!windowIsActive()) { return; }
                grab();
            }
            movePointer(event.GetPosition());
        }
        else if (m_grabbed) {
            relativeMove(event.GetPosition());
        }
    }

    void onEnter(wxMouseEvent&)
    {
        if (m_absolute && !m_grabbed && windowIsActive()) { grab(); }
    }

    void onLeave(wxMouseEvent&)
    {
        if (m_absolute && m_grabbed && m_heldButtons.empty()) { ungrab(); }
    }

    void bindButton(const wxEventTypeTag<wxMouseEvent>& down, const wxEventTypeTag<wxMouseEvent>& up,
                    const wxEventTypeTag<wxMouseEvent>& doubleClick, CKIUIButton button)
    {
        Bind(down, [this, button](wxMouseEvent& event) { pressButton(event, button); });
        Bind(doubleClick, [this, button](wxMouseEvent& event) { pressButton(event, button); });
        Bind(up, [this, button](wxMouseEvent& event) { releaseButton(event, button); });
    }

    void pressButton(const wxMouseEvent& event, CKIUIButton button)
    {
        SetFocus();
        if (!m_grabbed) {
            /* In relative mode the click that grabs is consumed; the grab happens on release. */
            if (!m_absolute) { return; }
            grab();
        }
        if (!m_heldButtons.insert(button).second) { return; }

        if (m_absolute) { movePointer(event.GetPosition()); }
        updateCapture();
        CKIUIButtonEvent(button, true);
    }

    void releaseButton(const wxMouseEvent& event, CKIUIButton button)
    {
        if (m_heldButtons.erase(button) == 0) {
            if (!m_grabbed && !m_absolute && button == CKIUIButtonLeft) { grab(); }
            return;
        }

        if (m_absolute) { movePointer(event.GetPosition()); }
        CKIUIButtonEvent(button, false);
        updateCapture();
        if (m_absolute && m_grabbed && m_heldButtons.empty() && !GetClientRect().Contains(event.GetPosition())) {
            ungrab();
        }
    }

    void releaseButtons()
    {
        for (const auto button : m_heldButtons) { CKIUIButtonEvent(button, false); }
        m_heldButtons.clear();
    }

    void onWheel(wxMouseEvent& event)
    {
        const int delta = event.GetWheelDelta();
        if (delta <= 0) { return; }

        const bool vertical  = event.GetWheelAxis() == wxMOUSE_WHEEL_VERTICAL;
        int&       remainder = vertical ? m_wheelRemainder : m_horizontalWheelRemainder;

        remainder         += event.GetWheelRotation();
        const int notches  = remainder / delta;
        remainder         -= notches * delta;
        if (notches == 0) { return; }

        const CKIUIButton button = vertical ? (notches > 0 ? CKIUIButtonWheelUp : CKIUIButtonWheelDown) :
                                              (notches > 0 ? CKIUIButtonWheelRight : CKIUIButtonWheelLeft);
        for (int i = 0; i < std::abs(notches); ++i) {
            CKIUIButtonEvent(button, true);
            CKIUIButtonEvent(button, false);
        }
    }

    /* Shared with the QEMU thread */
    std::mutex  m_mutex;
    Framebuffer m_framebuffer;
    wxRect      m_dirty;
    bool        m_hasDirty{false};
    bool        m_updateQueued{false};

    /* Main thread only */
    wxBitmap                     m_bitmap;
    wxBitmap                     m_cursorBitmap;
    std::shared_ptr<CursorImage> m_cursor;
    int                          m_mouseX{0};
    int                          m_mouseY{0};
    bool                         m_mouseOn{false};
    bool                         m_paused{false};
    bool                         m_smooth{true};
    bool                         m_hideCursor{false};
    bool                         m_absolute{false};
    bool                         m_grabbed{false};
    bool                         m_warping{false};
    int                          m_warpIgnored{0};
    wxPoint                      m_warpTarget;
    wxPoint                      m_lastPosition;
    std::set<std::string>        m_pressedKeys;
    std::set<CKIUIButton>        m_heldButtons;
    int                          m_wheelRemainder{0};
    int                          m_horizontalWheelRemainder{0};
};

class CKIUIFrame : public wxFrame
{
public:
    CKIUIFrame(const std::string& name, bool showCursor, bool zoomToFit, bool smooth) :
        wxFrame(NULL, wxID_ANY, "ChefKiss Inferno", wxDefaultPosition, wxDefaultSize),
        m_name(name),
        m_zoomToFit(zoomToFit),
        m_smooth(smooth)
    {
        SetTitle(makeTitle(false));
        loadIcons();

        canvas = new CKIUICanvas(this, !showCursor);

        const auto sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(canvas, 1, wxEXPAND);
        SetSizer(sizer);
        buildMenus();
        m_guestSize = FromDIP(m_guestSize);
        SetClientSize(m_guestSize);
        Center();

        canvas->onModeChange = [this](const wxSize& logical)
        {
            setGuestSize(logical);
            scheduleUIInfo();
        };
        canvas->onGrabChange = [this](bool grabbed) { SetTitle(makeTitle(grabbed)); };

        m_snapTimer.SetOwner(this);
        Bind(wxEVT_TIMER, &CKIUIFrame::onSnap, this);
        Bind(wxEVT_SIZE, &CKIUIFrame::onSize, this);
        Bind(wxEVT_CLOSE_WINDOW, &CKIUIFrame::onClose, this);
        Bind(wxEVT_ACTIVATE, &CKIUIFrame::onActivate, this);

#if wxCHECK_VERSION(3, 1, 5)
        if (wxTheApp != nullptr) {
            wxTheApp->Bind(wxEVT_CLIPBOARD_CHANGED,
                           [this](wxClipboardEvent& event)
                           {
                               event.Skip();
                               CallAfter([this] { checkHostClipboard(); });
                           });
        }
#endif

        applyConstraints();
        applyNativeAspect();
        scheduleSettle();
        canvas->SetFocus();
    }

    CKIUICanvas* canvas{nullptr};

    /* Called from the QEMU thread. */
    void setGuestClipboard(const std::string& text)
    {
        CallAfter([this, text] { applyGuestClipboard(text); });
    }

    void setFullScreen(bool enable)
    {
        if (enable == IsFullScreen()) { return; }
        if (enable) { clearConstraints(); }

        ShowFullScreen(enable, wxFULLSCREEN_NOTOOLBAR | wxFULLSCREEN_NOSTATUSBAR | wxFULLSCREEN_NOBORDER
                                   | wxFULLSCREEN_NOCAPTION);

        if (enable) { canvas->grabInput(); }
        else {
            canvas->ungrabInput();
            resizeToGuest();
        }
        applyConstraints();
        applyNativeAspect();
        scheduleSettle();
        scheduleUIInfo();
    }

private:
    enum : std::uint16_t
    {
        ID_PAUSE = wxID_HIGHEST + 1,
        ID_RESUME,
        ID_RESET,
        ID_POWER_DOWN,
        ID_FULLSCREEN,
        ID_ZOOM_TO_FIT,
        ID_INTERPOLATION,
        ID_DOCUMENTATION,
    };

    wxString makeTitle(bool grabbed) const
    {
        wxString title = "ChefKiss Inferno";
        if (!m_name.empty()) { title += " " + wxString::FromUTF8(m_name.c_str()); }
        if (grabbed) {
#if defined(__WXOSX__)
            title += wxString::FromUTF8(" - (Press  \xe2\x8c\x83 \xe2\x8c\xa5 G  to release Mouse)");
#else
            title += " - (Press Ctrl+Alt+G to release Mouse)";
#endif
        }
        return title;
    }

    void loadIcons()
    {
        char path[4096] = {};
        if (!CKIUIGetIconPath(path, sizeof path)) { return; }

        wxImage source;
        if (!source.LoadFile(wxString::FromUTF8(path), wxBITMAP_TYPE_PNG) || !source.IsOk()) { return; }

#if defined(__WXOSX__)
        m_icon = makeSquircleIcon(source);
#else
        m_icon = source;
#endif
        if (!m_icon.IsOk()) { return; }

        wxIcon windowIcon;
        windowIcon.CopyFromBitmap(wxBitmap(m_icon.Scale(256, 256, wxIMAGE_QUALITY_HIGH)));
        SetIcon(windowIcon);
#if defined(__WXOSX_COCOA__)
        setDockIcon(m_icon);
#endif
    }

    void buildMenus()
    {
        wxMenuBar* menubar = new wxMenuBar;

        wxMenu* machine = new wxMenu;
        machine->Append(ID_PAUSE, "&Pause");
        machine->Append(ID_RESUME, "&Resume");
        machine->AppendSeparator();
        machine->Append(ID_RESET, "R&eset");
        machine->Append(ID_POWER_DOWN, "Power &Down");

        std::vector<std::pair<std::string, wxString>> removable;
        {
            ItemVisitor visitor = [&removable](int, const char* label)
            { removable.emplace_back(label, wxString::FromUTF8(label)); };
            CKIUIEnumerateRemovableDevices(&visitItem, &visitor);
        }
        if (!removable.empty()) {
            machine->AppendSeparator();
            machine->Append(wxID_ANY, "Removable Media")->Enable(false);
            for (const auto& device : removable) {
                const std::string identifier = device.first;

                wxMenuItem* change = machine->Append(wxID_ANY, "Change " + escapeMenuLabel(device.second) + "...");
                Bind(wxEVT_MENU, [this, identifier](wxCommandEvent&) { changeMedia(identifier); }, change->GetId());

                wxMenuItem* eject = machine->Append(wxID_ANY, "Eject " + escapeMenuLabel(device.second));
                Bind(wxEVT_MENU, [this, identifier](wxCommandEvent&) { ejectMedia(identifier); }, eject->GetId());
            }
        }
        menubar->Append(machine, "&Machine");

        wxMenu* view = new wxMenu;
#if defined(__WXOSX__)
        view->AppendCheckItem(ID_FULLSCREEN, "Enter &Fullscreen\tCtrl+RawCtrl+F");
#else
        view->AppendCheckItem(ID_FULLSCREEN, "&Fullscreen\tF11");
#endif
        view->AppendCheckItem(ID_ZOOM_TO_FIT, "&Zoom To Fit");
        view->AppendCheckItem(ID_INTERPOLATION, "Zoom &Interpolation");

        std::vector<wxString> consoles;
        {
            ItemVisitor visitor = [&consoles](int, const char* label)
            { consoles.push_back(wxString::FromUTF8(label)); };
            CKIUIEnumerateConsoles(&visitItem, &visitor);
        }
        if (!consoles.empty()) {
            view->AppendSeparator();
            for (size_t index = 0; index < consoles.size(); ++index) {
                wxMenuItem* item = view->Append(wxID_ANY, escapeMenuLabel(consoles[index]));
                const int   id   = static_cast<int>(index);
                Bind(wxEVT_MENU, [id](wxCommandEvent&) { CKIUISelectConsole(id); }, item->GetId());
            }
        }
        menubar->Append(view, "&View");

        wxMenu* help = new wxMenu;
        help->Append(ID_DOCUMENTATION, "Inferno &Documentation");
        help->Append(wxID_ABOUT, "&About ChefKiss Inferno");
        menubar->Append(help, "&Help");

        SetMenuBar(menubar);

        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(false); }, wxID_EXIT);
        Bind(
            wxEVT_MENU,
            [this](wxCommandEvent&)
            {
                CKIUIPause();
                m_paused = true;
                canvas->setPaused(true);
            },
            ID_PAUSE);
        Bind(
            wxEVT_MENU,
            [this](wxCommandEvent&)
            {
                CKIUIResume();
                m_paused = false;
                canvas->setPaused(false);
            },
            ID_RESUME);
        Bind(wxEVT_MENU, [](wxCommandEvent&) { CKIUIReset(); }, ID_RESET);
        Bind(wxEVT_MENU, [](wxCommandEvent&) { CKIUIPowerDown(); }, ID_POWER_DOWN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setFullScreen(!IsFullScreen()); }, ID_FULLSCREEN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setZoomToFit(!m_zoomToFit); }, ID_ZOOM_TO_FIT);
        Bind(
            wxEVT_MENU,
            [this](wxCommandEvent&)
            {
                m_smooth = !m_smooth;
                canvas->setInterpolation(m_smooth);
            },
            ID_INTERPOLATION);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { openDocumentation(); }, ID_DOCUMENTATION);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showAbout(); }, wxID_ABOUT);

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Enable(!m_paused); }, ID_PAUSE);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Enable(m_paused); }, ID_RESUME);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Check(IsFullScreen()); }, ID_FULLSCREEN);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Check(m_zoomToFit); }, ID_ZOOM_TO_FIT);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Check(m_smooth); }, ID_INTERPOLATION);
    }

    void showError(const char* message)
    { wxMessageBox(wxString::FromUTF8(message), "ChefKiss Inferno", wxOK | wxICON_ERROR, this); }

    void ejectMedia(const std::string& device)
    {
        char error[512] = {};
        if (!CKIUIEjectMedia(device.c_str(), error, sizeof error)) { showError(error); }
    }

    void changeMedia(const std::string& device)
    {
        wxFileDialog dialog(this, "Select media image", wxEmptyString, wxEmptyString, wxFileSelectorDefaultWildcardStr,
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) { return; }

        const wxScopedCharBuffer path = dialog.GetPath().utf8_str();
        if (path.length() == 0) {
            wxMessageBox("Failed to convert URL to file path!", "ChefKiss Inferno", wxOK | wxICON_ERROR, this);
            return;
        }
        char error[512] = {};
        if (!CKIUIChangeMedia(device.c_str(), path.data(), error, sizeof error)) { showError(error); }
    }

    void openDocumentation()
    {
        if (wxLaunchDefaultBrowser("https://chefkiss.dev/applehax/inferno/")) { return; }
        wxBell();
        wxMessageBox("Failed to open website", "ChefKiss Inferno", wxOK | wxICON_ERROR, this);
    }

    void showAbout()
    {
        wxDialog dialog(this, wxID_ANY, "About ChefKiss Inferno");
        auto*    sizer = new wxBoxSizer(wxVERTICAL);

        if (m_icon.IsOk()) {
            sizer->Add(new wxStaticBitmap(&dialog, wxID_ANY, wxBitmap(m_icon.Scale(128, 128, wxIMAGE_QUALITY_HIGH))), 0,
                       wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, 16);
        }

        auto*  title     = new wxStaticText(&dialog, wxID_ANY, "ChefKiss Inferno");
        wxFont titleFont = title->GetFont();
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        titleFont.SetPointSize(titleFont.GetPointSize() + 4);
        title->SetFont(titleFont);
        sizer->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, 12);

        const wxString version = "ChefKiss Inferno emulator version " + wxString::FromUTF8(CKIUIVersion());
        sizer->Add(new wxStaticText(&dialog, wxID_ANY, version), 0,
                   wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, 8);

        const wxString copyright = "Copyright (c) 2023-2026 ChefKiss and the Inferno team"
                                   "\n\nQEMU emulator\n"
                                   + wxString::FromUTF8(CKIUICopyright());
        sizer->AddSpacer(12);
        for (const wxString& line : wxSplit(copyright, '\n', '\0')) {
            sizer->Add(new wxStaticText(&dialog, wxID_ANY, line.IsEmpty() ? wxString(" ") : line), 0,
                       wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, 12);
        }

        if (wxSizer* buttons = dialog.CreateStdDialogButtonSizer(wxOK)) {
            sizer->Add(buttons, 0, wxEXPAND | wxALL, 12);
        }

        dialog.SetSizerAndFit(sizer);
        dialog.CentreOnParent();
        dialog.ShowModal();
    }

    void checkHostClipboard()
    {
        std::string text;
        {
            wxClipboardLocker locker;
            if (!locker) { return; }
            if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
                wxTextDataObject data;
                if (wxTheClipboard->GetData(data)) { text = data.GetText().utf8_string(); }
            }
        }
        if (text == m_lastClipboard) { return; }
        if (CKIUIClipboardChanged(text.data(), text.size())) { m_lastClipboard = text; }
    }

    void applyGuestClipboard(const std::string& text)
    {
        if (text == m_lastClipboard) { return; }
        wxClipboardLocker locker;
        if (!locker) { return; }
        /* Remember it first so the resulting change notification is not echoed back to the guest. */
        m_lastClipboard = text;
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text.data(), text.size())));
    }

    void onActivate(wxActivateEvent& event)
    {
        event.Skip();
        if (event.GetActive()) {
            CallAfter([this] { checkHostClipboard(); });
        }
        else {
            canvas->releaseInput();
        }
    }

    void clearConstraints()
    {
        SetMinClientSize(wxDefaultSize);
        SetMaxClientSize(wxDefaultSize);
    }

    void applyConstraints()
    {
        if (IsFullScreen()) {
            clearConstraints();
            return;
        }
        if (m_zoomToFit) {
            SetMinClientSize(FromDIP(wxSize(160, 120)));
            SetMaxClientSize(wxDefaultSize);
        }
        else {
            SetMinClientSize(m_guestSize);
            SetMaxClientSize(m_guestSize);
        }
    }

    void applyNativeAspect()
    {
#if defined(__WXOSX_COCOA__)
        m_nativeAspect = setNativeAspectRatio(this, m_guestSize);
#endif
    }

    void resizeToGuest()
    {
        if (IsFullScreen() || IsMaximized()) { return; }
        const wxRect  before = GetRect();
        const wxPoint centre(before.x + before.width / 2, before.y + before.height / 2);

        m_adjusting = true;
        clearConstraints();
        SetClientSize(m_guestSize);
        keepCentred(centre);
        m_adjusting = false;
        applyConstraints();
    }

    void keepCentred(const wxPoint& centre)
    {
        const wxDisplay display(this);
        const wxRect    area = display.IsOk() ? display.GetClientArea() : wxRect(wxGetDisplaySize());
        const wxSize    size = GetSize();
        const int       x    = std::max(area.x, std::min(centre.x - size.x / 2, area.x + area.width - size.x));
        const int       y    = std::max(area.y, std::min(centre.y - size.y / 2, area.y + area.height - size.y));
        SetPosition(wxPoint(x, y));
    }

    void setGuestSize(const wxSize& logical)
    {
        const bool keepSize = m_zoomToFit && GetClientSize() != m_guestSize;
        m_guestSize         = logical;
        m_aspect            = logical;
        applyNativeAspect();
        if (keepSize) { snapToAspect(); }
        else {
            resizeToGuest();
        }
        applyConstraints();
        scheduleSettle();
    }

    void setZoomToFit(bool enable)
    {
        m_zoomToFit = enable;
        if (!enable) { resizeToGuest(); }
        applyConstraints();
        scheduleSettle();
    }

    void scheduleSettle()
    {
        if (m_settleQueued) { return; }
        m_settleQueued = true;
        CallAfter([this] { settle(); });
    }

    void settle()
    {
        m_settleQueued = false;
        applyConstraints();
        applyNativeAspect();
        Layout();
        scheduleUIInfo();
    }

    void onSize(wxSizeEvent& event)
    {
        event.Skip();
        scheduleUIInfo();

        if (m_adjusting || IsFullScreen()) { return; }

#if defined(__WXOSX__)
        /*
         * Safety net for "Window > Fill": the native content aspect ratio should already make macOS zoom to an
         * aspect-correct frame; if the zoomed frame still doesn't match, leave the zoomed state and fit manually.
         */
        if (IsMaximized()) {
            const wxSize client = GetClientSize();
            if (m_zoomToFit && !m_zoomFixQueued && m_aspect.x > 0 && m_aspect.y > 0 && client.x > 0 && client.y > 0) {
                const double expected = static_cast<double>(client.x) * m_aspect.y / m_aspect.x;
                if (std::fabs(expected - client.y) > client.y * 0.01 + 1) {
                    m_zoomFixQueued = true;
                    CallAfter([this] { fitToDisplay(); });
                }
            }
            return;
        }

        if (m_nativeAspect || !m_zoomToFit || m_aspect.x <= 0 || m_aspect.y <= 0) { return; }

        const wxSize client = GetClientSize();
        const wxSize wanted(client.x, wxRound(static_cast<double>(client.x) * m_aspect.y / m_aspect.x));
        if (std::abs(wanted.y - client.y) <= 1) { return; }

        m_adjusting = true;
        SetClientSize(wanted);
        m_adjusting = false;
#else
        if (IsMaximized()) { return; }
        if (m_zoomToFit) { m_snapTimer.StartOnce(150); }
        else if (GetClientSize() != m_guestSize) {
            scheduleSettle();
        }
#endif
    }

    void onSnap(wxTimerEvent&)
    {
        if (IsFullScreen() || IsMaximized() || !m_zoomToFit) { return; }

        const wxMouseState mouse = wxGetMouseState();
        if (mouse.LeftIsDown() || mouse.MiddleIsDown() || mouse.RightIsDown()) {
            m_snapTimer.StartOnce(150);
            return;
        }
        snapToAspect();
    }

    void snapToAspect()
    {
        if (IsFullScreen() || IsMaximized()) { return; }

        const wxSize client = GetClientSize();
        const wxSize wanted = fitAspect(client, m_aspect);
        if (std::abs(wanted.x - client.x) <= 1 && std::abs(wanted.y - client.y) <= 1) { return; }

        m_adjusting = true;
        SetClientSize(wanted);
        m_adjusting = false;
    }

    void fitToDisplay()
    {
        m_zoomFixQueued = false;
        if (IsFullScreen() || !IsMaximized()) { return; }

        const wxDisplay display(this);
        const wxRect    area   = display.IsOk() ? display.GetClientArea() : wxRect(wxGetDisplaySize());
        const wxSize    chrome = GetSize() - GetClientSize();
        const wxSize    fitted = fitAspect(wxSize(area.width - chrome.x, area.height - chrome.y), m_aspect);

        m_adjusting = true;
        Maximize(false);
        SetClientSize(fitted);
        const wxSize frame = GetSize();
        SetPosition(wxPoint(area.x + (area.width - frame.x) / 2, area.y + (area.height - frame.y) / 2));
        m_adjusting = false;
    }

    void scheduleUIInfo()
    {
        if (m_uiInfoQueued) { return; }
        m_uiInfoQueued = true;
        CallAfter([this] { sendUIInfo(); });
    }

    void sendUIInfo()
    {
        m_uiInfoQueued = false;

        const wxSize client = canvas->GetClientSize();
        if (client.x <= 0 || client.y <= 0) { return; }

        const double scale    = GetContentScaleFactor();
        const wxSize screen   = wxGetDisplaySize();
        const wxSize screenMM = wxGetDisplaySizeMM();

        const int widthMM  = screen.x > 0 ? wxRound(static_cast<double>(client.x) / screen.x * screenMM.x) : 0;
        const int heightMM = screen.y > 0 ? wxRound(static_cast<double>(client.y) / screen.y * screenMM.y) : 0;

        int             refresh = 0;
        const wxDisplay display(this);
        if (display.IsOk()) { refresh = display.GetCurrentMode().refresh; }

        const std::array<int, 5> info{wxRound(client.x * scale), wxRound(client.y * scale), widthMM, heightMM,
                                      std::max(refresh, 0)};
        if (info == m_lastUIInfo) { return; }
        m_lastUIInfo = info;
        CKIUISetUIInfo(info[0], info[1], info[2], info[3], info[4]);
    }

    bool confirmQuit()
    {
        wxMessageDialog dialog(this, "Are you sure you want to quit?", "ChefKiss Inferno",
                               wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        dialog.SetYesNoLabels("Quit", "Cancel");
        return dialog.ShowModal() == wxID_YES;
    }

    void onClose(wxCloseEvent& event)
    {
        if (!event.CanVeto()) {
            /* Session is ending: no confirmation. Keep the objects alive for the QEMU thread. */
            CKIUIRequestQuit();
            Hide();
            return;
        }
        if (!confirmQuit()) {
            event.Veto();
            return;
        }
        CKIUIRequestQuit();
        event.Veto();    // QEMU's main loop shuts the process down
    }

    std::string m_name;
    wxImage     m_icon;
    std::string m_lastClipboard;
    wxSize      m_guestSize{640, 480};
    wxSize      m_aspect{640, 480};
    bool        m_zoomToFit{true};
    bool        m_smooth{true};
    bool        m_paused{false};
    bool        m_nativeAspect{false};
    bool        m_adjusting{false};
    bool        m_uiInfoQueued{false};
    bool        m_zoomFixQueued{false};
    bool        m_settleQueued{false};
    wxTimer     m_snapTimer;

    std::array<int, 5> m_lastUIInfo{};
};

class CKIUIApp : public wxApp
{
public:
    void configure(const CKIUIOptions& options)
    {
        m_name              = options.name != nullptr ? options.name : "";
        m_fullScreen        = options.fullScreen;
        m_showCursor        = options.showCursor;
        m_zoomToFit         = options.zoomToFit;
        m_zoomInterpolation = options.zoomInterpolation;
    }

    bool OnInit() override
    {
        wxInitAllImageHandlers();

        window = new CKIUIFrame(m_name, m_showCursor, m_zoomToFit, m_zoomInterpolation);
        window->Show(true);
        if (m_fullScreen) { window->setFullScreen(true); }

        return true;
    }

    CKIUIFrame* window{nullptr};

private:
    std::string m_name;
    bool        m_fullScreen{false};
    bool        m_showCursor{false};
    bool        m_zoomToFit{false};
    bool        m_zoomInterpolation{false};
};

wxIMPLEMENT_APP_NO_MAIN(CKIUIApp);

extern "C" void* CKIUICreate(const CKIUIOptions* options)
{
    const auto app = new CKIUIApp;
    if (options != nullptr) { app->configure(*options); }
    static char  arg0[] = "inferno";
    static char* argv[] = {arg0, nullptr};
    int          argc   = 1;
    if (!wxEntryStart(argc, argv)) {
        fprintf(stderr, "wx init failed\n");
        exit(1);
    }
    static_cast<CKIUIApp*>(app)->CallOnInit();
    return app;
}

namespace
{

    std::mutex              loopMutex;
    std::condition_variable loopCondition;
    std::atomic<bool>       loopRunning{false};
    bool                    loopExited{false};

}    // namespace

extern "C" int CKIUIRun(void* app)
{
    loopRunning = true;
    static_cast<CKIUIApp*>(app)->MainLoop();
    {
        std::lock_guard<std::mutex> lock(loopMutex);
        loopExited = true;
    }
    loopCondition.notify_all();
    for (;;) { pause(); }
}

extern "C" void CKIUIShutdown(void* app)
{
    if (!loopRunning || wxIsMainThread()) { return; }
    const auto ui = static_cast<CKIUIApp*>(app);
    ui->CallAfter([ui] { ui->ExitMainLoop(); });
    std::unique_lock<std::mutex> lock(loopMutex);
    loopCondition.wait(lock, [] { return loopExited; });
}

extern "C" void CKIUISetSurface(void* app, void* image, const void* data, int width, int height, int stride)
{ static_cast<CKIUIApp*>(app)->window->canvas->setSurface({image, data, width, height, stride}); }

extern "C" void CKIUIInvalidate(void* app, int x, int y, int width, int height)
{ static_cast<CKIUIApp*>(app)->window->canvas->invalidate(x, y, width, height); }

extern "C" void CKIUISetCursor(void* app, int width, int height, int hotX, int hotY, const uint32_t* data)
{
    auto image = std::make_shared<CKIUICanvas::CursorImage>();
    if (data != nullptr && width > 0 && height > 0) {
        image->width  = width;
        image->height = height;
        image->hotX   = hotX;
        image->hotY   = hotY;
        image->pixels.assign(data, data + static_cast<size_t>(width) * height);
    }
    else {
        image.reset();
    }
    static_cast<CKIUIApp*>(app)->window->canvas->setCursor(image);
}

extern "C" void CKIUISetMousePosition(void* app, int x, int y, bool on)
{ static_cast<CKIUIApp*>(app)->window->canvas->setMousePosition(x, y, on); }

extern "C" void CKIUISetMouseMode(void* app, bool absolute)
{ static_cast<CKIUIApp*>(app)->window->canvas->setMouseMode(absolute); }

extern "C" void CKIUISetClipboardText(void* app, const char* text, size_t size)
{ static_cast<CKIUIApp*>(app)->window->setGuestClipboard(std::string(text, size)); }
