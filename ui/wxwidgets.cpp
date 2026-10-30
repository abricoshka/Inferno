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
#include <wx/graphics.h>
#include <wx/mstream.h>
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
#include <utility>
#include <vector>

#if defined(__WXGTK__)
    #include <cairo.h>
#elif defined(__WXMSW__)
    #include <wx/msw/wrapwin.h>
using std::max;    // the GDI+ headers want these unqualified, wrapwin.h defines NOMINMAX
using std::min;
    #include <gdiplus.h>
#elif defined(__WXOSX__)
    #include <CoreGraphics/CoreGraphics.h>
#endif

#if defined(__WXOSX_COCOA__)
    #include <objc/message.h>
    #include <objc/runtime.h>
#endif

namespace
{

    constexpr const char* appName = "ChefKiss Inferno";
    const wxSize          minimumSize(160, 120);

    const char* namedQcode(int keyCode)
    {
        switch (keyCode) {
            case '1'            :
            case '!'            : return "1";
            case '2'            :
            case '@'            : return "2";
            case '3'            :
            case '#'            : return "3";
            case '4'            :
            case '$'            : return "4";
            case '5'            :
            case '%'            : return "5";
            case '6'            :
            case '^'            : return "6";
            case '7'            :
            case '&'            : return "7";
            case '8'            :
            case '*'            : return "8";
            case '9'            :
            case '('            : return "9";
            case '0'            :
            case ')'            : return "0";
            case '-'            :
            case '_'            : return "minus";
            case '='            :
            case '+'            : return "equal";
            case '['            :
            case '{'            : return "bracket_left";
            case ']'            :
            case '}'            : return "bracket_right";
            case ';'            :
            case ':'            : return "semicolon";
            case '\''           :
            case '"'            : return "apostrophe";
            case '`'            :
            case '~'            : return "grave_accent";
            case '\\'           :
            case '|'            : return "backslash";
            case ','            :
            case '<'            : return "comma";
            case '.'            :
            case '>'            : return "dot";
            case '/'            :
            case '?'            : return "slash";
            case WXK_BACK       : return "backspace";
            case WXK_TAB        : return "tab";
            case WXK_RETURN     : return "ret";
            case WXK_ESCAPE     : return "esc";
            case WXK_SPACE      : return "spc";
            case WXK_DELETE     : return "delete";
            case WXK_INSERT     : return "insert";
            case WXK_HOME       : return "home";
            case WXK_END        : return "end";
            case WXK_PAGEUP     : return "pgup";
            case WXK_PAGEDOWN   : return "pgdn";
            case WXK_LEFT       : return "left";
            case WXK_RIGHT      : return "right";
            case WXK_UP         : return "up";
            case WXK_DOWN       : return "down";
            case WXK_SHIFT      : return "shift";
            case WXK_ALT        : return "alt";
            case WXK_RAW_CONTROL: return "ctrl";
#if defined(__WXOSX__)
            case WXK_CONTROL: return "meta_l";    // Command
#endif
            case WXK_WINDOWS_LEFT   : return "meta_l";
            case WXK_WINDOWS_RIGHT  : return "meta_r";
            case WXK_WINDOWS_MENU   : return "menu";
            case WXK_CAPITAL        : return "caps_lock";
            case WXK_NUMLOCK        : return "num_lock";
            case WXK_SCROLL         :
            case WXK_F14            : return "scroll_lock";
            case WXK_PAUSE          :
            case WXK_F15            : return "pause";
            case WXK_PRINT          :
            case WXK_F13            : return "print";
            case WXK_NUMPAD_ADD     : return "kp_add";
            case WXK_NUMPAD_SUBTRACT: return "kp_subtract";
            case WXK_NUMPAD_MULTIPLY: return "kp_multiply";
            case WXK_NUMPAD_DIVIDE  : return "kp_divide";
            case WXK_NUMPAD_DECIMAL :
            case WXK_NUMPAD_DELETE  : return "kp_decimal";
            case WXK_NUMPAD_ENTER   : return "kp_enter";
            case WXK_NUMPAD_EQUAL   : return "kp_equals";
            case WXK_NUMPAD_INSERT  : return "kp_0";
            case WXK_NUMPAD_END     : return "kp_1";
            case WXK_NUMPAD_DOWN    : return "kp_2";
            case WXK_NUMPAD_PAGEDOWN: return "kp_3";
            case WXK_NUMPAD_LEFT    : return "kp_4";
            case WXK_NUMPAD_BEGIN   : return "kp_5";
            case WXK_NUMPAD_RIGHT   : return "kp_6";
            case WXK_NUMPAD_HOME    : return "kp_7";
            case WXK_NUMPAD_UP      : return "kp_8";
            case WXK_NUMPAD_PAGEUP  : return "kp_9";
            default                 : return "";
        }
    }

    std::string qcodeName(int keyCode)
    {
        if (keyCode >= 'A' && keyCode <= 'Z') { return std::string(1, static_cast<char>(keyCode - 'A' + 'a')); }
        if (keyCode >= WXK_F1 && keyCode <= WXK_F12) { return "f" + std::to_string(keyCode - WXK_F1 + 1); }
        if (keyCode >= WXK_NUMPAD0 && keyCode <= WXK_NUMPAD9) { return "kp_" + std::to_string(keyCode - WXK_NUMPAD0); }
        return namedQcode(keyCode);
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

    // The renderer whose native bitmaps wrapFramebuffer creates.
    wxGraphicsRenderer* framebufferRenderer()
    {
#if defined(__WXGTK__)
        return wxGraphicsRenderer::GetCairoRenderer();
#elif defined(__WXMSW__)
        return wxGraphicsRenderer::GetGDIPlusRenderer();
#else
        return wxGraphicsRenderer::GetDefaultRenderer();
#endif
    }

    // Wraps x8r8g8b8 pixels without copying them, they must outlive the drawing.
    wxGraphicsBitmap wrapFramebuffer(wxGraphicsRenderer* renderer, const void* data, int width, int height, int stride)
    {
        void* pixels = const_cast<void*>(data);
#if defined(__WXGTK__)
        return renderer->CreateBitmapFromNativeBitmap(cairo_image_surface_create_for_data(
            static_cast<unsigned char*>(pixels), CAIRO_FORMAT_RGB24, width, height, stride));
#elif defined(__WXMSW__)
        return renderer->CreateBitmapFromNativeBitmap(
            new Gdiplus::Bitmap(width, height, stride, PixelFormat32bppRGB, static_cast<BYTE*>(pixels)));
#elif defined(__WXOSX__)
        static const CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef            provider =
            CGDataProviderCreateWithData(nullptr, pixels, static_cast<size_t>(height) * stride, nullptr);
        CGImageRef image =
            CGImageCreate(width, height, 8, 32, stride, space, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                          provider, nullptr, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        return renderer->CreateBitmapFromNativeBitmap(image);
#else
    #error "No native bitmap wrapper for this wxWidgets port"
#endif
    }

    using ItemVisitor = std::function<void(const char*)>;

    void visitItem(void* context, const char* label) { (*static_cast<ItemVisitor*>(context))(label); }

    wxRect displayArea(const wxWindow* window)
    {
        const wxDisplay display(window);
        return display.IsOk() ? display.GetClientArea() : wxRect(wxGetDisplaySize());
    }

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

    void setNativeAspectRatio(wxTopLevelWindow* window, const wxSize& aspect)
    {
        if (aspect.x <= 0 || aspect.y <= 0) { return; }
        void* nsWindow = (void*)window->GetWXWindow();
        if (nsWindow == nullptr) { return; }
        objcSend<void>(nsWindow, "setContentAspectRatio:",
                       CGSizeMake(static_cast<CGFloat>(aspect.x), static_cast<CGFloat>(aspect.y)));
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

#if defined(__WXOSX__)
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

                const double au2   = au * au;
                const double av2   = av * av;
                const double f     = au2 * au2 * au + av2 * av2 * av;
                const double grad  = 5.0 / radius * std::sqrt(au2 * au2 * au2 * au2 + av2 * av2 * av2 * av2);
                const double dist  = grad < 1e-12 ? -1e9 : (f - 1.0) / grad;
                const double cover = std::clamp(0.5 - dist, 0.0, 1.0);
                const double ring  = 0.18 * std::clamp(strokeHalf + 0.5 - std::fabs(dist), 0.0, 1.0);

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
                    rgb[index * 3 + c] = static_cast<unsigned char>(std::clamp(std::round(value), 0.0, 255.0));
                }
                alpha[index] = static_cast<unsigned char>(std::clamp(std::round(outAlpha * 255.0), 0.0, 255.0));
            }
        }
        return out;
    }
#endif

}    // namespace

class CKIUICanvas : public wxWindow
{
public:
    CKIUICanvas(wxWindow* parent, bool hideCursor, bool smooth) :
        wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS),
        m_smooth(smooth),
        m_hideCursor(hideCursor)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(minimumSize));
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
        int                   width;
        int                   height;
        int                   hotX;
        int                   hotY;
        std::vector<uint32_t> pixels;
    };

    std::function<void(const wxSize&)> onModeChange;
    std::function<void(bool)>          onGrabChange;

    void setSurface(Framebuffer framebuffer)
    {
        void* superseded;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            superseded = m_pending.image;
            m_pending  = framebuffer;
            m_dirty    = wxRect();
        }
        if (superseded != nullptr) { CKIUIReleaseSurface(superseded); }
        CallAfter([this] { present(); });
    }

    void invalidate(int x, int y, int w, int h)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_dirty.Union(wxRect(x, y, w, h));
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

    bool paused() const { return m_paused; }

    void setPaused(bool paused)
    {
        m_paused = paused;
        Refresh(false);
    }

    bool smooth() const { return m_smooth; }

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

private:
    struct Placement
    {
        double ratio;
        double x;
        double y;
    };

    wxSize logicalSize() const
    { return wxSize(wxRound(m_framebuffer.width / m_scale), wxRound(m_framebuffer.height / m_scale)); }

    Placement placement() const
    {
        const wxSize client = GetClientSize();
        const wxSize source = m_framebuffer.data != nullptr ? logicalSize() : wxSize(0, 0);
        if (source.x <= 0 || source.y <= 0 || client.x <= 0 || client.y <= 0) { return {0, 0, 0}; }
        const double ratio =
            std::min(static_cast<double>(client.x) / source.x, static_cast<double>(client.y) / source.y);
        return {ratio, (client.x - source.x * ratio) / 2, (client.y - source.y * ratio) / 2};
    }

    static int scaleToRange(double fraction, int extent)
    { return std::max(0, std::min(extent - 1, static_cast<int>(fraction * extent))); }

    void present()
    {
        Framebuffer next;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pending.image == nullptr) { return; }
            next = std::exchange(m_pending, Framebuffer{});
        }
        if (m_framebuffer.image != nullptr) { CKIUIReleaseSurface(m_framebuffer.image); }

        // Page flips switch the surface every frame, only a mode change resizes the window.
        const double scale = GetContentScaleFactor();
        const bool   changed =
            next.width != m_framebuffer.width || next.height != m_framebuffer.height || scale != m_scale;
        m_framebuffer = next;
        m_scale       = scale;

        if (changed && m_framebuffer.width > 0 && m_framebuffer.height > 0 && onModeChange) {
            onModeChange(logicalSize());
        }
        Refresh(false);
    }

    void flush()
    {
        wxRect area;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_updateQueued = false;
            if (m_dirty.IsEmpty()) { return; }
            area    = m_dirty;
            m_dirty = wxRect();
        }
        area.Intersect(wxRect(0, 0, m_framebuffer.width, m_framebuffer.height));
        if (!area.IsEmpty()) { RefreshRect(windowRect(area), false); }
    }

    wxRect windowRect(const wxRect& source) const
    {
        const Placement place = placement();
        if (place.ratio <= 0) { return wxRect(); }
        const double k      = place.ratio / m_scale;
        const double margin = std::ceil(k) + 1;    // filtering samples neighbouring pixels
        const int    x0     = static_cast<int>(std::floor(place.x + source.x * k - margin));
        const int    y0     = static_cast<int>(std::floor(place.y + source.y * k - margin));
        const int    x1     = static_cast<int>(std::ceil(place.x + source.GetRight() * k + k + margin));
        const int    y1     = static_cast<int>(std::ceil(place.y + source.GetBottom() * k + k + margin));
        return wxRect(x0, y0, x1 - x0, y1 - y0);
    }

    void applyCursor(const std::shared_ptr<CursorImage>& image)
    {
        const wxRect before = cursorBounds();

        m_cursor       = image;
        m_cursorBitmap = wxBitmap();
        if (image) {
            wxImage converted(image->width, image->height, false);
            converted.InitAlpha();
            unsigned char* rgb   = converted.GetData();
            unsigned char* alpha = converted.GetAlpha();
            for (size_t i = 0; i < image->pixels.size(); ++i) {
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
        if (!m_mouseOn || !m_cursor || !m_cursorBitmap.IsOk()) { return false; }
        const Placement place = placement();
        if (place.ratio <= 0) { return false; }
        const double k = place.ratio / m_scale;
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
        wxPaintDC       dc(this);
        const Placement place = placement();
        if (place.ratio <= 0) {
            dc.SetBackground(*wxBLACK_BRUSH);
            dc.Clear();
            return;
        }

        const wxSize source = logicalSize();
        const wxSize client = GetClientSize();
        const int    left   = static_cast<int>(std::ceil(place.x));
        const int    top    = static_cast<int>(std::ceil(place.y));
        const int    right  = static_cast<int>(std::floor(place.x + source.x * place.ratio));
        const int    bottom = static_cast<int>(std::floor(place.y + source.y * place.ratio));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(*wxBLACK_BRUSH);
        if (left > 0) { dc.DrawRectangle(0, 0, left, client.y); }
        if (right < client.x) { dc.DrawRectangle(right, 0, client.x - right, client.y); }
        if (top > 0) { dc.DrawRectangle(0, 0, client.x, top); }
        if (bottom < client.y) { dc.DrawRectangle(0, bottom, client.x, client.y - bottom); }

        std::unique_ptr<wxGraphicsContext> gc(framebufferRenderer()->CreateContext(dc));
        if (!gc) { return; }

        gc->SetInterpolationQuality(m_smooth ? wxINTERPOLATION_FAST : wxINTERPOLATION_NONE);
        gc->DrawBitmap(wrapFramebuffer(gc->GetRenderer(), m_framebuffer.data, m_framebuffer.width, m_framebuffer.height,
                                       m_framebuffer.stride),
                       place.x, place.y, source.x * place.ratio, source.y * place.ratio);

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
            event.Skip();
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
        const Placement place = placement();
        if (place.ratio <= 0) { return; }

        const wxSize source = logicalSize();
        const int    width  = m_framebuffer.width;
        const int    height = m_framebuffer.height;
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
    Framebuffer m_pending;
    wxRect      m_dirty;
    bool        m_updateQueued{false};

    /* Main thread only */
    Framebuffer                  m_framebuffer;    // drawn from directly
    double                       m_scale{1};
    wxBitmap                     m_cursorBitmap;
    std::shared_ptr<CursorImage> m_cursor;
    int                          m_mouseX{0};
    int                          m_mouseY{0};
    bool                         m_mouseOn{false};
    bool                         m_paused{false};
    bool                         m_smooth;
    bool                         m_hideCursor;
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
        wxFrame(NULL, wxID_ANY, appName),
        m_name(name),
        m_zoomToFit(zoomToFit)
    {
        SetTitle(makeTitle(false));
        loadIcons();

        canvas = new CKIUICanvas(this, !showCursor, smooth);

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

        if (enable) { canvas->grab(); }
        else {
            canvas->ungrab();
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
        wxString title = appName;
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

        std::vector<std::string> removable;
        {
            ItemVisitor visitor = [&removable](const char* label) { removable.emplace_back(label); };
            CKIUIEnumerateRemovableDevices(&visitItem, &visitor);
        }
        if (!removable.empty()) {
            machine->AppendSeparator();
            machine->Append(wxID_ANY, "Removable Media")->Enable(false);
            for (const auto& device : removable) {
                const wxString label = escapeMenuLabel(wxString::FromUTF8(device));

                wxMenuItem* change = machine->Append(wxID_ANY, "Change " + label + "...");
                Bind(wxEVT_MENU, [this, device](wxCommandEvent&) { changeMedia(device); }, change->GetId());

                wxMenuItem* eject = machine->Append(wxID_ANY, "Eject " + label);
                Bind(wxEVT_MENU, [this, device](wxCommandEvent&) { ejectMedia(device); }, eject->GetId());
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
            ItemVisitor visitor = [&consoles](const char* label) { consoles.push_back(wxString::FromUTF8(label)); };
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
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setPaused(true); }, ID_PAUSE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setPaused(false); }, ID_RESUME);
        Bind(wxEVT_MENU, [](wxCommandEvent&) { CKIUIReset(); }, ID_RESET);
        Bind(wxEVT_MENU, [](wxCommandEvent&) { CKIUIPowerDown(); }, ID_POWER_DOWN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setFullScreen(!IsFullScreen()); }, ID_FULLSCREEN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setZoomToFit(!m_zoomToFit); }, ID_ZOOM_TO_FIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { canvas->setInterpolation(!canvas->smooth()); }, ID_INTERPOLATION);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { openDocumentation(); }, ID_DOCUMENTATION);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showAbout(); }, wxID_ABOUT);

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Enable(!canvas->paused()); }, ID_PAUSE);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Enable(canvas->paused()); }, ID_RESUME);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Check(IsFullScreen()); }, ID_FULLSCREEN);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Check(m_zoomToFit); }, ID_ZOOM_TO_FIT);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Check(canvas->smooth()); }, ID_INTERPOLATION);
    }

    void setPaused(bool paused)
    {
        paused ? CKIUIPause() : CKIUIResume();
        canvas->setPaused(paused);
    }

    void showError(const wxString& message) { wxMessageBox(message, appName, wxOK | wxICON_ERROR, this); }

    void ejectMedia(const std::string& device)
    {
        char error[512] = {};
        if (!CKIUIEjectMedia(device.c_str(), error, sizeof error)) { showError(wxString::FromUTF8(error)); }
    }

    void changeMedia(const std::string& device)
    {
        wxFileDialog dialog(this, "Select media image", wxEmptyString, wxEmptyString, wxFileSelectorDefaultWildcardStr,
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) { return; }

        const wxScopedCharBuffer path = dialog.GetPath().utf8_str();
        if (path.length() == 0) {
            showError("Failed to convert URL to file path!");
            return;
        }
        char error[512] = {};
        if (!CKIUIChangeMedia(device.c_str(), path.data(), error, sizeof error)) {
            showError(wxString::FromUTF8(error));
        }
    }

    void openDocumentation()
    {
        if (wxLaunchDefaultBrowser("https://chefkiss.dev/applehax/inferno/")) { return; }
        wxBell();
        showError("Failed to open website");
    }

    void showAbout()
    {
        wxDialog dialog(this, wxID_ANY, "About ChefKiss Inferno");
        auto*    sizer = new wxBoxSizer(wxVERTICAL);

        if (m_icon.IsOk()) {
            sizer->Add(new wxStaticBitmap(&dialog, wxID_ANY, wxBitmap(m_icon.Scale(128, 128, wxIMAGE_QUALITY_HIGH))), 0,
                       wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, 16);
        }

        auto*  title     = new wxStaticText(&dialog, wxID_ANY, appName);
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
            SetMinClientSize(FromDIP(minimumSize));
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
        setNativeAspectRatio(this, m_guestSize);
#endif
    }

#if defined(__WXMSW__)
    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override
    {
        if (message != WM_SIZING || !m_zoomToFit || IsFullScreen()) {
            return wxFrame::MSWWindowProc(message, wParam, lParam);
        }

        RECT&        rect    = *reinterpret_cast<RECT*>(lParam);
        const wxSize chrome  = GetSize() - GetClientSize();
        const wxSize minimum = FromDIP(minimumSize);
        double       scale   = wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM ?
                                   static_cast<double>(rect.bottom - rect.top - chrome.y) / m_guestSize.y :
                                   static_cast<double>(rect.right - rect.left - chrome.x) / m_guestSize.x;
        scale                = std::max(
            {scale, static_cast<double>(minimum.x) / m_guestSize.x, static_cast<double>(minimum.y) / m_guestSize.y});

        const wxSize frame(wxRound(m_guestSize.x * scale) + chrome.x, wxRound(m_guestSize.y * scale) + chrome.y);
        if (wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT) {
            rect.left = rect.right - frame.x;
        }
        else {
            rect.right = rect.left + frame.x;
        }
        if (wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT) {
            rect.top = rect.bottom - frame.y;
        }
        else {
            rect.bottom = rect.top + frame.y;
        }
        return TRUE;
    }
#endif

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
        const wxRect area = displayArea(this);
        const wxSize size = GetSize();
        const int    x    = std::max(area.x, std::min(centre.x - size.x / 2, area.x + area.width - size.x));
        const int    y    = std::max(area.y, std::min(centre.y - size.y / 2, area.y + area.height - size.y));
        SetPosition(wxPoint(x, y));
    }

    void setGuestSize(const wxSize& logical)
    {
        const bool keepSize = m_zoomToFit && GetClientSize() != m_guestSize;
        m_guestSize         = logical;
        if (keepSize) { snapToAspect(); }
        else {
            resizeToGuest();
        }
        applyConstraints();
        applyNativeAspect();
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
        if (IsMaximized()) {
            const wxSize client = GetClientSize();
            if (m_zoomToFit && !m_zoomFixQueued && m_guestSize.x > 0 && m_guestSize.y > 0 && client.x > 0
                && client.y > 0)
            {
                const double expected = static_cast<double>(client.x) * m_guestSize.y / m_guestSize.x;
                if (std::fabs(expected - client.y) > client.y * 0.01 + 1) {
                    m_zoomFixQueued = true;
                    CallAfter([this] { fitToDisplay(); });
                }
            }
            return;
        }
#else
        if (IsMaximized()) { return; }
        if (!m_zoomToFit && GetClientSize() != m_guestSize) { scheduleSettle(); }
#endif
    }

    void snapToAspect()
    {
        if (IsFullScreen() || IsMaximized()) { return; }

        const wxSize client = GetClientSize();
        const wxSize wanted = fitAspect(client, m_guestSize);
        if (std::abs(wanted.x - client.x) <= 1 && std::abs(wanted.y - client.y) <= 1) { return; }

        m_adjusting = true;
        SetClientSize(wanted);
        m_adjusting = false;
    }

    void fitToDisplay()
    {
        m_zoomFixQueued = false;
        if (IsFullScreen() || !IsMaximized()) { return; }

        const wxRect area   = displayArea(this);
        const wxSize chrome = GetSize() - GetClientSize();
        const wxSize fitted = fitAspect(area.GetSize() - chrome, m_guestSize);

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
        wxMessageDialog dialog(this, "Are you sure you want to quit?", appName,
                               wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        dialog.SetYesNoLabels("Quit", "Cancel");
        return dialog.ShowModal() == wxID_YES;
    }

    void onClose(wxCloseEvent& event)
    {
        if (!event.CanVeto()) {
            CKIUIRequestQuit();
            Hide();
            return;
        }
        if (!confirmQuit()) {
            event.Veto();
            return;
        }
        CKIUIRequestQuit();
        event.Veto();
    }

    std::string m_name;
    wxImage     m_icon;
    std::string m_lastClipboard;
    wxSize      m_guestSize{640, 480};
    bool        m_zoomToFit{true};
    bool        m_adjusting{false};
    bool        m_uiInfoQueued{false};
    bool        m_zoomFixQueued{false};
    bool        m_settleQueued{false};

    std::array<int, 5> m_lastUIInfo{};
};

class CKIUIApp : public wxApp
{
public:
    void configure(const CKIUIOptions& options) { m_options = options; }

    bool OnInit() override
    {
        wxInitAllImageHandlers();

        window = new CKIUIFrame(m_options.name != nullptr ? m_options.name : "", m_options.showCursor,
                                m_options.zoomToFit, m_options.zoomInterpolation);
        window->Show(true);
        if (m_options.fullScreen) { window->setFullScreen(true); }

        return true;
    }

    CKIUIFrame* window{nullptr};

private:
    CKIUIOptions m_options{};
};

wxIMPLEMENT_APP_NO_MAIN(CKIUIApp);

extern "C" void* CKIUICreate(const CKIUIOptions* options)
{
    const auto app = new CKIUIApp;
    app->configure(*options);
    static char  arg0[] = "inferno";
    static char* argv[] = {arg0, nullptr};
    int          argc   = 1;
    if (!wxEntryStart(argc, argv)) {
        fprintf(stderr, "wx init failed\n");
        exit(1);
    }
    app->CallOnInit();
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
    std::unique_lock<std::mutex> lock(loopMutex);
    for (;;) { loopCondition.wait(lock); }
}

extern "C" void CKIUIShutdown(void* app)
{
    if (!loopRunning || wxIsMainThread()) { return; }
    const auto ui = static_cast<CKIUIApp*>(app);
    ui->CallAfter([ui] { ui->ExitMainLoop(); });
    std::unique_lock<std::mutex> lock(loopMutex);
    loopCondition.wait(lock, [] { return loopExited; });
}

static CKIUIFrame* frameOf(void* app) { return static_cast<CKIUIApp*>(app)->window; }

extern "C" void CKIUISetSurface(void* app, void* image, const void* data, int width, int height, int stride)
{ frameOf(app)->canvas->setSurface({image, data, width, height, stride}); }

extern "C" void CKIUIInvalidate(void* app, int x, int y, int width, int height)
{ frameOf(app)->canvas->invalidate(x, y, width, height); }

extern "C" void CKIUISetCursor(void* app, int width, int height, int hotX, int hotY, const uint32_t* data)
{
    std::shared_ptr<CKIUICanvas::CursorImage> image;
    if (data != nullptr && width > 0 && height > 0) {
        image = std::make_shared<CKIUICanvas::CursorImage>(
            CKIUICanvas::CursorImage{width, height, hotX, hotY, {data, data + static_cast<size_t>(width) * height}});
    }
    frameOf(app)->canvas->setCursor(image);
}

extern "C" void CKIUISetMousePosition(void* app, int x, int y, bool on)
{ frameOf(app)->canvas->setMousePosition(x, y, on); }

extern "C" void CKIUISetMouseMode(void* app, bool absolute) { frameOf(app)->canvas->setMouseMode(absolute); }

extern "C" void CKIUISetClipboardText(void* app, const char* text, size_t size)
{ frameOf(app)->setGuestClipboard(std::string(text, size)); }
