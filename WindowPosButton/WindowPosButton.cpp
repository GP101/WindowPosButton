// WindowPosButton
// Adds three floating buttons to the left of the Minimize button on every
// normal top-level window's title bar: one directly places the window on the
// left half of the screen, one on the right half, and one moves the window to
// the next
// monitor in the current display arrangement, wrapping after the last one.

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <taskschd.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "resource.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "shcore.lib")

namespace {

constexpr wchar_t kOverlayClassName[] = L"WindowPosButton_Overlay";
constexpr wchar_t kMainClassName[] = L"WindowPosButton_Main";
constexpr wchar_t kMutexName[] = L"Local\\WindowPosButton_SingleInstance";
constexpr wchar_t kStartupTaskName[] = L"WindowPosButton";
constexpr wchar_t kLegacyRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

constexpr UINT WM_TRAYICON = WM_APP + 1;
// Delivers a completed UpdateResult* from a one-shot measurement thread back
// to the main thread. wParam is the target HWND, lParam is the UpdateResult*
// (heap-allocated by the measurement thread, owned/freed by the handler).
constexpr UINT WM_OVERLAY_RESULT = WM_APP + 2;
constexpr UINT ID_TRAY_ABOUT = 1;
constexpr UINT ID_TRAY_STARTUP = 3;
constexpr UINT ID_TRAY_EXIT = 2;
constexpr UINT ID_TRAY_HIDE_REPOSITION = 4;
constexpr UINT ID_TRAY_ICON = 1;

// Recurring timer id (SetTimer on g_hMain). Distinct from the per-window
// retry timers elsewhere, which use the target HWND's own value as their id
// — a real HWND is never equal to this small constant in practice.
//
// This is now just a safety net: EVENT_OBJECT_REORDER (hooked below) fires
// whenever the desktop's top-level Z order changes and drives the same
// reassertion event-driven, at effectively no idle cost. The timer only
// covers the case where, for whatever reason, no REORDER event shows up.
constexpr UINT_PTR kZOrderReassertTimerId = 1;
constexpr UINT kZOrderReassertIntervalMs = 2000;
constexpr UINT_PTR kTrayRecoveryTimerId = 2;
constexpr UINT kTrayRecoveryRetryIntervalMs = 500;
constexpr UINT kTrayRecoveryRetryCount = 4;
// Polls only the current foreground tracked window.  This deliberately avoids
// a global mouse hook or sending WM_NCHITTEST into another process.
constexpr UINT_PTR kHorizontalResizeDoubleClickTimerId = 3;
constexpr UINT kHorizontalResizeDoubleClickIntervalMs = 15;

// Delay used by the per-window settle timer (id = the target HWND's own
// value, see DispatchUpdate and the WM_TIMER default case). Re-armed on
// every dispatch, so it only fires once, this long after the *last* one for
// a given window — giving DWM's caption-button-bounds refresh and the
// target's own DPI-change handling time to catch up before one final
// confirmation pass runs. Without it, a fast-moving burst of
// EVENT_OBJECT_LOCATIONCHANGE (e.g. dragging a window across monitors with
// different DPI) can leave the overlay buttons sized from whichever
// mid-transition sample happened to be the last one measured.
constexpr UINT kOverlaySettleDelayMs = 150;

// Allows a DPI-aware target app's WM_DPICHANGED handling to settle before we
// reapply the monitor-work-area-relative placement and reveal the window.
constexpr UINT kMonitorMoveReassertDelayMs = 300;

constexpr wchar_t kAppVersion[] = L"1.2";

enum class ButtonAction { SnapLeft, SnapRight, MoveNextMonitor, Center80 };
enum class ButtonVariant { Normal, RightClick, ShiftLeftClick };

struct OverlayButton {
    HWND hwnd = nullptr;
    HWND target = nullptr;  // the window this button acts on
    ButtonAction action = ButtonAction::SnapLeft;
    bool hover = false;
    bool pressed = false;
};

struct TrackedWindow {
    HWND target = nullptr;
    OverlayButton left;
    OverlayButton right;
    OverlayButton nextMonitor;
    OverlayButton center80;
    // Coalesces bursts of update triggers (e.g. EVENT_OBJECT_LOCATIONCHANGE
    // during a snap placement) onto a single in-flight measurement
    // thread per window, main-thread-only — see DispatchUpdate.
    bool dispatchInFlight = false;
    bool dispatchAgainRequested = false;
    bool pendingMonitorMoveReassert = false;
    bool pendingMonitorMoveWasHidden = false;
    RECT pendingMonitorMoveRect{};
};

enum class HorizontalResizeEdge { None, Left, Right };

struct HorizontalResizeDoubleClickState {
    bool leftButtonDown = false;
    HWND firstTarget = nullptr;
    HorizontalResizeEdge firstEdge = HorizontalResizeEdge::None;
    POINT firstPoint{};
    ULONGLONG firstClickTick = 0;
    HWND pendingTarget = nullptr;
};

HINSTANCE g_hInstance = nullptr;
HWND g_hMain = nullptr;  // hidden window owning the tray icon
UINT g_taskbarCreatedMessage = 0;
bool g_sessionNotificationRegistered = false;
UINT g_trayRecoveryAttemptsRemaining = 0;
HorizontalResizeDoubleClickState g_horizontalResizeDoubleClick;
// Preserve the previous default behavior; the tray menu can disable it for
// the current process.
bool g_hideMonitorMoveReposition = true;

// One entry per real top-level window currently being given buttons.
std::unordered_map<HWND, std::unique_ptr<TrackedWindow>> g_windows;

HWINEVENTHOOK g_hookForeground = nullptr;
HWINEVENTHOOK g_hookLocation = nullptr;
HWINEVENTHOOK g_hookMinimize = nullptr;
HWINEVENTHOOK g_hookObjectLifecycle = nullptr;
HWINEVENTHOOK g_hookReorder = nullptr;

std::wstring g_diagnosticLogPath;
// Opened once in InitializeDiagnosticLog and kept open for the process
// lifetime; AppendDiagnosticLine writes+flushes to it instead of
// re-opening/closing the file on every single log line (which used to mean
// a CreateFile/CloseHandle pair per WinEvent — see EVENT_OBJECT_REORDER,
// which fires for the whole desktop's Z-order and was the dominant cost).
FILE* g_diagnosticLogFile = nullptr;

template <typename T>
struct ComPtr {
    T* ptr = nullptr;

    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ~ComPtr() {
        if (ptr) ptr->Release();
    }

    T** put() {
        if (ptr) {
            ptr->Release();
            ptr = nullptr;
        }
        return &ptr;
    }

    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

struct ScopedBstr {
    BSTR value = nullptr;

    explicit ScopedBstr(const wchar_t* text) : value(SysAllocString(text)) {}
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    ~ScopedBstr() {
        if (value) SysFreeString(value);
    }
};

struct ScopedComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ~ScopedComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }

    bool usable() const {
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
};

struct LocationEventLogState {
    ULONGLONG lastFlushTick = 0;
    UINT count = 0;
};

std::unordered_map<HWND, LocationEventLogState> g_locationEventLogs;
// EVENT_OBJECT_REORDER's hwnd is typically the desktop container, not a
// specific window (see the WinEventProc case), so a single global throttle
// state is enough — no need to key it per-hwnd like g_locationEventLogs.
LocationEventLogState g_reorderEventLog;

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};

    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};

    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

void AppendDiagnosticLine(const char* line) {
    if (!g_diagnosticLogFile || !line) return;
    fputs(line, g_diagnosticLogFile);
    fputs("\r\n", g_diagnosticLogFile);
    fflush(g_diagnosticLogFile);
}

void LogDiagnostic(const char* format, ...) {
    if (!g_diagnosticLogFile) return;

    char message[2048] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now{};
    GetLocalTime(&now);

    char line[2300] = {};
    snprintf(line, sizeof(line),
             "%04u-%02u-%02u %02u:%02u:%02u.%03u tid=%lu %s",
             now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
             GetCurrentThreadId(), message);
    AppendDiagnosticLine(line);
}

void LogWindowDiagnostic(HWND hwnd, const char* eventName, const char* format = nullptr, ...) {
    if (!g_diagnosticLogFile) return;

    char detail[1024] = {};
    if (format) {
        va_list args;
        va_start(args, format);
        vsnprintf_s(detail, sizeof(detail), _TRUNCATE, format, args);
        va_end(args);
    }

    DWORD pid = 0;
    if (hwnd) {
        GetWindowThreadProcessId(hwnd, &pid);
    }

    LogDiagnostic("event=%s hwnd=0x%p pid=%lu %s", eventName, hwnd, pid, detail);
}

void InitializeDiagnosticLog() {
    wchar_t basePath[MAX_PATH] = {};
    DWORD baseLength = GetEnvironmentVariableW(L"LOCALAPPDATA", basePath, static_cast<DWORD>(_countof(basePath)));
    if (baseLength == 0 || baseLength >= _countof(basePath)) {
        DWORD tempLength = GetTempPathW(static_cast<DWORD>(_countof(basePath)), basePath);
        if (tempLength == 0 || tempLength >= _countof(basePath)) return;
    }

    std::wstring directory = basePath;
    if (!directory.empty() && directory.back() != L'\\') directory += L'\\';
    directory += L"WindowPosButton";
    CreateDirectoryW(directory.c_str(), nullptr);

    g_diagnosticLogPath = directory + L"\\diagnostic.log";
    std::wstring previousPath = directory + L"\\diagnostic-prev.log";
    DeleteFileW(previousPath.c_str());
    MoveFileExW(g_diagnosticLogPath.c_str(), previousPath.c_str(), MOVEFILE_REPLACE_EXISTING);

    _wfopen_s(&g_diagnosticLogFile, g_diagnosticLogPath.c_str(), L"ab");
    LogDiagnostic("app-start version=%s log=\"%s\"", WideToUtf8(kAppVersion).c_str(),
                  WideToUtf8(g_diagnosticLogPath).c_str());
}

void ShutdownDiagnosticLog() {
    if (!g_diagnosticLogFile) return;
    fclose(g_diagnosticLogFile);
    g_diagnosticLogFile = nullptr;
}

const char* WinEventName(DWORD event) {
    switch (event) {
        case EVENT_SYSTEM_FOREGROUND: return "EVENT_SYSTEM_FOREGROUND";
        case EVENT_OBJECT_SHOW: return "EVENT_OBJECT_SHOW";
        case EVENT_OBJECT_LOCATIONCHANGE: return "EVENT_OBJECT_LOCATIONCHANGE";
        case EVENT_SYSTEM_MINIMIZESTART: return "EVENT_SYSTEM_MINIMIZESTART";
        case EVENT_SYSTEM_MINIMIZEEND: return "EVENT_SYSTEM_MINIMIZEEND";
        case EVENT_OBJECT_HIDE: return "EVENT_OBJECT_HIDE";
        case EVENT_OBJECT_DESTROY: return "EVENT_OBJECT_DESTROY";
        case EVENT_OBJECT_REORDER: return "EVENT_OBJECT_REORDER";
        default: return "EVENT_UNKNOWN";
    }
}

void LogLocationEventThrottled(HWND hwnd) {
    ULONGLONG now = GetTickCount64();
    LocationEventLogState& state = g_locationEventLogs[hwnd];
    state.count++;
    if (state.lastFlushTick == 0) {
        state.lastFlushTick = now;
    }

    if (now - state.lastFlushTick >= 1000) {
        LogWindowDiagnostic(hwnd, "EVENT_OBJECT_LOCATIONCHANGE",
                            "count=%u elapsed=%llums", state.count, now - state.lastFlushTick);
        state.count = 0;
        state.lastFlushTick = now;
    }
}

// EVENT_OBJECT_REORDER fires for every Z-order change on the whole desktop
// (any process, not just windows we track), so — like
// LogLocationEventThrottled above — this collapses a burst into one summary
// line per second instead of logging (and disk-writing) every single one.
void LogReorderEventThrottled(HWND hwnd) {
    ULONGLONG now = GetTickCount64();
    g_reorderEventLog.count++;
    if (g_reorderEventLog.lastFlushTick == 0) {
        g_reorderEventLog.lastFlushTick = now;
    }

    if (now - g_reorderEventLog.lastFlushTick >= 1000) {
        LogWindowDiagnostic(hwnd, "EVENT_OBJECT_REORDER",
                            "count=%u elapsed=%llums", g_reorderEventLog.count,
                            now - g_reorderEventLog.lastFlushTick);
        g_reorderEventLog.count = 0;
        g_reorderEventLog.lastFlushTick = now;
    }
}

// Returns true if this window looks like a normal top-level app window with
// a title bar that has a Minimize button, i.e. a window we should attach our
// buttons to. Our own overlay windows are WS_POPUP with no caption, so they
// are naturally excluded here.
bool ShouldTrackWindow(HWND hwnd) {
    if (!hwnd || hwnd == g_hMain || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_CAPTION) || !(style & WS_MINIMIZEBOX)) return false;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    // Skip cloaked windows (e.g. UWP apps on another virtual desktop).
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return false;
    if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) return false;

    return true;
}

UINT GetMonitorDpi(HMONITOR hMonitor) {
    UINT dpiX = 96, dpiY = 96;
    if (hMonitor && SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return dpiX;
    }
    return 96;
}

// Computes the screen-space rect for the button occupying `slot`: a square
// immediately to the left of the native caption buttons (Minimize/
// Maximize/Close), matching their vertical span. Slot 0 sits flush against
// the caption buttons; each following slot continues further left.
bool ComputeButtonRect(HWND hwnd, int slot, bool shrink, RECT* outRect) {
    RECT captionButtons{};
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS,
                                        &captionButtons, sizeof(captionButtons));

    bool validBounds = SUCCEEDED(hr) && (captionButtons.right > captionButtons.left);

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return false;

    // Query the monitor once and reuse it below for both the DPI lookup and
    // the work-area clamp. Calling MonitorFromWindow a second time later (as
    // this used to) left a window, while dragging across a DPI boundary,
    // where Windows' "nearest monitor" verdict could flip between the two
    // calls, making the DPI and work-area reads disagree about which
    // monitor they belong to.
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    UINT monitorDpi = GetMonitorDpi(monitor);
    LONG captionLeft = 0;
    LONG top = 0;
    LONG bottom = 0;
    LONG height = 0;

    if (validBounds) {
        UINT windowDpi = GetDpiForWindow(hwnd);
        if (windowDpi == 0) windowDpi = 96;

        double scale = static_cast<double>(monitorDpi) / static_cast<double>(windowDpi);

        // Scale the caption button coordinates from target window's DPI space to monitor's physical DPI space
        captionLeft = windowRect.left + static_cast<LONG>(captionButtons.left * scale);
        top = windowRect.top + static_cast<LONG>(captionButtons.top * scale);
        bottom = windowRect.top + static_cast<LONG>(captionButtons.bottom * scale);
        height = bottom - top;
    } else {
        // Fallback: estimate caption button bounds based on window rect and monitor DPI.
        double scale = static_cast<double>(monitorDpi) / 96.0;
        
        // At 96 DPI, title bar height is typically 30px, and each button is ~45px wide.
        height = static_cast<LONG>(30.0 * scale);
        LONG buttonWidth = static_cast<LONG>(45.0 * scale);
        
        // 3 buttons: Minimize, Maximize, Close.
        LONG totalCaptionWidth = 3 * buttonWidth;
        
        // Estimate right border/padding.
        LONG border = static_cast<LONG>(8.0 * scale);
        
        top = windowRect.top;
        bottom = windowRect.top + height;
        captionLeft = windowRect.right - border - totalCaptionWidth;
    }

    if (height <= 0) return false;

    // Some maximized/custom-frame applications report a window rectangle that
    // starts above the monitor work area. Keep the overlays top-aligned, but
    // never let their top edge begin off-screen where Windows would clip them.
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        if (top < monitorInfo.rcWork.top) {
            top = monitorInfo.rcWork.top;
        }
        if (top >= monitorInfo.rcWork.bottom) return false;

        // A pathological caption measurement must not make an overlay extend
        // past the visible work area either.
        LONG availableHeight = monitorInfo.rcWork.bottom - top;
        if (height > availableHeight) {
            height = availableHeight;
        }
        bottom = top + height;
    }

    if (shrink) {
        // Shrink to 80% of original height/width
        LONG originalHeight = height;
        height = static_cast<LONG>(originalHeight * 0.8);
        
        // Keep top at the original top, and set bottom relative to it
        bottom = top + height;

        // Slot coordinates using shrunken size
        outRect->right = captionLeft - static_cast<LONG>(slot) * height;
        outRect->left = outRect->right - height;
        outRect->top = top;
        outRect->bottom = bottom;
    } else {
        outRect->right = captionLeft - static_cast<LONG>(slot) * height;
        outRect->left = outRect->right - height;
        outRect->top = top;
        outRect->bottom = bottom;
    }
    return true;
}

void SetPixelPremultiplied(BYTE* bits, int width, int x, int y, BYTE r, BYTE g, BYTE b, BYTE a) {
    BYTE* p = bits + (static_cast<size_t>(y) * width + x) * 4;
    p[0] = static_cast<BYTE>(b * a / 255);
    p[1] = static_cast<BYTE>(g * a / 255);
    p[2] = static_cast<BYTE>(r * a / 255);
    p[3] = a;
}

// Renders a button's appearance (hover / pressed state, and its
// snap-left / snap-right / move-to-next-monitor glyph) into its layered
// window using per-pixel alpha, and positions it at `screenRect`.
void RenderAndPositionOverlay(const OverlayButton& button, const RECT& screenRect) {
    int width = screenRect.right - screenRect.left;
    int height = screenRect.bottom - screenRect.top;
    if (width <= 0 || height <= 0) return;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, bitmap));

    ZeroMemory(bits, static_cast<size_t>(width) * height * 4);
    BYTE* pixels = static_cast<BYTE*>(bits);

    // Hover / pressed background wash, so the button reads like a native
    // caption button when the mouse is over it.
    if (button.pressed || button.hover) {
        BYTE alpha = button.pressed ? 90 : 55;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                SetPixelPremultiplied(pixels, width, x, y, 0, 0, 0, alpha);
            }
        }
    }

    const BYTE accentR = 0, accentG = 120, accentB = 215;   // Windows accent blue
    const BYTE outlineR = 90, outlineG = 90, outlineB = 90;

    // Shared glyph bounding box, so every button's icon occupies the exact
    // same visual footprint regardless of what it draws inside it.
    int margin = static_cast<int>(width * 0.30);
    int glyphLeft = margin;
    int glyphRight = width - margin;
    int glyphTop = static_cast<int>(height * 0.32);
    int glyphBottom = height - glyphTop;
	int offset = glyphBottom - glyphTop;
	glyphTop = 2;
	glyphBottom = glyphTop + offset;

    if (button.action == ButtonAction::SnapLeft || button.action == ButtonAction::SnapRight) {
        // "Snap left/right" glyph: a small window outline with the target half filled.
        // The border is always drawn, even over the filled half, so the button
        // stays visible against a same-colored (e.g. accent blue) title bar.
        int glyphMid = glyphLeft + (glyphRight - glyphLeft) / 2;
        bool fillLeftHalf = (button.action == ButtonAction::SnapLeft);

        for (int y = glyphTop; y < glyphBottom; ++y) {
            for (int x = glyphLeft; x < glyphRight; ++x) {
                bool isBorder = (x == glyphLeft || x == glyphRight - 1 ||
                                  y == glyphTop || y == glyphBottom - 1);
                bool inFilledHalf = fillLeftHalf ? (x < glyphMid) : (x >= glyphMid);
                if (isBorder) {
                    SetPixelPremultiplied(pixels, width, x, y, outlineR, outlineG, outlineB, 255);
                } else if (inFilledHalf) {
                    SetPixelPremultiplied(pixels, width, x, y, accentR, accentG, accentB, 235);
                } else {
                    SetPixelPremultiplied(pixels, width, x, y, 255, 255, 255, 255);
                }
            }
        }
    } else if (button.action == ButtonAction::Center80) {
        // "Centered 80%" glyph: an outer screen outline, and a filled
        // inner rectangle centered within it representing the window.
        int innerMarginX = (glyphRight - glyphLeft) / 10;
        if (innerMarginX < 1) innerMarginX = 1;
        int innerMarginY = (glyphBottom - glyphTop) / 10;
        if (innerMarginY < 1) innerMarginY = 1;

        int innerLeft = glyphLeft + innerMarginX;
        int innerRight = glyphRight - innerMarginX;
        int innerTop = glyphTop + innerMarginY;
        int innerBottom = glyphBottom - innerMarginY;

        for (int y = glyphTop; y < glyphBottom; ++y) {
            for (int x = glyphLeft; x < glyphRight; ++x) {
                bool isOuterBorder = (x == glyphLeft || x == glyphRight - 1 ||
                                      y == glyphTop || y == glyphBottom - 1);
                bool inInnerRect = (x >= innerLeft && x < innerRight &&
                                    y >= innerTop && y < innerBottom);
                if (isOuterBorder) {
                    SetPixelPremultiplied(pixels, width, x, y, outlineR, outlineG, outlineB, 255);
                } else if (inInnerRect) {
                    SetPixelPremultiplied(pixels, width, x, y, accentR, accentG, accentB, 235);
                } else {
                    SetPixelPremultiplied(pixels, width, x, y, 255, 255, 255, 255);
                }
            }
        }
    } else {
        // "Move to next monitor" glyph: two monitor rectangles inside the
        // same bounding box the snap glyphs use, the destination (right)
        // one filled. Both are always fully bordered for the same reason.
        int gap = static_cast<int>(width * 0.08);
        int monitorWidth = ((glyphRight - glyphLeft) - gap) / 2;

        int leftMonLeft = glyphLeft;
        int leftMonRight = leftMonLeft + monitorWidth;
        int rightMonLeft = glyphRight - monitorWidth;
        int rightMonRight = glyphRight;

        for (int y = glyphTop; y < glyphBottom; ++y) {
            for (int x = leftMonLeft; x < leftMonRight; ++x) {
                bool isBorder = (x == leftMonLeft || x == leftMonRight - 1 ||
                                  y == glyphTop || y == glyphBottom - 1);
                if (isBorder) {
                    SetPixelPremultiplied(pixels, width, x, y, outlineR, outlineG, outlineB, 255);
                } else {
                    SetPixelPremultiplied(pixels, width, x, y, 255, 255, 255, 255);
                }
            }
            for (int x = rightMonLeft; x < rightMonRight; ++x) {
                bool isBorder = (x == rightMonLeft || x == rightMonRight - 1 ||
                                  y == glyphTop || y == glyphBottom - 1);
                if (isBorder) {
                    SetPixelPremultiplied(pixels, width, x, y, outlineR, outlineG, outlineB, 255);
                } else {
                    SetPixelPremultiplied(pixels, width, x, y, accentR, accentG, accentB, 235);
                }
            }
        }
    }

    SIZE size{width, height};
    POINT srcPoint{0, 0};
    POINT dstPoint{screenRect.left, screenRect.top};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    UpdateLayeredWindow(button.hwnd, screenDC, &dstPoint, &size, memDC, &srcPoint, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// Right after a minimized window is restored, EVENT_SYSTEM_FOREGROUND (and
// sometimes EVENT_SYSTEM_MINIMIZEEND) can fire while IsIconic() already
// reports false but GetWindowRect/DWM caption bounds still reflect the
// window's old off-screen minimized placeholder position (the classic
// (-32000, -32000)-ish sentinel). If we trusted that rect we'd render the
// buttons off-screen — invisible — and nothing guarantees a later event
// will come along to correct it. Treat that as "not ready yet" instead.
bool WindowRectLooksLikeMinimizedPlaceholder(HWND hwnd) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return true;
    return rc.left <= -30000 && rc.top <= -30000;
}

HWND GetForegroundRootWindow() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return nullptr;
    return GetAncestor(foreground, GA_ROOT);
}

// Outcome of measuring one window, computed entirely from the target HWND
// (a plain value, safely copyable across threads) with no access to
// g_windows/TrackedWindow/OverlayButton. This is what lets ComputeUpdatePlan
// run on a disposable worker thread without any lock: there is no shared
// mutable state to guard, because none is touched.
struct ButtonPlacement {
    bool visible = false;
    RECT rect{};
};

struct UpdateResult {
    bool ready = false;
    bool retryLater = false;
    ButtonPlacement right, left, center80, nextMonitor;
};

// Runs on a one-shot measurement thread (see SpawnMeasureThread). Only calls
// APIs that query `target` or global system state (GetWindowRect,
// DwmGetWindowAttribute, GetSystemMetrics, ...) — never touches our own
// overlay HWNDs, which remain main-thread-only.
UpdateResult ComputeUpdatePlan(HWND target) {
    ULONGLONG startTick = GetTickCount64();
    LogWindowDiagnostic(target, "ComputeUpdatePlan.begin");

    UpdateResult result;

    bool stale = IsWindow(target) && !IsIconic(target) &&
                 WindowRectLooksLikeMinimizedPlaceholder(target);
    bool ready = IsWindow(target) && ShouldTrackWindow(target) &&
                 !IsIconic(target) && !stale;

    if (!ready) {
        // The window may just be mid-restore (real position not settled
        // yet) rather than genuinely minimized/gone. Ask the caller to
        // retry shortly in case no further WinEvent arrives to trigger a
        // natural update.
        result.retryLater = IsWindow(target) && !IsIconic(target);
        LogWindowDiagnostic(target, "ComputeUpdatePlan.not-ready",
                            "stale=%d elapsed=%llums", stale ? 1 : 0,
                            GetTickCount64() - startTick);
        return result;
    }

    result.ready = true;

    bool multiMonitor = GetSystemMetrics(SM_CMONITORS) > 1;

    // Keep overlay updates local-only. Sending synchronous messages to target
    // windows from this process can amplify UI stalls during COM/OLE-heavy
    // workflows such as Unity launching Visual Studio.
    bool shrinkButtons = false;

    struct Entry { ButtonPlacement* placement; ButtonAction action; };
    Entry entries[] = {
        {&result.right, ButtonAction::SnapRight},
        {&result.left, ButtonAction::SnapLeft},
        {&result.center80, ButtonAction::Center80},
        {&result.nextMonitor, ButtonAction::MoveNextMonitor},
    };

    int visibleSlotCount = 0;
    for (Entry& entry : entries) {
        if (entry.action == ButtonAction::MoveNextMonitor && !multiMonitor) {
            continue;
        }

        RECT buttonRect;
        if (!ComputeButtonRect(target, visibleSlotCount, shrinkButtons, &buttonRect)) {
            continue;
        }

        entry.placement->visible = true;
        entry.placement->rect = buttonRect;
        visibleSlotCount++;
    }

    LogWindowDiagnostic(target, "ComputeUpdatePlan.end",
                        "buttons=%d shrink=%d elapsed=%llums",
                        visibleSlotCount, shrinkButtons ? 1 : 0, GetTickCount64() - startTick);
    return result;
}

TrackedWindow& EnsureTracked(HWND target) {
    auto it = g_windows.find(target);
    if (it != g_windows.end()) return *it->second;

    auto window = std::make_unique<TrackedWindow>();
    window->target = target;
    window->left = OverlayButton{nullptr, target, ButtonAction::SnapLeft, false, false};
    window->right = OverlayButton{nullptr, target, ButtonAction::SnapRight, false, false};
    window->nextMonitor = OverlayButton{nullptr, target, ButtonAction::MoveNextMonitor, false, false};
    window->center80 = OverlayButton{nullptr, target, ButtonAction::Center80, false, false};

    for (OverlayButton* button : {&window->left, &window->right, &window->nextMonitor, &window->center80}) {
        // Keep overlays unowned. Cross-process owned popups can couple this
        // helper's UI thread to COM/OLE-heavy apps such as Explorer, Unity,
        // and Visual Studio during activation and shutdown.
        button->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                        kOverlayClassName, L"", WS_POPUP, 0, 0, 1, 1,
                                        nullptr, nullptr, g_hInstance, nullptr);
        SetWindowLongPtrW(button->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(button));
    }

    TrackedWindow& ref = *window;
    g_windows.emplace(target, std::move(window));
    LogWindowDiagnostic(target, "EnsureTracked");
    return ref;
}

void HideOverlaysForTarget(HWND target) {
    auto it = g_windows.find(target);
    if (it == g_windows.end()) return;
    ShowWindow(it->second->left.hwnd, SW_HIDE);
    ShowWindow(it->second->right.hwnd, SW_HIDE);
    ShowWindow(it->second->nextMonitor.hwnd, SW_HIDE);
    ShowWindow(it->second->center80.hwnd, SW_HIDE);
}

// Walks up the Z order from `target` (toward the front) past any of our own
// overlay windows already sitting there from a previous pass, and returns
// the first window that isn't ours — i.e. whatever currently actually
// occludes `target`. Returns nullptr if target is already the frontmost
// window in the normal (non-topmost) band.
HWND FindExternalFrontNeighbor(HWND target) {
    HWND candidate = GetWindow(target, GW_HWNDPREV);
    while (candidate) {
        wchar_t className[64] = {};
        GetClassNameW(candidate, className, static_cast<int>(_countof(className)));
        if (wcscmp(className, kOverlayClassName) != 0) return candidate;
        candidate = GetWindow(candidate, GW_HWNDPREV);
    }
    return nullptr;
}

// Re-slots `window`'s overlay HWNDs into the Z order immediately in front of
// its target, without touching their position/size/visibility. Activating a
// window (e.g. clicking its title bar), or Windows silently reasserting a
// window's normal Z position when its caption is clicked again while
// already active, jumps it to the front of the Z order ahead of any buttons
// a previous pass had placed just in front of it — leaving the buttons
// covered by the very window they decorate. Called from EVENT_SYSTEM_
// FOREGROUND, EVENT_OBJECT_REORDER, and a low-frequency safety-net timer
// (see ReassertForegroundZOrder). Safe to call synchronously/often:
// GetWindow/SetWindowPos on our own windows never block on the target's
// thread, and the guard below makes repeat calls a no-op.
void RaiseOverlaysAboveTarget(TrackedWindow& window) {
    HWND target = window.target;

    // If the four buttons are already mutually contiguous immediately in
    // front of target, there's nothing to do. This isn't just an
    // optimization: our own SetWindowPos calls below also generate
    // EVENT_OBJECT_REORDER, which routes right back into this function —
    // without this early-out, that would free-run forever instead of
    // settling after one pass.
    if (GetWindow(target, GW_HWNDPREV) == window.nextMonitor.hwnd &&
        GetWindow(window.nextMonitor.hwnd, GW_HWNDPREV) == window.center80.hwnd &&
        GetWindow(window.center80.hwnd, GW_HWNDPREV) == window.left.hwnd &&
        GetWindow(window.left.hwnd, GW_HWNDPREV) == window.right.hwnd) {
        return;
    }

    HWND insertAfter = FindExternalFrontNeighbor(target);
    if (!insertAfter) insertAfter = HWND_TOP;
    for (OverlayButton* button : {&window.right, &window.left, &window.center80, &window.nextMonitor}) {
        SetWindowPos(button->hwnd, insertAfter, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        insertAfter = button->hwnd;
    }
}

// Re-pins whichever tracked window currently has focus, if any. Shared by
// the EVENT_OBJECT_REORDER handler (the common, event-driven path) and the
// safety-net timer (see kZOrderReassertTimerId).
void ReassertForegroundZOrder() {
    HWND foreground = GetForegroundRootWindow();
    if (!foreground) return;
    auto it = g_windows.find(foreground);
    if (it != g_windows.end()) RaiseOverlaysAboveTarget(*it->second);
}

// Applies a ComputeUpdatePlan result to `window`'s overlay HWNDs. Called only
// from the main thread's WM_OVERLAY_RESULT handler, so this remains the only
// place that ever calls SetWindowPos/UpdateLayeredWindow/ShowWindow on our
// own overlay windows — no cross-thread window ownership issues.
//
// result.ready reflects state at the moment the measurement thread sampled
// it; by the time the result gets here the window may have since minimized,
// closed, or stopped qualifying (e.g. a MINIMIZESTART WinEvent arrived after
// dispatch but before the measurement finished). IsWindow/ShouldTrackWindow/
// IsIconic don't send messages to the target and return promptly regardless
// of whether its thread is responsive, so re-checking them here is cheap and
// closes that staleness window.
void ApplyUpdateResult(TrackedWindow& window, const UpdateResult& result) {
    HWND target = window.target;
    bool stillReady = result.ready && IsWindow(target) && ShouldTrackWindow(target) && !IsIconic(target);

    if (!stillReady) {
        ShowWindow(window.left.hwnd, SW_HIDE);
        ShowWindow(window.right.hwnd, SW_HIDE);
        ShowWindow(window.nextMonitor.hwnd, SW_HIDE);
        ShowWindow(window.center80.hwnd, SW_HIDE);
        if (result.retryLater && IsWindow(target) && !IsIconic(target)) {
            SetTimer(g_hMain, reinterpret_cast<UINT_PTR>(target), kOverlaySettleDelayMs, nullptr);
        }
        return;
    }

    struct Entry { OverlayButton* button; const ButtonPlacement* placement; };
    Entry entries[] = {
        {&window.right, &result.right},
        {&window.left, &result.left},
        {&window.center80, &result.center80},
        {&window.nextMonitor, &result.nextMonitor},
    };

    // Slot the buttons into the real Z order immediately in front of target
    // — right behind whatever currently occludes it — instead of forcing
    // them into the always-on-top band. That way a window that later covers
    // target naturally covers its buttons too, and one that doesn't, doesn't.
    HWND insertAfter = FindExternalFrontNeighbor(target);
    if (!insertAfter) insertAfter = HWND_TOP;

    for (Entry& entry : entries) {
        if (!entry.placement->visible) {
            ShowWindow(entry.button->hwnd, SW_HIDE);
            continue;
        }
        RenderAndPositionOverlay(*entry.button, entry.placement->rect);
        SetWindowPos(entry.button->hwnd, insertAfter, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        insertAfter = entry.button->hwnd;
    }
}

struct MeasureThreadParam {
    HWND target;
};

// Entry point for a one-shot measurement work item: does all the (possibly
// slow, cross-process) querying for one window, then hands the result to the
// main thread via a posted message and returns. Never touches g_windows,
// TrackedWindow, or any OverlayButton, so it needs no lock — there is
// nothing shared to protect.
DWORD WINAPI MeasureThreadProc(LPVOID param) {
    std::unique_ptr<MeasureThreadParam> owned(static_cast<MeasureThreadParam*>(param));
    HWND target = owned->target;

    auto* result = new UpdateResult(ComputeUpdatePlan(target));
    if (!PostMessageW(g_hMain, WM_OVERLAY_RESULT, reinterpret_cast<WPARAM>(target),
                       reinterpret_cast<LPARAM>(result))) {
        delete result;
    }
    return 0;
}

// Runs MeasureThreadProc on the process's shared OS thread pool
// (QueueUserWorkItem) instead of spinning up a brand-new thread per
// dispatch. A fresh CreateThread/ExitThread pair costs real, avoidable CPU
// for work this small (a handful of DWM/GDI queries); the pool instead
// parks and reuses a small number of worker threads, which sit idle between
// dispatches exactly like a one-shot thread would, just without the
// per-call creation/teardown cost. WT_EXECUTELONGFUNCTION tells the pool
// not to treat a slow/blocked DWM call (see below) as a reason to delay
// other queued work — it just grows the pool instead.
void SpawnMeasureThread(HWND target) {
    auto* param = new MeasureThreadParam{target};
    if (!QueueUserWorkItem(MeasureThreadProc, param, WT_EXECUTELONGFUNCTION)) {
        delete param;
        auto it = g_windows.find(target);
        if (it != g_windows.end()) it->second->dispatchInFlight = false;
        return;
    }
    // One-shot and detached: we never wait on or cancel it. It runs to
    // completion (or, in the pathological case of a truly hung DWM call,
    // sits blocked indefinitely) on its own, entirely independent of the
    // main thread and every other window's dispatch.
}

// Single entry point for "window X may need its overlays refreshed". Safe to
// call from the WinEvent hook callback directly: in the common case it's
// just a flag check, and spawning a thread never blocks the caller. Bursts
// of triggers for the same window (e.g. EVENT_OBJECT_LOCATIONCHANGE during a
// snap animation) coalesce onto whichever measurement thread is already in
// flight for that window, exactly like the old updating/updatePending guard
// did for synchronous calls.
//
// `rearmSettleTimer` re-arms the per-window settle timer (see
// kOverlaySettleDelayMs) so one confirmation pass runs shortly after the
// last dispatch. Callers reacting to a live event (window moved, shown,
// activated, display topology changed, ...) should leave this at its
// default of true. Pass false only when this call *is* that confirmation
// pass firing (the WM_TIMER default case) — otherwise it would re-arm
// itself and never stop ticking.
void DispatchUpdate(HWND target, bool rearmSettleTimer = true) {
    auto it = g_windows.find(target);
    if (it == g_windows.end()) return;
    TrackedWindow& window = *it->second;

    if (rearmSettleTimer) {
        SetTimer(g_hMain, reinterpret_cast<UINT_PTR>(target), kOverlaySettleDelayMs, nullptr);
    }

    if (window.dispatchInFlight) {
        window.dispatchAgainRequested = true;
        return;
    }
    window.dispatchInFlight = true;
    SpawnMeasureThread(target);
}

void SetWindowPosForVisibleFrame(HWND hwnd, const RECT& visibleRect);

HorizontalResizeEdge GetHorizontalResizeEdge(HWND hwnd, const POINT& cursor) {
    if (!hwnd || IsZoomed(hwnd) || IsIconic(hwnd)) return HorizontalResizeEdge::None;
    if (!(GetWindowLongW(hwnd, GWL_STYLE) & WS_THICKFRAME)) return HorizontalResizeEdge::None;

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return HorizontalResizeEdge::None;

    int borderWidth = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    if (borderWidth <= 0) return HorizontalResizeEdge::None;

    // Exclude the corner bands, whose cursor is diagonal rather than the
    // left/right resize cursor requested for this gesture.
    if (cursor.y < windowRect.top + borderWidth || cursor.y >= windowRect.bottom - borderWidth) {
        return HorizontalResizeEdge::None;
    }
    if (cursor.x >= windowRect.left && cursor.x < windowRect.left + borderWidth) {
        return HorizontalResizeEdge::Left;
    }
    if (cursor.x < windowRect.right && cursor.x >= windowRect.right - borderWidth) {
        return HorizontalResizeEdge::Right;
    }
    return HorizontalResizeEdge::None;
}

void ExpandWindowHorizontally(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || IsZoomed(hwnd) || IsIconic(hwnd)) return;

    RECT visibleFrame{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &visibleFrame, sizeof(visibleFrame))) &&
        !GetWindowRect(hwnd, &visibleFrame)) {
        LogDiagnostic("horizontal-expand frame-query-failed gle=%lu", GetLastError());
        return;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) {
        LogDiagnostic("horizontal-expand monitor-query-failed gle=%lu", GetLastError());
        return;
    }

    RECT targetFrame = {monitorInfo.rcWork.left, visibleFrame.top,
                        monitorInfo.rcWork.right, visibleFrame.bottom};
    if (targetFrame.bottom <= targetFrame.top) return;

    SetWindowPosForVisibleFrame(hwnd, targetFrame);
    LogWindowDiagnostic(hwnd, "horizontal-expand",
                        "work=(%ld,%ld,%ld,%ld) frame-top-bottom=(%ld,%ld)",
                        monitorInfo.rcWork.left, monitorInfo.rcWork.top,
                        monitorInfo.rcWork.right, monitorInfo.rcWork.bottom,
                        visibleFrame.top, visibleFrame.bottom);
    DispatchUpdate(hwnd);
}

void PollHorizontalResizeDoubleClick() {
    bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    HorizontalResizeDoubleClickState& state = g_horizontalResizeDoubleClick;

    if (!leftButtonDown && state.leftButtonDown && state.pendingTarget) {
        HWND target = state.pendingTarget;
        state.pendingTarget = nullptr;
        if (target == GetForegroundRootWindow() && IsWindow(target)) {
            ExpandWindowHorizontally(target);
        }
    }

    if (leftButtonDown && !state.leftButtonDown) {
        POINT cursor{};
        GetCursorPos(&cursor);
        HWND target = GetForegroundRootWindow();
        HorizontalResizeEdge edge = HorizontalResizeEdge::None;
        if (target && g_windows.find(target) != g_windows.end()) {
            edge = GetHorizontalResizeEdge(target, cursor);
        }

        ULONGLONG now = GetTickCount64();
        LONG dx = cursor.x - state.firstPoint.x;
        LONG dy = cursor.y - state.firstPoint.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        bool isDoubleClick = edge != HorizontalResizeEdge::None &&
                             target == state.firstTarget && edge == state.firstEdge &&
                             now - state.firstClickTick <= GetDoubleClickTime() &&
                             dx <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                             dy <= GetSystemMetrics(SM_CYDOUBLECLK);

        if (isDoubleClick) {
            state.pendingTarget = target;
            state.firstTarget = nullptr;
            state.firstEdge = HorizontalResizeEdge::None;
            state.firstClickTick = 0;
            LogWindowDiagnostic(target, "horizontal-expand double-click",
                                "edge=%s", edge == HorizontalResizeEdge::Left ? "left" : "right");
        } else if (edge != HorizontalResizeEdge::None) {
            state.firstTarget = target;
            state.firstEdge = edge;
            state.firstPoint = cursor;
            state.firstClickTick = now;
        } else {
            state.firstTarget = nullptr;
            state.firstEdge = HorizontalResizeEdge::None;
            state.firstClickTick = 0;
        }
    }

    state.leftButtonDown = leftButtonDown;
}

void RemoveTracked(HWND target) {
    auto it = g_windows.find(target);
    if (it == g_windows.end()) return;
    LogWindowDiagnostic(target, "RemoveTracked");
    DestroyWindow(it->second->left.hwnd);
    DestroyWindow(it->second->right.hwnd);
    DestroyWindow(it->second->nextMonitor.hwnd);
    DestroyWindow(it->second->center80.hwnd);
    g_windows.erase(it);
    g_locationEventLogs.erase(target);
}

// Restores hwnd from maximized (if needed) and returns its monitor's work
// area. Shared by every PerformButtonAction branch that computes a target
// rect directly with SetWindowPos rather than delegating to a synthesized
// key chord.
bool PrepareForManualResize(HWND hwnd, RECT* outWorkArea) {
    if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) return false;
    *outWorkArea = mi.rcWork;
    return true;
}

// SetWindowPos uses the non-client window bounds. On many window styles the
// visible DWM frame is inset from those bounds by an invisible resize border.
// Compensate for it so the visible frame, rather than just the logical window
// rectangle, reaches the requested work-area edges.
void SetWindowPosForVisibleFrame(HWND hwnd, const RECT& visibleRect) {
    RECT windowRect{};
    RECT frameRect{};
    if (GetWindowRect(hwnd, &windowRect) &&
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &frameRect, sizeof(frameRect)))) {
        LONG leftInset = frameRect.left - windowRect.left;
        LONG topInset = frameRect.top - windowRect.top;
        LONG rightInset = windowRect.right - frameRect.right;
        LONG bottomInset = windowRect.bottom - frameRect.bottom;

        SetWindowPos(hwnd, nullptr,
                     visibleRect.left - leftInset, visibleRect.top - topInset,
                     (visibleRect.right - visibleRect.left) + leftInset + rightInset,
                     (visibleRect.bottom - visibleRect.top) + topInset + bottomInset,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    SetWindowPos(hwnd, nullptr,
                 visibleRect.left, visibleRect.top,
                 visibleRect.right - visibleRect.left,
                 visibleRect.bottom - visibleRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// Windows maximizes a window on only one monitor. For the right-click span
// action, use the complete virtual desktop bounds instead so the visible frame
// covers every connected monitor.
void ExpandWindowAcrossAllMonitors(HWND hwnd) {
    if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) return;

    RECT targetFrame = {left, top, left + width, top + height};
    SetWindowPosForVisibleFrame(hwnd, targetFrame);
    LogWindowDiagnostic(hwnd, "all-monitor-expand",
                        "virtual=(%d,%d,%d,%d)",
                        targetFrame.left, targetFrame.top,
                        targetFrame.right, targetFrame.bottom);
}

struct MonitorWorkArea {
    HMONITOR monitor = nullptr;
    RECT work{};
};

BOOL CALLBACK CollectMonitorWorkArea(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<MonitorWorkArea>*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        monitors->push_back({monitor, info.rcWork});
    }
    return TRUE;
}

// GetWindowRect includes the invisible resize border on many DWM-managed
// windows.  Use the visible frame when translating a window between monitors
// so an edge-aligned window remains edge-aligned after the move.
RECT GetVisibleFrameRect(HWND hwnd, const RECT& fallbackRect) {
    RECT frameRect{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &frameRect, sizeof(frameRect)))) {
        return frameRect;
    }
    return fallbackRect;
}

RECT GetWindowRectForVisibleFrame(HWND hwnd, const RECT& visibleRect,
                                  const RECT& currentWindowRect) {
    RECT frameRect = GetVisibleFrameRect(hwnd, currentWindowRect);
    LONG leftInset = frameRect.left - currentWindowRect.left;
    LONG topInset = frameRect.top - currentWindowRect.top;
    LONG rightInset = currentWindowRect.right - frameRect.right;
    LONG bottomInset = currentWindowRect.bottom - frameRect.bottom;
    return {visibleRect.left - leftInset, visibleRect.top - topInset,
            visibleRect.right + rightInset, visibleRect.bottom + bottomInset};
}

void RevealMonitorMovePlacementIfPending(HWND target) {
    auto it = g_windows.find(target);
    if (it == g_windows.end() || !it->second->pendingMonitorMoveReassert) return;
    TrackedWindow& window = *it->second;
    window.pendingMonitorMoveReassert = false;

    if (!IsWindow(target)) return;
    if (!IsIconic(target) && !IsZoomed(target)) {
        SetWindowPosForVisibleFrame(target, window.pendingMonitorMoveRect);
    }
    if (window.pendingMonitorMoveWasHidden) {
        window.pendingMonitorMoveWasHidden = false;
        ShowWindow(target, SW_SHOW);
        SetForegroundWindow(target);
    }
}

bool MoveWindowToNextMonitor(HWND hwnd) {
    HMONITOR sourceMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!sourceMonitor) return false;

    std::vector<MonitorWorkArea> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitorWorkArea,
                             reinterpret_cast<LPARAM>(&monitors)) || monitors.size() < 2) {
        LogDiagnostic("move-next-monitor unavailable monitors=%zu", monitors.size());
        return false;
    }

    std::sort(monitors.begin(), monitors.end(), [](const MonitorWorkArea& a, const MonitorWorkArea& b) {
        if (a.work.left != b.work.left) return a.work.left < b.work.left;
        if (a.work.top != b.work.top) return a.work.top < b.work.top;
        if (a.work.right != b.work.right) return a.work.right < b.work.right;
        return a.work.bottom < b.work.bottom;
    });

    auto source = std::find_if(monitors.begin(), monitors.end(),
                               [sourceMonitor](const MonitorWorkArea& item) {
                                   return item.monitor == sourceMonitor;
                               });
    if (source == monitors.end()) {
        LogDiagnostic("move-next-monitor source-not-found");
        return false;
    }
    const MonitorWorkArea& destination = monitors[(std::distance(monitors.begin(), source) + 1) % monitors.size()];

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    RECT currentRect{};
    if (!GetWindowPlacement(hwnd, &placement) || !GetWindowRect(hwnd, &currentRect)) {
        LogDiagnostic("move-next-monitor window-query-failed gle=%lu", GetLastError());
        return false;
    }

    bool wasMaximized = placement.showCmd == SW_SHOWMAXIMIZED;
    RECT sourceOuterRect = wasMaximized ? placement.rcNormalPosition : currentRect;
    RECT sourceRect = GetVisibleFrameRect(hwnd, sourceOuterRect);
    LONG sourceWidth = source->work.right - source->work.left;
    LONG sourceHeight = source->work.bottom - source->work.top;
    LONG destinationWidth = destination.work.right - destination.work.left;
    LONG destinationHeight = destination.work.bottom - destination.work.top;

    // Preserve the window's placement as a fraction of its monitor's work
    // area across the move — e.g. "flush left, 50% width" on the source
    // monitor stays "flush left, 50% width" on the destination monitor,
    // regardless of the two monitors' resolution or DPI. This matches how
    // this app's own snap buttons already place windows (see
    // PerformButtonAction's SnapLeft/SnapRight/Center80, which size windows
    // as monitor-work-area-width * ratio) rather than trying to preserve an
    // absolute pixel or DPI-scaled size.
    auto clampUnit = [](double ratio) {
        if (ratio < 0.0) return 0.0;
        if (ratio > 1.0) return 1.0;
        return ratio;
    };
    double leftRatio = clampUnit(static_cast<double>(sourceRect.left - source->work.left) / sourceWidth);
    double topRatio = clampUnit(static_cast<double>(sourceRect.top - source->work.top) / sourceHeight);
    double widthRatio = clampUnit(static_cast<double>(sourceRect.right - sourceRect.left) / sourceWidth);
    double heightRatio = clampUnit(static_cast<double>(sourceRect.bottom - sourceRect.top) / sourceHeight);

    RECT destinationRect{};
    destinationRect.left = destination.work.left + static_cast<LONG>(leftRatio * destinationWidth + 0.5);
    destinationRect.top = destination.work.top + static_cast<LONG>(topRatio * destinationHeight + 0.5);
    LONG width = static_cast<LONG>(widthRatio * destinationWidth + 0.5);
    if (width > destinationWidth) width = destinationWidth;
    LONG height = static_cast<LONG>(heightRatio * destinationHeight + 0.5);
    if (height > destinationHeight) height = destinationHeight;
    destinationRect.right = destinationRect.left + width;
    destinationRect.bottom = destinationRect.top + height;

    bool hideReposition = g_hideMonitorMoveReposition &&
                          g_windows.find(hwnd) != g_windows.end();
    bool moved = false;
    if (wasMaximized) {
        placement.rcNormalPosition = GetWindowRectForVisibleFrame(hwnd, destinationRect, currentRect);
        placement.showCmd = SW_SHOWNORMAL;
        moved = SetWindowPlacement(hwnd, &placement) != FALSE;
        if (moved) ShowWindow(hwnd, SW_MAXIMIZE);
    } else {
        if (hideReposition) {
            ShowWindow(hwnd, SW_HIDE);
        }
        SetWindowPosForVisibleFrame(hwnd, destinationRect);
        moved = true;
    }

    if (!moved) {
        LogDiagnostic("move-next-monitor move-failed gle=%lu", GetLastError());
        return false;
    }

    LogDiagnostic("move-next-monitor source=(%ld,%ld,%ld,%ld) destination=(%ld,%ld,%ld,%ld) "
                  "leftRatio=%.3f topRatio=%.3f widthRatio=%.3f heightRatio=%.3f maximized=%d",
                  source->work.left, source->work.top, source->work.right, source->work.bottom,
                  destination.work.left, destination.work.top, destination.work.right, destination.work.bottom,
                  leftRatio, topRatio, widthRatio, heightRatio, wasMaximized ? 1 : 0);

    if (wasMaximized) {
        DispatchUpdate(hwnd);
    } else {
        auto it = g_windows.find(hwnd);
        if (it != g_windows.end()) {
            it->second->pendingMonitorMoveReassert = true;
            it->second->pendingMonitorMoveWasHidden = hideReposition;
            it->second->pendingMonitorMoveRect = destinationRect;
            SetTimer(g_hMain, reinterpret_cast<UINT_PTR>(hwnd), kMonitorMoveReassertDelayMs, nullptr);
        } else {
            DispatchUpdate(hwnd);
        }
    }
    return true;
}

// Snap actions use direct placement so they remain independent of Windows
// Snap and Snap Assist. The monitor button also uses direct placement because
// it must wrap from the last arranged monitor to the first.
void PerformButtonAction(HWND hwnd, ButtonAction action,
                         ButtonVariant variant = ButtonVariant::Normal) {
    if (!hwnd || !IsWindow(hwnd)) return;

    SetForegroundWindow(hwnd);

    if (action == ButtonAction::Center80) {
        RECT work;
        if (PrepareForManualResize(hwnd, &work)) {
            LONG monWidth = work.right - work.left;
            LONG monHeight = work.bottom - work.top;
            LONG targetWidth = static_cast<LONG>(monWidth * 0.8);
            LONG targetHeight = variant == ButtonVariant::RightClick
                                    ? monHeight
                                    : static_cast<LONG>(targetWidth / 1.777777);
            LONG targetX = work.left + (monWidth - targetWidth) / 2;
            LONG targetY = variant == ButtonVariant::RightClick
                               ? work.top
                               : work.top + (monHeight - targetHeight) / 2;
            SetWindowPos(hwnd, nullptr, targetX, targetY, targetWidth, targetHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return;
    }

    if (action == ButtonAction::SnapLeft || action == ButtonAction::SnapRight) {
        RECT work;
        if (PrepareForManualResize(hwnd, &work)) {
            LONG monWidth = work.right - work.left;
            LONG monHeight = work.bottom - work.top;
            double widthRatio = variant == ButtonVariant::RightClick ? 0.7
                              : variant == ButtonVariant::ShiftLeftClick ? 0.3
                                                                         : 0.5;
            LONG targetWidth = static_cast<LONG>(monWidth * widthRatio);
            LONG targetX = (action == ButtonAction::SnapLeft) ? work.left
                                                               : work.right - targetWidth;
            RECT targetFrame = {targetX, work.top, targetX + targetWidth, work.bottom};
            SetWindowPosForVisibleFrame(hwnd, targetFrame);

            // If snapping right, the app may have a minimum width that prevented
            // the window from shrinking to targetWidth. In that case the window's
            // left edge was placed at targetX but its actual width is larger, so
            // the visible right edge overflows the monitor. Read the actual visible
            // frame and shift the window left until its right edge aligns with
            // work.right.
            if (action == ButtonAction::SnapRight) {
                RECT actualWindow{};
                RECT actualFrame{};
                if (GetWindowRect(hwnd, &actualWindow) &&
                    SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                                    &actualFrame, sizeof(actualFrame)))) {
                    if (actualFrame.right > work.right) {
                        // Shift the non-client rect left by the same amount the
                        // visible right edge overflows the monitor work area.
                        LONG overflow = actualFrame.right - work.right;
                        LONG newLeft = actualWindow.left - overflow;
                        SetWindowPos(hwnd, nullptr, newLeft, actualWindow.top,
                                     actualWindow.right - actualWindow.left,
                                     actualWindow.bottom - actualWindow.top,
                                     SWP_NOZORDER | SWP_NOACTIVATE);
                        LogWindowDiagnostic(hwnd, "snap-right-overflow-corrected",
                                            "overflow=%ld newLeft=%ld", overflow, newLeft);
                    }
                }
            }
        }
        return;
    }

    if (action == ButtonAction::MoveNextMonitor) {
        if (variant == ButtonVariant::RightClick) {
            ExpandWindowAcrossAllMonitors(hwnd);
        } else {
            MoveWindowToNextMonitor(hwnd);
        }
        return;
    }

}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    if (ShouldTrackWindow(hwnd)) {
        EnsureTracked(hwnd);
        DispatchUpdate(hwnd);
    }
    return TRUE;
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                            LONG /*idChild*/, DWORD /*eventThread*/, DWORD /*eventTime*/) {
    if (idObject != OBJID_WINDOW) return;

    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        LogLocationEventThrottled(hwnd);
    } else if (event == EVENT_OBJECT_REORDER) {
        LogReorderEventThrottled(hwnd);
    } else {
        LogWindowDiagnostic(hwnd, WinEventName(event));
    }

    switch (event) {
        case EVENT_SYSTEM_FOREGROUND:
            if (ShouldTrackWindow(hwnd)) {
                TrackedWindow& window = EnsureTracked(hwnd);
                // Activation just moved `hwnd` to the very front of the Z
                // order synchronously; fix the buttons' Z-order in the same
                // tick instead of waiting on the async measurement pass —
                // see RaiseOverlaysAboveTarget.
                RaiseOverlaysAboveTarget(window);
                DispatchUpdate(hwnd);
            }
            break;

        case EVENT_OBJECT_SHOW:
            // Buttons are shown on every tracked window, not just the
            // foreground one, so this just ensures tracking exists and
            // (re)dispatches a measurement — DispatchUpdate spawns a
            // one-shot thread and returns immediately, so this hook callback
            // never blocks on the (possibly slow, cross-process) DWM/GDI work.
            if (ShouldTrackWindow(hwnd)) {
                EnsureTracked(hwnd);
                DispatchUpdate(hwnd);
            }
            break;

        case EVENT_OBJECT_LOCATIONCHANGE:
            // Only refreshes windows already tracked; DispatchUpdate itself
            // coalesces bursts (e.g. a snap placement) onto the one
            // measurement thread already in flight for this window.
            DispatchUpdate(hwnd);
            break;

        case EVENT_SYSTEM_MINIMIZESTART:
        case EVENT_OBJECT_HIDE:
            HideOverlaysForTarget(hwnd);
            break;

        case EVENT_SYSTEM_MINIMIZEEND:
            DispatchUpdate(hwnd);
            break;

        case EVENT_OBJECT_DESTROY:
            RemoveTracked(hwnd);
            break;

        case EVENT_OBJECT_REORDER:
            // Fires whenever the desktop's top-level Z order changes,
            // including cases with no other WinEvent to react to (e.g.
            // clicking the caption of a window that's already foreground —
            // see RaiseOverlaysAboveTarget). `hwnd` here is the reordered
            // container (typically the desktop), not a specific window, so
            // just re-pin whichever tracked window currently has focus.
            ReassertForegroundZOrder();
            break;

        default:
            break;
    }
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayButton* button = reinterpret_cast<OverlayButton*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!button) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!button->hover) {
                button->hover = true;
                TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                RECT rc;
                GetWindowRect(hwnd, &rc);
                RenderAndPositionOverlay(*button, rc);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            button->hover = false;
            button->pressed = false;
            RECT rc;
            GetWindowRect(hwnd, &rc);
            RenderAndPositionOverlay(*button, rc);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            button->pressed = true;
            SetCapture(hwnd);
            RECT rc;
            GetWindowRect(hwnd, &rc);
            RenderAndPositionOverlay(*button, rc);
            return 0;
        }
        case WM_LBUTTONUP: {
            bool wasPressed = button->pressed;
            button->pressed = false;
            ReleaseCapture();

            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            GetWindowRect(hwnd, &rc);
            RenderAndPositionOverlay(*button, rc);

            if (wasPressed && PtInRect(&rc, pt)) {
                ButtonVariant variant = (wParam & MK_SHIFT) != 0
                                            ? ButtonVariant::ShiftLeftClick
                                            : ButtonVariant::Normal;
                PerformButtonAction(button->target, button->action, variant);
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            button->pressed = true;
            SetCapture(hwnd);
            RECT rc;
            GetWindowRect(hwnd, &rc);
            RenderAndPositionOverlay(*button, rc);
            return 0;
        }
        case WM_RBUTTONUP: {
            bool wasPressed = button->pressed;
            button->pressed = false;
            ReleaseCapture();

            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            GetWindowRect(hwnd, &rc);
            RenderAndPositionOverlay(*button, rc);

            if (wasPressed && PtInRect(&rc, pt)) {
                PerformButtonAction(button->target, button->action, ButtonVariant::RightClick);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

std::wstring GetCurrentExecutablePath() {
    wchar_t currentPath[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, currentPath, static_cast<DWORD>(_countof(currentPath)));
    if (length == 0 || length >= _countof(currentPath)) return {};
    return currentPath;
}

void DeleteLegacyStartupValue() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyRunKeyPath, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, kStartupTaskName);
        RegCloseKey(hKey);
    }
}

bool ConnectTaskScheduler(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& rootFolder) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(service.put()));
    if (FAILED(hr)) {
        LogDiagnostic("startup-task cocreate-failed hr=0x%08lx", hr);
        return false;
    }

    VARIANT empty;
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        LogDiagnostic("startup-task connect-failed hr=0x%08lx", hr);
        return false;
    }

    ScopedBstr rootPath(L"\\");
    if (!rootPath.value) return false;

    hr = service->GetFolder(rootPath.value, rootFolder.put());
    if (FAILED(hr)) {
        LogDiagnostic("startup-task get-root-folder-failed hr=0x%08lx", hr);
        return false;
    }

    return true;
}

bool IsStartupEnabled() {
    ScopedComInit com;
    if (!com.usable()) {
        LogDiagnostic("startup-task com-init-failed hr=0x%08lx", com.hr);
        return false;
    }

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> rootFolder;
    if (!ConnectTaskScheduler(service, rootFolder)) return false;

    ScopedBstr taskName(kStartupTaskName);
    if (!taskName.value) return false;

    ComPtr<IRegisteredTask> registeredTask;
    HRESULT hr = rootFolder->GetTask(taskName.value, registeredTask.put());
    if (FAILED(hr)) return false;

    VARIANT_BOOL enabled = VARIANT_FALSE;
    hr = registeredTask->get_Enabled(&enabled);
    if (FAILED(hr) || enabled == VARIANT_FALSE) return false;

    ComPtr<ITaskDefinition> definition;
    hr = registeredTask->get_Definition(definition.put());
    if (FAILED(hr)) return false;

    ComPtr<IActionCollection> actions;
    hr = definition->get_Actions(actions.put());
    if (FAILED(hr)) return false;

    LONG count = 0;
    hr = actions->get_Count(&count);
    if (FAILED(hr)) return false;

    std::wstring currentPath = GetCurrentExecutablePath();
    if (currentPath.empty()) return false;

    for (LONG i = 1; i <= count; ++i) {
        ComPtr<IAction> action;
        hr = actions->get_Item(i, action.put());
        if (FAILED(hr)) continue;

        TASK_ACTION_TYPE type{};
        hr = action->get_Type(&type);
        if (FAILED(hr) || type != TASK_ACTION_EXEC) continue;

        ComPtr<IExecAction> execAction;
        hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execAction.put()));
        if (FAILED(hr)) continue;

        BSTR path = nullptr;
        hr = execAction->get_Path(&path);
        if (FAILED(hr) || !path) continue;

        bool matches = _wcsicmp(path, currentPath.c_str()) == 0;
        SysFreeString(path);
        if (matches) return true;
    }

    return false;
}

bool RegisterStartupTask() {
    std::wstring currentPath = GetCurrentExecutablePath();
    if (currentPath.empty()) {
        LogDiagnostic("startup-task current-path-empty");
        return false;
    }

    ScopedComInit com;
    if (!com.usable()) {
        LogDiagnostic("startup-task com-init-failed hr=0x%08lx", com.hr);
        return false;
    }

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> rootFolder;
    if (!ConnectTaskScheduler(service, rootFolder)) return false;

    ComPtr<ITaskDefinition> taskDefinition;
    HRESULT hr = service->NewTask(0, taskDefinition.put());
    if (FAILED(hr)) {
        LogDiagnostic("startup-task new-task-failed hr=0x%08lx", hr);
        return false;
    }

    ComPtr<IRegistrationInfo> registrationInfo;
    hr = taskDefinition->get_RegistrationInfo(registrationInfo.put());
    if (SUCCEEDED(hr)) {
        ScopedBstr author(L"WindowPosButton");
        ScopedBstr description(L"Starts WindowPosButton when the current user logs on.");
        if (author.value) registrationInfo->put_Author(author.value);
        if (description.value) registrationInfo->put_Description(description.value);
    }

    ComPtr<IPrincipal> principal;
    hr = taskDefinition->get_Principal(principal.put());
    if (FAILED(hr)) {
        LogDiagnostic("startup-task principal-failed hr=0x%08lx", hr);
        return false;
    }
    principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

    ComPtr<ITaskSettings> settings;
    hr = taskDefinition->get_Settings(settings.put());
    if (SUCCEEDED(hr)) {
        settings->put_Enabled(VARIANT_TRUE);
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    }

    ComPtr<ITriggerCollection> triggers;
    hr = taskDefinition->get_Triggers(triggers.put());
    if (FAILED(hr)) {
        LogDiagnostic("startup-task triggers-failed hr=0x%08lx", hr);
        return false;
    }

    ComPtr<ITrigger> trigger;
    hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.put());
    if (FAILED(hr)) {
        LogDiagnostic("startup-task create-logon-trigger-failed hr=0x%08lx", hr);
        return false;
    }

    ScopedBstr triggerId(L"LogonTrigger");
    if (triggerId.value) trigger->put_Id(triggerId.value);
    trigger->put_Enabled(VARIANT_TRUE);

    ComPtr<IActionCollection> actions;
    hr = taskDefinition->get_Actions(actions.put());
    if (FAILED(hr)) {
        LogDiagnostic("startup-task actions-failed hr=0x%08lx", hr);
        return false;
    }

    ComPtr<IAction> action;
    hr = actions->Create(TASK_ACTION_EXEC, action.put());
    if (FAILED(hr)) {
        LogDiagnostic("startup-task create-action-failed hr=0x%08lx", hr);
        return false;
    }

    ComPtr<IExecAction> execAction;
    hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execAction.put()));
    if (FAILED(hr)) {
        LogDiagnostic("startup-task exec-action-failed hr=0x%08lx", hr);
        return false;
    }

    ScopedBstr path(currentPath.c_str());
    if (!path.value) return false;

    hr = execAction->put_Path(path.value);
    if (FAILED(hr)) {
        LogDiagnostic("startup-task set-path-failed hr=0x%08lx", hr);
        return false;
    }

    VARIANT empty;
    VariantInit(&empty);
    ScopedBstr taskName(kStartupTaskName);
    if (!taskName.value) return false;

    ComPtr<IRegisteredTask> registeredTask;
    hr = rootFolder->RegisterTaskDefinition(taskName.value,
                                            taskDefinition.ptr,
                                            TASK_CREATE_OR_UPDATE,
                                            empty,
                                            empty,
                                            TASK_LOGON_INTERACTIVE_TOKEN,
                                            empty,
                                            registeredTask.put());
    if (FAILED(hr)) {
        LogDiagnostic("startup-task register-failed hr=0x%08lx", hr);
        return false;
    }

    DeleteLegacyStartupValue();
    LogDiagnostic("startup-task registered path=\"%s\"", WideToUtf8(currentPath).c_str());
    return true;
}

bool DeleteStartupTask() {
    ScopedComInit com;
    if (!com.usable()) {
        LogDiagnostic("startup-task com-init-failed hr=0x%08lx", com.hr);
        return false;
    }

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> rootFolder;
    if (!ConnectTaskScheduler(service, rootFolder)) return false;

    ScopedBstr taskName(kStartupTaskName);
    if (!taskName.value) return false;

    HRESULT hr = rootFolder->DeleteTask(taskName.value, 0);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        LogDiagnostic("startup-task delete-failed hr=0x%08lx", hr);
        return false;
    }

    DeleteLegacyStartupValue();
    LogDiagnostic("startup-task deleted");
    return true;
}

void SetStartupEnabled(bool enable) {
    bool ok = enable ? RegisterStartupTask() : DeleteStartupTask();
    if (!ok) {
        MessageBoxW(g_hMain,
                    enable ? L"Failed to register the startup task."
                           : L"Failed to delete the startup task.",
                    L"WindowPosButton", MB_OK | MB_ICONWARNING);
    }
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_ABOUT, L"About");

    UINT startupFlags = MF_STRING;
    if (IsStartupEnabled()) {
        startupFlags |= MF_CHECKED;
    } else {
        startupFlags |= MF_UNCHECKED;
    }
    AppendMenuW(menu, startupFlags, ID_TRAY_STARTUP, L"Start automatically with Windows");

    UINT hideRepositionFlags = MF_STRING |
                              (g_hideMonitorMoveReposition ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(menu, hideRepositionFlags, ID_TRAY_HIDE_REPOSITION, L"Hide Repositioning");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void ShowAboutWindow(HWND owner) {
    // Do not use a fixed formatting buffer here.  The English copy is longer
    // than the former Korean copy and can exceed 512 characters; swprintf_s
    // then invokes the invalid-parameter handler and terminates the process.
    const std::wstring text = std::wstring(L"WindowPosButton ") + kAppVersion +
        L"\n\n"
        L"Adds buttons to the left of the minimize button in every window title bar:\n\n"
        L"  • Snap left (50% width; Shift+left click: 30% width; right click: 70% width)\n"
        L"  • Snap right (50% width; Shift+left click: 30% width; right click: 70% width)\n"
        L"  • Move to next monitor (cycles through display layout order; right click: expand across all monitors)\n"
        L"  • Center at 80% (80% of the current display width at 16:9, centered in the work area; right click: full height)\n\n"
        L"This application runs with administrator privileges so the buttons are available on elevated windows.\n\n"
        L"Copyright (c) 2026 jintaeks@gmail.com";
    MessageBoxW(owner, text.c_str(), L"About WindowPosButton", MB_OK | MB_ICONINFORMATION);
}

void CreateTrayIcon();
void RestoreTrayIcon();

void ScheduleTrayIconRecovery(const char* reason) {
    LogDiagnostic("tray-icon recovery-scheduled reason=%s", reason);
    RestoreTrayIcon();

    // Explorer can still be rebuilding the notification area when a session
    // becomes active. Retry briefly so the icon registration is not lost to
    // that initialization race.
    g_trayRecoveryAttemptsRemaining = kTrayRecoveryRetryCount;
    SetTimer(g_hMain, kTrayRecoveryTimerId, kTrayRecoveryRetryIntervalMs, nullptr);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Explorer owns the notification area. When it restarts, it discards all
    // icon registrations while this process remains alive. Re-add the icon
    // after the shell broadcasts TaskbarCreated.
    if (msg == g_taskbarCreatedMessage) {
        LogDiagnostic("message=TaskbarCreated readding-tray-icon");
        ScheduleTrayIconRecovery("TaskbarCreated");
        return 0;
    }

    switch (msg) {
        case WM_TRAYICON:
            // NOTIFYICON_VERSION_4 packs the notification code into the low
            // word of lParam and the cursor coordinates into the high word.
            // Comparing the full value misses right-clicks away from (0, 0).
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_ABOUT) {
                ShowAboutWindow(hwnd);
            } else if (LOWORD(wParam) == ID_TRAY_STARTUP) {
                bool currentlyEnabled = IsStartupEnabled();
                SetStartupEnabled(!currentlyEnabled);
            } else if (LOWORD(wParam) == ID_TRAY_HIDE_REPOSITION) {
                g_hideMonitorMoveReposition = !g_hideMonitorMoveReposition;
                LogDiagnostic("monitor-move hide-reposition=%d",
                              g_hideMonitorMoveReposition ? 1 : 0);
            } else if (LOWORD(wParam) == ID_TRAY_EXIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DISPLAYCHANGE:
            // Monitor topology changed (hot-plug/resolution change) — refresh
            // every tracked window so the "move to next monitor" button
            // appears/disappears promptly on all of them.
            LogDiagnostic("message=WM_DISPLAYCHANGE tracked=%zu", g_windows.size());
            for (auto& entry : g_windows) {
                DispatchUpdate(entry.first);
            }
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_UNLOCK) {
                ScheduleTrayIconRecovery("WTS_SESSION_UNLOCK");
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                ScheduleTrayIconRecovery("PBT_APMRESUME");
            }
            return TRUE;
        case WM_OVERLAY_RESULT: {
            HWND target = reinterpret_cast<HWND>(wParam);
            std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lParam));
            auto it = g_windows.find(target);
            if (it != g_windows.end()) {
                TrackedWindow& window = *it->second;
                LogWindowDiagnostic(target, "WM_OVERLAY_RESULT", "ready=%d", result->ready ? 1 : 0);
                ApplyUpdateResult(window, *result);
                if (window.dispatchAgainRequested) {
                    window.dispatchAgainRequested = false;
                    SpawnMeasureThread(target);  // dispatchInFlight stays true
                } else {
                    window.dispatchInFlight = false;
                }
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == kHorizontalResizeDoubleClickTimerId) {
                PollHorizontalResizeDoubleClick();
                return 0;
            }
            if (wParam == kTrayRecoveryTimerId) {
                RestoreTrayIcon();
                if (g_trayRecoveryAttemptsRemaining > 0 && --g_trayRecoveryAttemptsRemaining == 0) {
                    KillTimer(hwnd, kTrayRecoveryTimerId);
                    LogDiagnostic("tray-icon recovery-complete");
                }
                return 0;
            }
            if (wParam == kZOrderReassertTimerId) {
                // Safety net only — see the comment on kZOrderReassertTimerId
                // and the EVENT_OBJECT_REORDER case, which normally handles
                // this event-driven at effectively no idle cost.
                ReassertForegroundZOrder();
                return 0;
            }
            // Fires as one of three things that share this one
            // per-window timer slot (id = the target HWND itself as
            // wParam): the retry from ApplyUpdateResult (a just-restored
            // window's real position wasn't settled yet when we last
            // checked it), the settle-confirmation timer armed by
            // DispatchUpdate (see kOverlaySettleDelayMs), or the delayed
            // monitor-move reassertion. The tray setting controls only
            // whether that reassertion is visibly hidden. Pass
            // rearmSettleTimer=false to
            // DispatchUpdate: this call *is* the confirmation pass, so it
            // must not re-arm itself.
            KillTimer(hwnd, wParam);
            LogWindowDiagnostic(reinterpret_cast<HWND>(wParam), "WM_TIMER.retry");
            RevealMonitorMovePlacementIfPending(reinterpret_cast<HWND>(wParam));
            DispatchUpdate(reinterpret_cast<HWND>(wParam), /*rearmSettleTimer=*/false);
            return 0;
        }
        case WM_DESTROY: {
            LogDiagnostic("message=WM_DESTROY tracked=%zu", g_windows.size());
            KillTimer(hwnd, kHorizontalResizeDoubleClickTimerId);
            KillTimer(hwnd, kTrayRecoveryTimerId);
            if (g_sessionNotificationRegistered) {
                WTSUnRegisterSessionNotification(hwnd);
                g_sessionNotificationRegistered = false;
            }
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = ID_TRAY_ICON;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void RegisterClasses() {
    WNDCLASSEXW overlayClass{};
    overlayClass.cbSize = sizeof(overlayClass);
    overlayClass.lpfnWndProc = OverlayWndProc;
    overlayClass.hInstance = g_hInstance;
    overlayClass.lpszClassName = kOverlayClassName;
    overlayClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&overlayClass);

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainWndProc;
    mainClass.hInstance = g_hInstance;
    mainClass.lpszClassName = kMainClassName;
    mainClass.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    mainClass.hIconSm = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassExW(&mainClass);
}

void CreateTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hMain;
    nid.uID = ID_TRAY_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(nid.szTip, L"WindowPosButton");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        DWORD error = GetLastError();
        LogDiagnostic("tray-icon add-failed gle=%lu", error);
        return;
    }

    // Request current notification-area callback behavior, including the
    // modern WM_CONTEXTMENU notification used by the tray menu handler.
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid)) {
        DWORD error = GetLastError();
        LogDiagnostic("tray-icon set-version-failed gle=%lu", error);
    } else {
        LogDiagnostic("tray-icon added");
    }
}

void RestoreTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hMain;
    nid.uID = ID_TRAY_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(nid.szTip, L"WindowPosButton");

    // If Explorer retained the registration this is an in-place refresh. If
    // it did not (the usual post-resume/reset case), add it back from scratch.
    if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        LogDiagnostic("tray-icon restored-by-modify");
        return;
    }

    DWORD modifyError = GetLastError();
    LogDiagnostic("tray-icon modify-failed gle=%lu; adding", modifyError);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        LogDiagnostic("tray-icon recovery-add-failed gle=%lu", GetLastError());
        return;
    }

    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid)) {
        LogDiagnostic("tray-icon recovery-set-version-failed gle=%lu", GetLastError());
    } else {
        LogDiagnostic("tray-icon restored-by-add");
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"WindowPosButton is already running.", L"WindowPosButton", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    InitializeDiagnosticLog();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_hInstance = hInstance;
    RegisterClasses();

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreatedMessage == 0) {
        LogDiagnostic("taskbar-created-register-failed gle=%lu", GetLastError());
    }

    // A plain (never-shown) top-level window rather than a message-only one:
    // message-only windows don't receive broadcast messages like
    // WM_DISPLAYCHANGE, which we need for multi-monitor hot-plug handling.
    g_hMain = CreateWindowExW(0, kMainClassName, L"WindowPosButton", 0, 0, 0, 0, 0,
                               nullptr, nullptr, hInstance, nullptr);
    g_sessionNotificationRegistered = WTSRegisterSessionNotification(g_hMain, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (!g_sessionNotificationRegistered) {
        LogDiagnostic("wts-session-notification-register-failed gle=%lu", GetLastError());
    } else {
        LogDiagnostic("wts-session-notification-registered");
    }
    // A logon-triggered scheduled task can start before Explorer has finished
    // creating its notification area.  The first NIM_ADD may therefore fail
    // without a later TaskbarCreated/session notification to recover it.
    // Use the same short retry path used for Explorer/session recovery.
    ScheduleTrayIconRecovery("startup");
    SetTimer(g_hMain, kZOrderReassertTimerId, kZOrderReassertIntervalMs, nullptr);
    g_horizontalResizeDoubleClick.leftButtonDown =
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    SetTimer(g_hMain, kHorizontalResizeDoubleClickTimerId,
             kHorizontalResizeDoubleClickIntervalMs, nullptr);

    g_hookForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                       nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookLocation = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                     nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookMinimize = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
                                     nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookObjectLifecycle = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
                                            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookReorder = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER,
                                    nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    LogDiagnostic("hooks foreground=0x%p location=0x%p minimize=0x%p lifecycle=0x%p reorder=0x%p gle=%lu",
                  g_hookForeground, g_hookLocation, g_hookMinimize, g_hookObjectLifecycle, g_hookReorder,
                  GetLastError());

    // Attach to every qualifying window that already exists.
    EnumWindows(EnumWindowsProc, 0);
    LogDiagnostic("initial-enum-complete tracked=%zu", g_windows.size());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LogDiagnostic("message-loop-exit code=%llu tracked=%zu",
                  static_cast<unsigned long long>(msg.wParam), g_windows.size());

    if (g_hookForeground) UnhookWinEvent(g_hookForeground);
    if (g_hookLocation) UnhookWinEvent(g_hookLocation);
    if (g_hookMinimize) UnhookWinEvent(g_hookMinimize);
    if (g_hookObjectLifecycle) UnhookWinEvent(g_hookObjectLifecycle);
    if (g_hookReorder) UnhookWinEvent(g_hookReorder);

    ReleaseMutex(mutex);
    CloseHandle(mutex);
    ShutdownDiagnosticLog();
    return static_cast<int>(msg.wParam);
}
