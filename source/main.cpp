#include "main.inl"
#include "ogl_overlay.h"
#include "util.h"
#include <VersionHelpers.h>
#include <process.h>
#include <timeapi.h>
#include <prsht.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdint>
#include <charconv>
#include <cwchar>
#include <string>

// Link against essential Windows libraries
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")


// Global state variables
HINSTANCE g_hInstRelay = NULL;
HWND g_hMainWnd = NULL;
bool g_vistaOrGreater;
std::wstring g_errMsg;
std::wstring g_errWndName;
HMODULE g_hDwmapi = NULL;
DwmSetWindowAttribute_t DwmSetWindowAttribute = nullptr; // Pointer to function from library
namespace {
    NOTIFYICONDATAW nid{};
    HANDLE g_hSingleTonMutex = NULL;
    HANDLE g_hMapFileGlobal = NULL;
    HWND g_hTooltipWnd = NULL;
    HICON g_hGearIcon = NULL;
    HFONT g_uiFont = NULL;
    HFONT g_hPshFont = NULL;
    HFONT g_hPshTabsFont = NULL;
    BYTE g_fontQuality;
    std::wstring g_trayName;
    int g_uiScale = 96;
    static constexpr wchar_t Tray_owner_Class[] = L"Tray_owner_Class";
    static constexpr wchar_t CLASS_STATIC[] = L"STATIC";
    static constexpr wchar_t CLASS_BUTTON[] = L"BUTTON";
    static constexpr wchar_t CLASS_EDIT[] = L"EDIT";
    static constexpr wchar_t CLASS_COMBOBOX[] = L"COMBOBOX";
}


// Utility functions for scaling lengths based on a UI scale setting
__forceinline static int Scale(const int val) noexcept { return (val * g_uiScale + 48) / 96; }
__forceinline static int ScaleTo(const int scalar, const int val) noexcept { return (val * scalar + 48) / 96; }


// Function for handling cases where another instance of the same executable is running already
static bool isAnotherInstanceRunning(const LPCWSTR memRegionName, const bool writeSharedMem = false) {
    g_errMsg.clear(); // Empty g_errMsg
    if (writeSharedMem) {
        SetLastError(ERROR_SUCCESS); // Reset last error
        // Create a shared mem region
        g_hMapFileGlobal = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(HWND), memRegionName);
        if (!g_hMapFileGlobal) { // If handle is invalid
            char errBuf[ERRBUF_SIZE];
            const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
            g_errMsg.append(L"could not create shared mem region, failed with error code: ").append(errBuf, r.ptr);
            goto throwError_wMem;
        }
        SetLastError(ERROR_SUCCESS); // Reset last error
        // Map view of shared mem region in write mode
        const HANDLE pView = MapViewOfFile(g_hMapFileGlobal, FILE_MAP_WRITE, 0, 0, sizeof(HWND));
        if (!pView) { // If mapping is invalid
            char errBuf[ERRBUF_SIZE];
            const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
            g_errMsg.append(L"could not map view of shared mem region, failed with error code: ").append(errBuf, r.ptr);
            goto throwError_wMem;
        }
        *static_cast<HWND*>(pView) = g_hMainWnd; // Write window identifier to shared mem region
        UnmapViewOfFile(pView);
        fwprintf(stdout, L"[*] Successfully created shared mem region with window identifier\n");
        return true;
        throwError_wMem: {
                fwprintf(stderr, L"[!] Warning: %ls\nIf another instance is launched, "
                    L"it won't be able to wake this one up before killing itself\n", g_errMsg.c_str());
                MessageBoxW(g_hMainWnd, g_errMsg.c_str(), g_errWndName.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
                return false;
        }
    }
    struct { // Prepare name for the mutex
        __forceinline std::wstring operator()(void) const {
            std::wstring mut;
            mut.reserve(26 + g_execName.size());
            mut.append(L"Local\\").append(g_execName).append(L"_SingleInstanceMutex");
            return mut;
        }
    } getMutexName;
    SetLastError(ERROR_SUCCESS); // Reset last error
    // Create the mutex (unique system-wide) as g_hSingleTonMutex
    g_hSingleTonMutex = CreateMutexW(NULL, TRUE, getMutexName().c_str());
    const DWORD hMutexErr = GetLastError();
    if (!g_hSingleTonMutex) { // If mutex handle is invalid
        fwprintf(stderr, L"[!] Warning: could not create app's unique mutex, failed with error code %lu\n", hMutexErr);
    } else if (hMutexErr == ERROR_ALREADY_EXISTS) {
        fwprintf(stdout, L"[*] Could not create app's unique mutex because it already exists\n"
            L"This means an instance of this executable is already running\n");
    } else {
        fwprintf(stdout, L"[*] Successfully created app's unique mutex\n");
    }
    bool validExtInst = false;
    SetLastError(ERROR_SUCCESS); // Reset last error
    // Attempt at opening handle to external shared mem region as hMapFileExt
    const HANDLE hMapFileExt = OpenFileMappingW(FILE_MAP_READ, FALSE, memRegionName);
    if (!hMapFileExt) {
        if (hMutexErr == ERROR_ALREADY_EXISTS) {
            char errBuf[ERRBUF_SIZE];
            const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
            g_errMsg.append(L"could not open a handle to the existing instance's shared mem region, failed with error code ").append(errBuf, r.ptr);
            errBox(g_errMsg.c_str());
            return false;
        }
    } else {
        SetLastError(ERROR_SUCCESS); // Reset last error
        // Map view of ext shared mem region from handle in read mode
        const HANDLE pViewExt = MapViewOfFile(hMapFileExt, FILE_MAP_READ, 0, 0, sizeof(HWND));
        if (!pViewExt) { // If mapping is invalid
            char errBuf[ERRBUF_SIZE];
            const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
            g_errMsg.append(L"could not map view of the existing instance's shared mem region, failed with error code ").append(errBuf, r.ptr);
        } else {
            // Read window identifier from mapped mem region
            const HWND hExtWnd = *static_cast<HWND*>(pViewExt);
            // If handle for window id is valid and leads to an existing window
            if (hExtWnd && IsWindow(hExtWnd)) {
                // Message said window telling it to show itself
                PostMessageW(hExtWnd, WM_COMMAND, ID_TRAY_SHOW, 0);
                fwprintf(stdout, L"[*] Message sent to existing instance\n");
                validExtInst = true;
            } else {
                fwprintf(stderr, L"[!] Warning: could not find an id for a valid window in the existing instance's shared mem region\n"
                    L"This could indicate that a previous instance has crashed\n");
            }
            UnmapViewOfFile(pViewExt);
        }
    }
    CloseHandle(hMapFileExt);
    return validExtInst;
}


// Reload text font for all UI elements of a window
__declspec(noinline) static void reloadTextFont(const HWND hWnd = g_hMainWnd, const HFONT fontVar = g_uiFont) {
    if (fontVar && hWnd) { // Enumerate child windows of the main window and set their font to g_uiFont
        EnumChildWindows(hWnd, [](HWND hChild, LPARAM lp) -> BOOL {
            SendMessageW(hChild, WM_SETFONT, lp, TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(fontVar));
    }
}


// Utility function to create a new instance of the UI font whenever needed
static void createGuiFont(HFONT& fontVar, const int fontSize) {
    if (fontVar) DeleteObject(fontVar);
    { // Attempt retrieving default system font
        const UINT raw_ncm = sizeof(NONCLIENTMETRICSW);
        const UINT ncmSize = g_vistaOrGreater ? raw_ncm : raw_ncm - sizeof(int);
        NONCLIENTMETRICSW ncm = { ncmSize };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncmSize, &ncm, 0)) {
            ncm.lfMessageFont.lfHeight = -fontSize;
            ncm.lfMessageFont.lfQuality = g_fontQuality;
            fontVar = CreateFontIndirectW(&ncm.lfMessageFont);
            return;
        }
    }
    // Fallback mechanism: creating a Segoe UI font
    const HFONT hNewFont = CreateFontW(
        -fontSize, 0, 0, 0, 
        FW_NORMAL, 
        FALSE, FALSE, FALSE, 
        DEFAULT_CHARSET, 
        OUT_DEFAULT_PRECIS, 
        CLIP_DEFAULT_PRECIS, 
        g_fontQuality, 
        DEFAULT_PITCH | FF_DONTCARE, 
        L"Segoe UI"
    );
    // Absolute worst case scenario, use default ms font
    fontVar = hNewFont ? hNewFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}


// Utility function for updating tooltips for preset buttons and settings fields
static void setTooltip(const HWND hCtrl, const LPCWSTR text) {
    if (!g_hTooltipWnd || !hCtrl) return;
    // Build trigger-specific tooltip class
    TOOLINFOW ti{};
    ti.cbSize = TTTOOLINFOW_V1_SIZE; // Set binary size
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS; // Assign flags (use separate window instead of pixel coordinates, and auto manage subclass)
    ti.hwnd = GetParent(hCtrl); // Assign parent window
    ti.uId = reinterpret_cast<UINT_PTR>(hCtrl); // Assign unique tooltip window id (app-wide only)
    ti.lpszText = const_cast<LPWSTR>(text); // Set tooltip text
    if (!SendMessageW(g_hTooltipWnd, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti))) { // Create tooltip (if it doesn't yet exist)
        SendMessageW(g_hTooltipWnd, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti)); // If tooltip already exists, update it
    }
}


// Utility function for updating text in preset buttons
__declspec(noinline) static void presetBtnsUpdate(void) {
    constexpr uint32_t maxSizeForBtn = 24;
    // Loop with variable i as startInd by default, restarting as long as i isn't higher than endInd, with i getting incremented each turn
    for (int i = PRST1_TRIG_IDX, ind = 0; ind < PRESET_COUNT; ++i, ++ind) {
        const trig::info& trig = TrigsReg[i];
        if (!trig.hTrigPtr) continue; // Verify pointer validity
        const std::wstring_view nameView{ *static_cast<const std::wstring*>(g_cfg.get(SEC_PRESET1 + trig.id - 300, KEY_PRESET_NAME)) }; // Obtain set name from runtime settings
        std::wstring& prstName = prst::g_presetNames[ind];
        prstName.clear();
        if (nameView.empty()) { // If no custom name was set, reset the text
            prstName.append(trig.text);
        } else { // Else (if a name was set)
            // Craft button text accordingly
            prstName.reserve(11 + nameView.size());
            prstName.append(trig.text);
            prstName.append(L" - ");
            prstName.append(nameView.substr(0, maxSizeForBtn));
        }
        SetWindowTextW(*(trig.hTrigPtr), prstName.c_str()); // Apply text to button
        if (!nameView.empty() && nameView.size() > maxSizeForBtn) prstName.append(nameView.substr(maxSizeForBtn)); // If using a custom name and it was shortened, add the rest
        setTooltip(*(trig.hTrigPtr), prstName.c_str()); // Apply text to tooltip
    }
}


// Function running in separate thread, disabling a button button for 3 second after it's been clicked
unsigned __stdcall ButtonRestrictorThread(void* pArg) {
    const HWND hBtn = static_cast<HWND>(pArg);
    if (!hBtn || !IsWindow(hBtn)) return 1;
    EnableWindow(hBtn, FALSE); // Block button
    Sleep(3000);
    if (hBtn && IsWindow(hBtn)) {
        flushBtnClicks(hBtn);
        EnableWindow(hBtn, TRUE); // Unblock
    }
    return 0;
}


// Hex-only mode field procedures - handles interactions
LRESULT CALLBACK HexSubclassProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    const WNDPROC pfnOldproc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CHAR: { // Entering a new char
            wchar_t c = static_cast<wchar_t>(wp);
            if (c < 0x20) break;
            if (!iswxdigit(c)) return 0; // If it's not a hex char, block the input
            break;
        }
        case WM_PASTE: { // Pasting text
            if (OpenClipboard(hWnd)) {
                const HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* pText = static_cast<wchar_t*>(GlobalLock(hData));
                    if (pText) {
                        // Look at all chars and if any isn't a hex char, block the paste operation
                        for (wchar_t* p = pText; *p; ++p) {
                            if (*p >= 0x20 && !iswxdigit(*p)) {
                                GlobalUnlock(hData);
                                CloseClipboard();
                                return 0;
                            }
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            break;
        }
    }
    return CallWindowProcW(pfnOldproc, hWnd, msg, wp, lp);
}


// Dynamic color selector manager function
static bool DynamicColorPicker(const HWND hParentPage, ogl::PickColorContext *const pCtx) {
    struct ColorSelector { // Worker functions
        // Function converting RGB to HS (hue + saturation)
        static void RGBtoHS(const DWORD argb, float *const outH, float *const outS) {
            // Compute input into a float array
            float bgr_f[3];
            for (int i = 0; i < 3; ++i) { bgr_f[i] = static_cast<float>(reinterpret_cast<const BYTE*>(&argb)[i]) / 255.0f; }
            // Get min & max values
            float maxVal = bgr_f[0], minVal = maxVal;
            for (int i = 0 + 1; i < 3; ++i) {
                if (bgr_f[i] > maxVal) { maxVal = bgr_f[i]; continue; }
                if (bgr_f[i] < minVal) { minVal = bgr_f[i]; continue; }
            }
            const float delta = maxVal - minVal;

            *outS = (maxVal > 0.0f) ? (delta / maxVal) : 0.0f;
            if (delta < 0.00001f) {
                *outH = 0.0f;
            } else {
                if (maxVal == bgr_f[2]) {
                    *outH = 60.0f * fmodf(((bgr_f[1] - bgr_f[0]) / delta), 6.0f);
                } else if (maxVal == bgr_f[1]) {
                    *outH = 60.0f * (((bgr_f[0] - bgr_f[2]) / delta) + 2.0f);
                } else {
                    *outH = 60.0f * (((bgr_f[2] - bgr_f[1]) / delta) + 4.0f);
                }
                if (*outH < 0.0f) *outH += 360.0f;
            }
        }
        // Function converting HSV to ARGB
        static DWORD HSVtoARGB(float h, float s, float v) {
            float r, g, b;
            if (s <= 0.0f) {
                r = g = b = v;
            } else {
                h = fmodf(h, 360.0f) / 60.0f;
                const int i = static_cast<int>(h);
                const float f = h - i;
                const float p = v * (1.0f - s);
                const float q = v * (1.0f - s * f);
                const float t = v * (1.0f - s * (1.0f - f));
                switch (i) {
                    case 0: r = v, g = t, b = p; break;
                    case 1: r = q, g = v, b = p; break;
                    case 2: r = p, g = v, b = t; break;
                    case 3: r = p, g = q, b = v; break;
                    case 4: r = t, g = p, b = v; break;
                    default: r = v; g = p; b = q;
                }
            }
            // Build ARGB array
            DWORD result = 0;
            constexpr float multiplier = 255.0f;
            reinterpret_cast<BYTE*>(&result)[0] = static_cast<BYTE>(b * multiplier);
            reinterpret_cast<BYTE*>(&result)[1] = static_cast<BYTE>(g * multiplier);
            reinterpret_cast<BYTE*>(&result)[2] = static_cast<BYTE>(r * multiplier);
            return result;
        }
        // Function converting a point on the color square to an ARGB value
        static DWORD ColorSquareToARGB(int x, int y, int width, int height, float value = 1.0f) {
            width -= 1, height -= 1;
            float hue = (static_cast<float>(x) / static_cast<float>(width)) * 360.0f;
            float sat = 1.0f - (static_cast<float>(y) / static_cast<float>(height));
            if (x == width) hue = 359.9f; // If maxed out left, reduce hue a tiny bit
            return HSVtoARGB(hue, sat, value);
        }
        // Function for creating the color square bitmap
        static HBITMAP CreateColorSquareBitmap(const HDC hdc, int width, int height, const float value = 1.0f) {
            // Preparing the bitmap grid
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = width;
            bmi.bmiHeader.biHeight = -height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            COLORREF* pPixels = nullptr;
            const HBITMAP hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pPixels), NULL, 0);
            if (!hBitmap || !pPixels) return NULL;
            // Define height and width indexes as float
            const float floatHeightIdx = static_cast<float>(height - 1);
            const float floatWidthIdx = static_cast<float>(width - 1);
            // Parse columns
            for (int y = 0; y < height; ++y) {
                const float sat = 1.0f - (static_cast<float>(y) / floatHeightIdx); // Calculate saturation
                // Parse rows
                for (int x = 0; x < width; ++x) {
                    const float hue = (static_cast<float>(x) / floatWidthIdx) * 360.0f; // Calculate hue
                    pPixels[y * width + x] = HSVtoARGB(hue, sat, value); // Set pixel
                }
            }
            return hBitmap;
        }
        // Color square procedures - handles interactions
        static LRESULT CALLBACK SquareSubclassProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
            ogl::PickColorContext *const pCtx = reinterpret_cast<ogl::PickColorContext*>(dwRefData);
            if (!pCtx) { goto default_handler; }
            switch (msg) {
                case WM_PAINT: { // Initial draw
                    PAINTSTRUCT ps;
                    const HDC hdc = BeginPaint(hWnd, &ps);
                    RECT rc; GetClientRect(hWnd, &rc); // Obtain square's internal dimensions
                    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
                    if (!pCtx->hSquareBmp) pCtx->hSquareBmp = CreateColorSquareBitmap(hdc, w, h); // Create square bitmap if it doesn't already exist
                    if (pCtx->hSquareBmp) {
                        // Draw the color square
                        const HDC hMemDC = CreateCompatibleDC(hdc);
                        const HBITMAP hOldBmp = static_cast<HBITMAP>(SelectObject(hMemDC, pCtx->hSquareBmp));
                        BitBlt(hdc, 0, 0, w, h, hMemDC, 0, 0, SRCCOPY);
                        SelectObject(hMemDC, hOldBmp);
                        DeleteDC(hMemDC);
                        int cx, cy;
                        // Obtain coordinates for cursor (only calculate from color if the user isn't dragging the mouse)
                        if (pCtx->isDragging) {
                            cx = pCtx->ptX, cy = pCtx->ptY;
                        } else {
                            float hue, sat;
                            RGBtoHS(pCtx->currentARGB, &hue, &sat);
                            cx = static_cast<int>((hue / 360.0f) * (w - 1));
                            cy = static_cast<int>((1.0f - sat) * (h - 1));
                        }
                        const int r_cursor = ScaleTo(pCtx->ui_scale, 4); // Cursor radius
                        { // Clamp cursor position so it stays in the square
                            const int wIdx_rcursor = w - 1 - r_cursor;
                            const int hIdx_rcursor = h - 1 - r_cursor;
                            if (cx < r_cursor) cx = r_cursor;
                            else if (cx > wIdx_rcursor) cx = wIdx_rcursor;
                            if (cy < r_cursor) cy = r_cursor;
                            else if (cy > hIdx_rcursor) cy = hIdx_rcursor;
                        }
                        // Draw the cursor
                        const HPEN hBlackPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                        const HPEN hWhitePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
                        const HBRUSH hNullBrush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
                        const HPEN hOldPen = static_cast<HPEN>(SelectObject(hdc, hBlackPen));
                        const HBRUSH hOldBrush = static_cast<HBRUSH>(SelectObject(hdc, hNullBrush));
                        Ellipse(hdc, cx - r_cursor, cy - r_cursor, cx + r_cursor + 1, cy + r_cursor + 1);
                        SelectObject(hdc, hWhitePen);
                        Ellipse(hdc, cx - (r_cursor - 1), cy - (r_cursor - 1), cx + r_cursor, cy + r_cursor);
                        SelectObject(hdc, hOldPen);
                        SelectObject(hdc, hOldBrush);
                        DeleteObject(hBlackPen);
                        DeleteObject(hWhitePen);
                        EndPaint(hWnd, &ps);
                    }
                    return 0;
                }
                case WM_LBUTTONDOWN: { // Pressed left click
                    SetCapture(hWnd); // Start capturing the mouse
                    goto left_click;
                }
                case WM_MOUSEMOVE: { // Moving mouse
                    if (wp & MK_LBUTTON) left_click: { // While holding left click
                        RECT rc; GetClientRect(hWnd, &rc); // Obtain square's internal dimensions
                        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
                        // Obtain coords and clamp them to the square
                        int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
                        if (x < 0) x = 0; else if (x >= w) x = w - 1;
                        if (y < 0) y = 0; else if (y >= h) y = h - 1;
                        pCtx->ptX = x, pCtx->ptY = y;
                        pCtx->isDragging = true; // Set dragging mouse flag
                        DWORD newARGB = ColorSquareToARGB(x, y, w, h); // Obtain RGB value from cursor coords
                        reinterpret_cast<BYTE*>(&newARGB)[3] = reinterpret_cast<BYTE*>(&pCtx->currentARGB)[3]; // Keep original opacity
                        SendMessageW(GetParent(hWnd), IDC_COLOR_SQUARE, 0, newARGB); // Message the window
                        return 0;
                    }
                    break;
                }
                case WM_LBUTTONUP: { // Left click released
                    pCtx->isDragging = false; // Disable dragging mouse flag
                    if (GetCapture() == hWnd) ReleaseCapture(); // Stop capturing the mouse
                    return 0;
                }
                case WM_NCDESTROY: { // Destruction routine
                    if (pCtx->hSquareBmp) { DeleteObject(pCtx->hSquareBmp); pCtx->hSquareBmp = NULL; } // Destroy square bitmap if it exists
                    RemoveWindowSubclass(hWnd, SquareSubclassProc, uIdSubclass);
                    break;
                }
            }
            default_handler:
            return DefSubclassProc(hWnd, msg, wp, lp);
        }
        // Dynamic color selector page procedures - handles interactions
        static LRESULT CALLBACK Proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
            ogl::PickColorContext* pCtx = reinterpret_cast<ogl::PickColorContext*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            static constexpr uint32_t edits_countIdx = std::size(cs_edits) - 1; // Fields count index
            static bool hex_mode = false; // Hex mode
            struct { // Operator for toggling a field's mode (hex or base 10, updating text has to be done separately)
                __declspec(noinline) void operator()(const HWND hWnd) const {
                    const WNDPROC currProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hWnd, GWLP_WNDPROC)); // Get current callback
                    LONG_PTR styles = GetWindowLongPtrW(hWnd, GWL_STYLE); // Get styles
                    uint32_t charsLimit;
                    if (hex_mode) {
                        styles &= ~ES_NUMBER; // Remove number only style
                        styles |= ES_UPPERCASE; // Add force uppercase style
                        if (currProc != HexSubclassProc) {
                            const LONG_PTR origProc = SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HexSubclassProc)); // Set HexSubclassProc
                            SetWindowLongPtrW(hWnd, GWLP_USERDATA, origProc); // Keep pointer to original callback
                        }
                        charsLimit = 2;
                    } else {
                        styles |= ES_NUMBER; // Add number only style
                        styles &= ~ES_UPPERCASE; // remove force uppercase style
                        if (currProc == HexSubclassProc) {
                            const LONG_PTR origProc = GetWindowLongPtrW(hWnd, GWLP_USERDATA); // Get pointer to original callback
                            if (origProc) {
                                SetWindowLongPtrW(hWnd, GWLP_WNDPROC, origProc); // Set original callback
                                SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
                            }
                        }
                        charsLimit = 3;
                    }
                    SetWindowLongPtrW(hWnd, GWL_STYLE, styles); // Set new styles
                    SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED); // Apply
                    SendMessageW(hWnd, EM_LIMITTEXT, charsLimit, 0); // Set chars limit
                }
            } toggleHex;
            struct { // Operator for reading a number from a field (base 10 or hex)
                __forceinline unsigned char operator()(const HWND hWnd, const int fieldId) const {
                    wchar_t numBuf[16];
                    GetDlgItemTextW(hWnd, fieldId, numBuf, 16); // Read the text
                    const unsigned long val = wcstoul(numBuf, nullptr, hex_mode ? 16 : 10); // Convert with either base 16 or 10 depending on hex mode state
                    return (val > MAX_BYTE) ? MAX_BYTE : static_cast<unsigned char>(val); // Return as max 255
                }
            } readField;
            struct { // Operator for synchronizing UI triggers with an evolving ARGB value
                __forceinline void operator()(const HWND hWnd, ogl::PickColorContext *const localCtx, const DWORD colors, const bool upd_edits, const bool upd_sliders, const bool rgba_mode = false) const {
                    localCtx->isUpdatingSync = true; // Set sync updating flag
                    if (localCtx->hPreviewBrush) { DeleteObject(localCtx->hPreviewBrush); } // Destroy preview color if it exists
                    // Compute both ARGB and RGBA color vars
                    DWORD normal_argb, rgba;
                    if (rgba_mode) {
                        normal_argb = _rotr(colors, 8);
                        rgba = colors;
                    } else {
                        normal_argb = colors;
                        rgba = _rotl(colors, 8);
                    }
                    localCtx->currentARGB = normal_argb;
                    localCtx->hPreviewBrush = CreateSolidBrush(RGB((normal_argb >> 16) & 0xFF, (normal_argb >> 8) & 0xFF, (normal_argb) & 0xFF)); // Create new preview color
                    InvalidateRect(GetDlgItem(hWnd, IDC_COLOR_PREVIEW), nullptr, TRUE); // Redraw preview
                    InvalidateRect(GetDlgItem(hWnd, IDC_COLOR_SQUARE), nullptr, FALSE); // Redraw color square
                    // Update sliders values if necessary
                    // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 4
                    if (upd_sliders) for (int dword_pos = 32, i = 0; i < 4; ++i) {
                        dword_pos -= 8;
                        SendDlgItemMessageW(hWnd, cs_sliders[i], TBM_SETPOS, TRUE, ((rgba >> dword_pos) & 0xFF));
                    }
                    // Update fields values if necessary
                    // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 3
                    if (upd_edits) for (int dword_pos = 32, i = 0; i < 4; ++i) {
                        dword_pos -= 8;
                        const BYTE val = (rgba >> dword_pos) & 0xFF;
                        if (hex_mode) {
                            wchar_t numBuf[8];
                            util::ultowhex(numBuf, val); // Convert to text
                            SetDlgItemTextW(hWnd, cs_edits[i], numBuf);
                            continue;
                        }
                        SetDlgItemInt(hWnd, cs_edits[i], val, FALSE);
                    }
                    localCtx->isUpdatingSync = false; // Disable sync updating flag
                }
            } SyncUI;
            switch (msg) {
                case WM_CREATE: { // Window initialization
                    CREATESTRUCTW* pcs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    pCtx = reinterpret_cast<ogl::PickColorContext*>(pcs->lpCreateParams);
                    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCtx));
                    pCtx->hSquareBmp = NULL; // Nullify color square bitmap var
                    pCtx->isDragging = false; // Ensure dragging mouse flag is disabled
                    // Draw the UI triggers
                    RECT rcPage; GetClientRect(hWnd, &rcPage); // Obtain page's internal dimensions
                    const int padding = ScaleTo(pCtx->ui_scale, 10);
                    const int space = ScaleTo(pCtx->ui_scale, 2);
                    const int marginX = rcPage.left + padding;
                    const int rightBound = rcPage.right - padding;
                    const int prvw = ScaleTo(pCtx->ui_scale, 50);
                    const int sldWidth = ScaleTo(pCtx->ui_scale, 120);
                    const int sldHeight = ScaleTo(pCtx->ui_scale, 20);
                    const int sldX = rightBound - sldWidth;
                    const int sldLblWidth = ScaleTo(pCtx->ui_scale, 42);
                    const int sldEditX = sldX + sldLblWidth + space;
                    const int btnHeight = ScaleTo(pCtx->ui_scale, 24);
                    const int beginY = rcPage.top + padding;
                    int currentY = beginY;
                    ShowWindow(hWnd, SW_SHOWNORMAL);
                    CreateWindowExW( // Color preview
                        WS_EX_STATICEDGE, CLASS_STATIC, L"", WS_CHILD | WS_VISIBLE, 
                        rightBound - prvw, currentY, prvw, prvw, hWnd, 
                        reinterpret_cast<HMENU>(IDC_COLOR_PREVIEW), g_hInstRelay, nullptr
                    );
                    currentY += prvw;
                    CreateWindowExW( // Hex mode toggle
                        0, CLASS_BUTTON, L"Hex mode", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                        sldX + space, currentY, sldWidth, sldHeight, hWnd, 
                        reinterpret_cast<HMENU>(IDC_PICKCOLOR_HEX), g_hInstRelay, nullptr
                    );
                    SendDlgItemMessageW(hWnd, IDC_PICKCOLOR_HEX, BM_SETCHECK, hex_mode ? BST_CHECKED : BST_UNCHECKED, 0);
                    {
                        const LPCWSTR labels[] = { L"Red:", L"Green:", L"Blue:", L"Opac:" };
                        const LPARAM range = MAKELPARAM(0, MAX_BYTE); // Sliders range
                        // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 4
                        for (int i = 0; i < 4; ++i) {
                            currentY += sldHeight + space;
                            CreateWindowExW( // Fields labels
                                0, CLASS_STATIC, labels[i], WS_CHILD | WS_VISIBLE, 
                                sldX, currentY, sldLblWidth, sldHeight, hWnd, 
                                NULL, g_hInstRelay, nullptr
                            );
                            const HWND hField = CreateWindowExW( // Fields
                                WS_EX_CLIENTEDGE, CLASS_EDIT, L"", WS_CHILD | WS_VISIBLE, 
                                sldEditX, currentY, prvw, sldHeight, hWnd, 
                                reinterpret_cast<HMENU>(cs_edits[i]), g_hInstRelay, nullptr
                            );
                            currentY += sldHeight;
                            CreateWindowExW( // Sliders
                                0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 
                                sldX, currentY, sldWidth, sldHeight, hWnd, 
                                reinterpret_cast<HMENU>(cs_sliders[i]), g_hInstRelay, nullptr
                            );
                            SendDlgItemMessageW(hWnd, cs_sliders[i], TBM_SETRANGE, TRUE, range); // Set sliders range
                            toggleHex(hField); // Set field mode (base 10 or hex)
                        }
                    }
                    const int square_side = ScaleTo(pCtx->ui_scale, 140);
                    currentY += sldHeight - square_side;
                    const HWND hColorSquare = CreateWindowExW( // Color square
                        WS_EX_CLIENTEDGE, CLASS_STATIC, L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY, 
                        marginX, currentY, square_side, square_side, hWnd, 
                        reinterpret_cast<HMENU>(IDC_COLOR_SQUARE), g_hInstRelay, nullptr
                    );
                    SetWindowSubclass(hColorSquare, SquareSubclassProc, IDC_COLOR_SQUARE, reinterpret_cast<DWORD_PTR>(pCtx)); // Subclass the square
                    int currentX = marginX;
                    currentY = beginY;
                    const LPCWSTR labels[] = { L"Reset", L"Apply" };
                    // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 2
                    for (int i = 0; i < 2; ++i) {
                        CreateWindowExW(
                            0, CLASS_BUTTON, labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                            currentX, currentY, prvw, btnHeight, hWnd, 
                            reinterpret_cast<HMENU>(IDC_PICKCOLOR_RESET + i), g_hInstRelay, nullptr
                        );
                        currentX += prvw + space;
                    }
                    reloadTextFont(hWnd, pCtx->txtFont);
                    SyncUI(hWnd, pCtx, pCtx->initialARGB, true, true);
                    return 0;
                }
                case WM_HSCROLL: { // Slider scroll
                    if (!pCtx->isUpdatingSync) { // Verify absence of sync updating flag
                        DWORD rgba;
                        // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 4
                        for (int i = 0; i < 4; ++i) {
                            reinterpret_cast<BYTE*>(&rgba)[i] = static_cast<BYTE>(SendDlgItemMessageW(hWnd, cs_sliders[3 - i], TBM_GETPOS, 0, 0)); // Get all slider values
                        }
                        SyncUI(hWnd, pCtx, rgba, true, false, true);
                        return 0;
                    }
                    break;
                }
                case WM_COMMAND: { // User interaction with the window
                    const WORD wmId = LOWORD(wp), notifCode = HIWORD(wp);
                    switch (notifCode) {
                        case EN_KILLFOCUS: { // A trigger lost focus
                            if (!hex_mode && wmId >= cs_edits[0] && wmId <= cs_edits[edits_countIdx]) { // Filter for fields and disable in hex mode
                                const uint32_t value = GetDlgItemInt(hWnd, wmId, nullptr, FALSE);
                                if (value > MAX_BYTE) { SetDlgItemInt(hWnd, wmId, MAX_BYTE, FALSE); } // Clamp to 255
                                return 0;
                            }
                            break;
                        }
                        case EN_CHANGE: { // Field value changed
                            if (!pCtx->isUpdatingSync) { // Verify absence of sync updating flag
                                DWORD argb = pCtx->currentARGB; // Copy current ARGB value
                                // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 4
                                for (int i = 0; i < 4; ++i) {
                                    if (cs_edits[i] != wmId) continue; // Match ID with fields array index
                                    reinterpret_cast<BYTE*>(&argb)[(2 - i) & 3] = readField(hWnd, wmId); // Update the corresponding value
                                    break;
                                }
                                SyncUI(hWnd, pCtx, argb, false, true);
                                return 0;
                            }
                            break;
                        }
                        case BN_CLICKED: { // A button was clicked
                            switch (wmId) {
                                case IDC_PICKCOLOR_HEX: { // Toggled hex mode
                                    hex_mode = (SendMessageW(reinterpret_cast<HWND>(lp), BM_GETCHECK, 0, 0) == BST_CHECKED); // Check the state we're toggling to
                                    // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 4
                                    for (int i = 0; i < 4; ++i) {
                                        const HWND hField = GetDlgItem(hWnd, cs_edits[i]); // Get field handle
                                        if (hField) { // Ignore if invalid (should never be)
                                            toggleHex(hField); // Update field mode (base 10 or hex)
                                            wchar_t szBuf[8];
                                            GetWindowTextW(hField, szBuf, 8); // Read the text
                                            DWORD val = wcstoul(szBuf, nullptr, hex_mode ? 10 : 16); // Convert to DWORD with base from previous mode
                                            if (val > MAX_BYTE) val = MAX_BYTE; // Clamp to 255
                                            hex_mode ? util::ultowhex(szBuf, val) : _ultow_s(static_cast<uint32_t>(val), szBuf, 8, 10); // Convert to text
                                            SetWindowTextW(hField, szBuf); // Update text
                                        }
                                    }
                                    return 0;
                                }
                                case IDC_PICKCOLOR_APPLY: { // Pressed apply button
                                    pCtx->initialARGB = pCtx->currentARGB; // Update initial ARGB as current
                                    pCtx->wasApplied = true;
                                    cfgChangeAndApply(SEC_OGLSPOOFER, KEY_OGLCOLOR, &pCtx->initialARGB, TYPE_DWORD); // Update runtime setting
                                    OglSettingLoad(KEY_OGLCOLOR); // Update OpenGL Spoofer color
                                    g_OglSomethingChanged.store(true, std::memory_order_release);
                                    return 0;
                                }
                                case IDC_PICKCOLOR_RESET: { // Pressed reset button
                                    SyncUI(hWnd, pCtx, pCtx->initialARGB, true, true);
                                    return 0;
                                }
                            }
                            break;
                        }
                    }
                    if (wmId == IDOK) { goto focus_window; }
                    if (wmId == IDCANCEL) { goto closing_routine; }
                    break;
                }
                case WM_LBUTTONDOWN: { focus_window: // Left click
                    SetFocus(GetDlgItem(hWnd, IDC_COLOR_PREVIEW));
                    return 0;
                }
                case IDC_COLOR_SQUARE: { // Cursor moved on color square
                    SyncUI(hWnd, pCtx, lp, true, true);
                    return 0;
                }
                case WM_CTLCOLORSTATIC: { // Static trigger draw procedure
                    if (pCtx && reinterpret_cast<HWND>(lp) == GetDlgItem(hWnd, IDC_COLOR_PREVIEW)) { // For color preview, return relevant color
                        return reinterpret_cast<INT_PTR>(pCtx->hPreviewBrush);
                    }
                    break;
                }
                case WM_CLOSE: { closing_routine: // Window needs to be closed
                    const HWND hParent = GetParent(hWnd);
                    if (hParent && IsWindow(hParent)) {
                        EnableWindow(hParent, TRUE);
                        // Empty the parent's message queue from while it was blocked
                        MSG discardMsg;
                        while (PeekMessageW(&discardMsg, hParent, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE));
                        while (PeekMessageW(&discardMsg, hParent, WM_SYSCOMMAND, WM_SYSCOMMAND, PM_REMOVE));
                        SetForegroundWindow(hParent);
                        SetActiveWindow(hParent);
                    }
                    break;
                }
            }
            return DefWindowProcW(hWnd, msg, wp, lp);
        }
    };
    // Launcher code block
    HWND hWndPicker;
    {
        static constexpr wchar_t colorPickerClass[] = L"Dynamic_color_picker_Class"; // Class name for color selector window
        const HWND hTopDlg = GetAncestor(hParentPage, GA_ROOT); // Get parent root (propsheet)
        if (WNDCLASSW wcCheck{}; !GetClassInfoW(g_hInstRelay, colorPickerClass, &wcCheck)) {
            // Build up color selector window class
            WNDCLASSW cs{};
            cs.lpfnWndProc = ColorSelector::Proc; // Assign callback function
            cs.hInstance = g_hInstRelay; // Assign parent process instance
            cs.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(COLOR_BTNFACE + 1)); // Set background color
            cs.lpszClassName = L"Dynamic_color_picker_Class"; // Set unique class name
            cs.hCursor = LoadCursorW(NULL, IDC_ARROW); // Set default mouse cursor
            RegisterClassW(&cs); // Register the class
        }
        RECT rcParent; GetWindowRect(hParentPage, &rcParent); // Obtain propsheet's internal dimensions
        RECT wr = { 0, 0, ScaleTo(pCtx->ui_scale, 300), ScaleTo(pCtx->ui_scale, 275) }; // Prepare dimensions for color selector
        const int excess = rcParent.left + wr.right - rcParent.right; // Calculate excess width compared to propsheet
        wr.left = rcParent.left - excess / 2; // Divide excess between right and left sides as to center the window
        wr.top = rcParent.top + ScaleTo(pCtx->ui_scale, 120); // Set height
        wr.right += wr.left, wr.bottom += wr.top; // Apply left and top offsets to right and bottom
        AdjustWindowRectEx(&wr, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME); // Adjust rect for color selector window to account for things like borders
        pCtx->txtFont = NULL; // Nullify font var initially
        createGuiFont(pCtx->txtFont, ScaleTo(pCtx->ui_scale, 14));
        hWndPicker = CreateWindowExW( // Summon the window (not visible yet)
            WS_EX_DLGMODALFRAME, colorPickerClass, L"Color selector", 
            WS_POPUP | WS_CAPTION | WS_SYSMENU, 
            wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, 
            hTopDlg, NULL, g_hInstRelay, pCtx
        );
        if (!hWndPicker) return false; // Verify
        EnableWindow(hTopDlg, FALSE); // Block interactions with propsheet
    }
    // Message processing loop
    MSG msg;
    while (IsWindow(hWndPicker) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hWndPicker, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (pCtx->wasApplied && ogl::g_hOglTabDlg) SendMessageW(ogl::g_hOglTabDlg, WM_UPDATECOLORFIELD, 0, 0); // If any change was applied, message the propsheet tab
    if (pCtx->txtFont && pCtx->txtFont != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(pCtx->txtFont); // Destroy the font used for the color selector window
    delete pCtx;
    return true;
}


// Text fadeout slideshow animation trigger
static void TriggerToastAnim(const HWND hStatic, const LPCWSTR text) {
    struct ToastAnim { // Worker functions
        // UI element procedures
        static LRESULT CALLBACK Proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
            ToastAnimContext *const pCtx = reinterpret_cast<ToastAnimContext*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            if (!pCtx) return DefWindowProcW(hWnd, msg, wp, lp);
            const WNDPROC pfnOldProc = pCtx->pfnOldProc;
            switch (msg) {
                case WM_PAINT: { // Drawing procedure
                    PAINTSTRUCT ps;
                    const HDC hdc = BeginPaint(hWnd, &ps);
                    RECT rc; GetClientRect(hWnd, &rc);
                    // Draw background
                    const HBRUSH hBgBrush = CreateSolidBrush(pCtx->bg);
                    FillRect(hdc, &rc, hBgBrush);
                    DeleteObject(hBgBrush);
                    // Draw text
                    COLORREF txtColor = 0;
                    const BYTE a = pCtx->opacity;
                    for (int i = 0; i < 3; ++i) { reinterpret_cast<BYTE*>(&txtColor)[i] = reinterpret_cast<BYTE*>(&pCtx->bg)[i] * a; }
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, txtColor);
                    // Draw the text
                    HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
                    const HFONT hOldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));
                    DrawTextW(hdc, pCtx->text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, hOldFont);
                    EndPaint(hWnd, &ps);
                    return 0;
                }
                case WM_NCDESTROY: { // Destruction routine
                    pCtx->bOrphaned.store(true, std::memory_order_release);
                    SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
                    std::atomic_thread_fence(std::memory_order_release);
                    break;
                }
            }
            return CallWindowProcW(pfnOldProc, hWnd, msg, wp, lp);
        }
        // Handler thread
        static unsigned __stdcall Thread(void* pArg) {
            if (!pArg) return 1;
            const HWND hStatic = static_cast<HWND>(pArg);
            ToastAnimContext *const pCtx = reinterpret_cast<ToastAnimContext*>(GetWindowLongPtrW(hStatic, GWLP_USERDATA));
            if (!pCtx) return 1;
            bool wasnt_orphaned = true;
            // Loop with variable t as 0 by default, restarting as long as t is lower than 3, with t getting incremented each turn
            for (int t = 0; t < 3; ++t) {
                Sleep(500);
                // If UI element was orphaned (mother window closed), abort all operations
                if (pCtx->bOrphaned.load(std::memory_order_acquire)) {
                    wasnt_orphaned = false;
                    break;
                }
                // Decrease opacity and apply
                pCtx->opacity -= 85;
                RedrawWindow(hStatic, nullptr, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            // If UI element wasn't orphaned, properly erase it from screen
            if (wasnt_orphaned && IsWindow(hStatic)) {
                const WNDPROC pfnOldProc = pCtx->pfnOldProc;
                SetWindowLongPtrW(hStatic, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pfnOldProc));
                SetWindowLongPtrW(hStatic, GWLP_USERDATA, 0);
                RedrawWindow(hStatic, nullptr, NULL, RDW_INVALIDATE | RDW_ERASE);
            }
            delete pCtx; // Destroy context data
            return 0;
        }
    };
    // Startup code block
    { // If an animation is already running, properly terminate it
        ToastAnimContext *const pOldCtx = reinterpret_cast<ToastAnimContext*>(GetWindowLongPtrW(hStatic, GWLP_USERDATA));
        if (pOldCtx) {
            pOldCtx->bOrphaned.store(true, std::memory_order_release);
            SetWindowLongPtrW(hStatic, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pOldCtx->pfnOldProc));
            SetWindowLongPtrW(hStatic, GWLP_USERDATA, 0);
        }
    }
    // Create context data for a new animation
    ToastAnimContext *const pCtx = new (std::nothrow) ToastAnimContext();
    if (!pCtx) return;
    const HDC hdc = GetDC(hStatic);
    pCtx->bg = GetPixel(hdc, 1, 1); // Get base background color
    ReleaseDC(hStatic, hdc);
    pCtx->opacity = MAX_BYTE;
    pCtx->text = text;
    pCtx->pfnOldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hStatic, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ToastAnim::Proc)));
    SetWindowLongPtrW(hStatic, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCtx));
    RedrawWindow(hStatic, nullptr, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    _beginthreadex(nullptr, 0, &ToastAnim::Thread, hStatic, 0, nullptr); // Start animation thread
}


// OpenGL Spoofer settings page procedures - handles interactions
INT_PTR CALLBACK OGLSpoofDlgProc(HWND hwndDlg, UINT msg, WPARAM wp, LPARAM lp) {
    static int PshUiScale;
    // Define handles for UI triggers
    static HWND hBtnUseOgl, hBtnSyncState, hSyncAnim, hLedIcon, hLedText;
    static HWND hCheckShow, hCheckTop, hEditX, hEditY, hEditWidth, hEditHeight, hEditColor, hEditFrameTime, hBtnHighPrecis;
    static HWND hBtnPickCoord, hBtnScaleSize, hBtnPickColor;
    static constexpr wchar_t synced[] = L"Synced!";
    static SettingField oglCfgTrigs[] = {
        { &hBtnUseOgl, CLASS_BUTTON, L"", IDC_OGL_USEOGL, BS_CHECKBOX | BS_PUSHLIKE | BS_MULTILINE, KEY_USEOGL }, 
        { &hBtnSyncState, CLASS_BUTTON, L"", IDC_OGL_SYNCSTATE, BS_PUSHBUTTON | BS_ICON }, 
        { &hCheckShow, CLASS_BUTTON, L"Show window", IDC_OGL_SHOW, BS_AUTOCHECKBOX, KEY_OGLSHOW }, 
        { &hCheckTop, CLASS_BUTTON, L"Always on top", IDC_OGL_TOP, BS_AUTOCHECKBOX, KEY_OGLTOP }, 
        { &hEditX, CLASS_EDIT, L"X Coord:", IDC_OGL_XCOORD, ES_NUMBER | WS_BORDER, KEY_OGLXCOORD, 5 }, 
        { &hEditY, CLASS_EDIT, L"Y Coord:", IDC_OGL_YCOORD, ES_NUMBER | WS_BORDER, KEY_OGLYCOORD, 5 }, 
        { &hBtnPickCoord, CLASS_BUTTON, L"", IDC_OGL_PICK_COORD, BS_CHECKBOX | BS_PUSHLIKE | BS_ICON }, 
        { &hEditWidth, CLASS_EDIT, L"Width:", IDC_OGL_WIDTH, ES_NUMBER | WS_BORDER, KEY_OGLWIDTH, 5 }, 
        { &hEditHeight, CLASS_EDIT, L"Height", IDC_OGL_HEIGHT, ES_NUMBER | WS_BORDER, KEY_OGLHEIGHT, 5 }, 
        { &hBtnScaleSize, CLASS_BUTTON, L"", IDC_OGL_SCALE_SIZE, BS_PUSHBUTTON | BS_ICON }, 
        { &hEditColor, CLASS_EDIT, L"Color (0xARGB):", IDC_OGL_COLOR, ES_AUTOHSCROLL | ES_UPPERCASE | WS_BORDER, KEY_OGLCOLOR, 8 }, 
        { &hBtnPickColor, CLASS_BUTTON, L"", IDC_OGL_PICK_COLOR, BS_PUSHBUTTON | BS_ICON }, 
        { &hEditFrameTime, CLASS_EDIT, L"Frametime cap (ms):", IDC_OGL_FRAMETIME, ES_NUMBER | WS_BORDER, KEY_OGLFRAMETIME, 4 }, 
        { &hBtnHighPrecis, CLASS_BUTTON, L"High consistency", IDC_OGL_PRECIS, BS_AUTOCHECKBOX, KEY_OGLPRECIS }, 
        { nullptr, nullptr, nullptr, 0, 0, 0, 0 }
    };
    struct LocalKbdHookOgl {
        // OGL local keyboard hook procedures
        static LRESULT CALLBACK Proc(int nCode, WPARAM wp, LPARAM lp) {
            if (ogl::g_pickingCoords && nCode >= 0 && (lp & 0x80000000) == 0) {
                if (wp == VK_RETURN) { // If key pressed was enter
                    PostMessageW(ogl::g_hOglTabDlg, WM_PICKCOORDS, 0, 0);
                    return 1;
                }
            }
            return CallNextHookEx(ogl::g_hLocalKbdHook, nCode, wp, lp); // Pass the call over to windows's builtin hook (if the code above didn't swallow it)
        }
        // Function for disabling the keyboard hook (if enabled)
        __declspec(noinline) static void Disable(void) { UnhookWindowsHookEx(ogl::g_hLocalKbdHook); ogl::g_hLocalKbdHook = NULL; }
    };
    struct { // Operator for refreshing OpenGL status LED state
        __declspec(noinline) void operator()(const bool isChecked) {
            SendMessageW(hLedIcon, STM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(isChecked ? ogl::g_hIconGreenLight : ogl::g_hIconRedLight));
            SetWindowTextW(hLedText, isChecked ? L"Running" : L"Stopped");
        }
    } refreshLed;
    switch (msg) {
        case WM_INITDIALOG: { // Propsheet tab initialization
            ogl::g_hOglTabDlg = hwndDlg;
            PshUiScale = g_uiScale;
            // Draw the UI triggers
            RECT rcPage; GetClientRect(hwndDlg, &rcPage); // Obtain page's internal dimensions
            const int padding = Scale(8);
            const int space = Scale(2);
            const int labelHeight = Scale(20);
            const int ctrlHeight = Scale(24);
            const int marginX = rcPage.left + padding;
            const int BuddyWidth = Scale(32);
            const int buddyX = rcPage.right - padding - BuddyWidth;
            const int limitBeforeBuddy = buddyX - space;
            const int siblingCtrlWidth = (limitBeforeBuddy - marginX - space) / 2;
            const int secondSiblingCtrlX = marginX + siblingCtrlWidth + space;
            const int soloCtrlWidth = limitBeforeBuddy - marginX;
            int currentY = rcPage.top + padding;
            { // Create icons and draw main toggle logic
                const int scaled16 = Scale(16);
                ogl::g_hIconOglSync = static_cast<HICON>(LoadImageW(g_hInstRelay, MAKEINTRESOURCEW(IDI_SYNC_ICON), IMAGE_ICON, scaled16, scaled16, LR_SHARED));
                ogl::g_hIconRedLight = static_cast<HICON>(LoadImageW(g_hInstRelay, MAKEINTRESOURCEW(IDI_RED_LIGHT_ICON), IMAGE_ICON, scaled16, scaled16, LR_SHARED));
                ogl::g_hIconGreenLight = static_cast<HICON>(LoadImageW(g_hInstRelay, MAKEINTRESOURCEW(IDI_GREEN_LIGHT_ICON), IMAGE_ICON, scaled16, scaled16, LR_SHARED));
                ogl::g_hIconMouse = static_cast<HICON>(LoadImageW(g_hInstRelay, MAKEINTRESOURCEW(IDI_MOUSE_CLICK_ICON), IMAGE_ICON, scaled16, scaled16, LR_SHARED));
                ogl::g_hIconScale = static_cast<HICON>(LoadImageW(g_hInstRelay, MAKEINTRESOURCEW(IDI_TRIANGLE_RULER_ICON), IMAGE_ICON, scaled16, scaled16, LR_SHARED));
                ogl::g_hIconColor = static_cast<HICON>(LoadImageW(g_hInstRelay, MAKEINTRESOURCEW(IDI_PAINT_BRUSH_ICON), IMAGE_ICON, scaled16, scaled16, LR_SHARED));
                const int mainToggleHeight = Scale(48);
                int Xpos = marginX;
                const SettingField& mainToggle = oglCfgTrigs[0];
                const SettingField& syncStateKey = *(&mainToggle + 1);
                *(mainToggle.hCtrlPtr) = CreateWindowW( // Main toggle
                    CLASS_BUTTON, L"", WS_CHILD | WS_VISIBLE | mainToggle.styles, 
                    Xpos, currentY, siblingCtrlWidth, mainToggleHeight, hwndDlg, 
                    reinterpret_cast<HMENU>(mainToggle.id), g_hInstRelay, 0
                );
                Xpos += siblingCtrlWidth + space;
                const int syncHeight = currentY + space / 2;
                *(syncStateKey.hCtrlPtr) = CreateWindowW( // Sync state button
                    CLASS_BUTTON, syncStateKey.text, WS_CHILD | WS_VISIBLE | syncStateKey.styles, 
                    Xpos, syncHeight, BuddyWidth, ctrlHeight, hwndDlg, 
                    reinterpret_cast<HMENU>(syncStateKey.id), g_hInstRelay, 0
                );
                const int txtWidth = mainToggleHeight * 2;
                const int Xanim = Xpos + BuddyWidth + space;
                hSyncAnim = CreateWindowW(
                    CLASS_STATIC, L"", WS_CHILD | WS_VISIBLE, 
                    Xanim, syncHeight, txtWidth, ctrlHeight, hwndDlg, 
                    NULL, g_hInstRelay, 0
                );
                currentY += ctrlHeight + space;
                // Set sync button icon
                if (ogl::g_hIconOglSync) { SendMessageW(*(syncStateKey.hCtrlPtr), BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(ogl::g_hIconOglSync)); }
                hLedIcon = CreateWindowW( // Led icon element
                    CLASS_STATIC, L"", WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZEIMAGE, 
                    Xpos, currentY, scaled16, scaled16, hwndDlg, NULL, g_hInstRelay, 0
                );
                Xpos += scaled16 + space;
                hLedText = CreateWindowW( // Led text
                    CLASS_STATIC, L"", WS_CHILD | WS_VISIBLE, 
                    Xpos, currentY - space, txtWidth, ctrlHeight, hwndDlg, 
                    NULL, g_hInstRelay, 0
                );
                currentY += (mainToggleHeight + padding - ctrlHeight);
                const int sepWidth = rcPage.right - padding - marginX;
                CreateWindowW( // Horizontal separator line
                    CLASS_STATIC, L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 
                    marginX, currentY, sepWidth, Scale(1), hwndDlg, 
                    NULL, g_hInstRelay, 0
                );
                currentY += padding;
            }
            int savedY;
            // Loop that restarts for each entry in oglCfgTrigs
            for (SettingField& trig : oglCfgTrigs) {
                if (!trig.hCtrlPtr) break;
                trig.isDirty = false;
                const bool isButton = (trig.className[0] == L'B');
                int ctrlWidth = soloCtrlWidth, itemX = marginX;
                if (trig.id < 4180) { // Exclude main toggle
                    continue;
                } else if (trig.id < 4230) { // Checkboxes
                } else if (trig.id < 4260) { // Sibling fields
                    ctrlWidth = siblingCtrlWidth;
                    if (trig.id % 2 == 0) {
                        savedY = currentY;
                    } else {
                        itemX = secondSiblingCtrlX;
                        currentY = savedY;
                    }
                } else if (trig.id < 4540) { // Solo fields without buddy button
                } else if (trig.id < 4565) { // Solo fields
                    savedY = currentY;
                } else if (trig.id < 4600) { // Buddy buttons
                    itemX = buddyX;
                    ctrlWidth = BuddyWidth;
                    currentY = savedY + labelHeight;
                }
                if (!isButton) {
                    CreateWindowW(
                        CLASS_STATIC, trig.text, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 
                        itemX, currentY, ctrlWidth, labelHeight, hwndDlg, NULL, 
                        g_hInstRelay, 0
                    );
                    currentY += labelHeight; // Update height point for trigger after label
                }
                // Create UI trigger, and set button text if applicable
                const LPCWSTR ctrlText = isButton ? trig.text : L"";
                *(trig.hCtrlPtr) = CreateWindowW(
                    trig.className, ctrlText, WS_CHILD | WS_VISIBLE | trig.styles, 
                    itemX, currentY, ctrlWidth, ctrlHeight, hwndDlg, reinterpret_cast<HMENU>(trig.id), 
                    g_hInstRelay, 0
                );
                if (trig.className[0] == L'E') SendMessageW(*(trig.hCtrlPtr), EM_LIMITTEXT, trig.maxLength, 0); // Set input text limit if applicable
                currentY += ctrlHeight + space; // Update height point for next element
            }
            reloadTextFont(hwndDlg, g_hPshFont); // Set font
            // Set icons
            if (ogl::g_hIconMouse) { SendMessageW(hBtnPickCoord, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(ogl::g_hIconMouse)); }
            if (ogl::g_hIconScale) { SendMessageW(hBtnScaleSize, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(ogl::g_hIconScale)); }
            if (ogl::g_hIconColor) { SendMessageW(hBtnPickColor, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(ogl::g_hIconColor)); }
            // Loop that restarts for each entry in oglCfgTrigs
            for (const SettingField& trig : oglCfgTrigs) {
                if (!trig.hCtrlPtr) break; if (!trig.cfgKeyId) continue;
                if (trig.id < 4230) { // Checkboxes
                    if (trig.id == IDC_OGL_SYNCSTATE) continue;
                    const bool isChecked = *static_cast<const bool*>(g_cfg.get(SEC_OGLSPOOFER, trig.cfgKeyId));
                    SendMessageW(*(trig.hCtrlPtr), BM_SETCHECK, isChecked ? BST_CHECKED : BST_UNCHECKED, 0); // Set state
                    if (trig.id == IDC_OGL_USEOGL) { // Main toggle
                        SetWindowTextW(*(trig.hCtrlPtr), isChecked ? OGL_TOGGLE_STOP : OGL_TOGGLE_START);
                        refreshLed(isChecked);
                    }
                } else if (trig.id < 4565) { // Fields
                    wchar_t numBuf[16];
                    if (trig.cfgKeyId == KEY_OGLCOLOR) { // Color field (DWORD)
                        const DWORD val = *static_cast<const DWORD*>(g_cfg.get(SEC_OGLSPOOFER, trig.cfgKeyId)); // Get runtime setting
                        util::ultowhex(numBuf, val, 8); // Convert to text
                        SetWindowTextW(*(trig.hCtrlPtr), numBuf); // Set text
                        const WNDPROC pfnOldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(*(trig.hCtrlPtr), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HexSubclassProc)));
                        SetWindowLongPtrW(*(trig.hCtrlPtr), GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pfnOldProc));
                    } else if (trig.styles & ES_NUMBER) { // Number fields
                        const int intVal = *static_cast<const int*>(g_cfg.get(SEC_OGLSPOOFER, trig.cfgKeyId)); // Get runtime setting
                        SetDlgItemInt(hwndDlg, trig.id, intVal, FALSE); // Set value
                    }
                }
            }
            return TRUE;
        }
        case WM_COMMAND: { // User interaction with the window
            const WORD wmId = LOWORD(wp), notifCode = HIWORD(wp);
            if (notifCode == EN_CHANGE || (notifCode == BN_CLICKED && wmId < 4565 && wmId >= 4180)) { // If any UI trigger was adjusted (except buddy buttons and main toggle)
                // Loop that restarts for each entry in oglCfgTrigs
                for (SettingField& trig: oglCfgTrigs) { if (wmId == trig.id) { trig.isDirty = true; break; }} // Update "isDirty" flag when ID matches
                PropSheet_Changed(GetParent(hwndDlg), hwndDlg); // Enable apply button
                break;
            }
            if (notifCode == BN_CLICKED) {
                switch (wmId) { // We get there if a buddy button was adjusted, or the main toggle
                    case IDC_OGL_USEOGL: { // Main toggle
                        bool nextState = !(SendMessageW(reinterpret_cast<HWND>(lp), BM_GETCHECK, 0, 0) == BST_CHECKED);
                        if (OglSpooferManage(nextState ? 2 : 1, reinterpret_cast<HWND>(lp))) {
                            cfgChangeAndApply(SEC_OGLSPOOFER, KEY_USEOGL, &nextState, TYPE_BOOL);
                            refreshLed(nextState);
                        } else {
                            TriggerToastAnim(hSyncAnim, synced);
                            nextState = (SendMessageW(reinterpret_cast<HWND>(lp), BM_GETCHECK, 0, 0) == BST_CHECKED);
                        }
                        refreshLed(nextState);
                        return TRUE;
                    }
                    case IDC_OGL_SYNCSTATE: { // Sync state button
                        OglSpooferManage(0, hBtnUseOgl); // Trigger sync
                        TriggerToastAnim(hSyncAnim, synced);
                        const bool isChecked = (SendMessageW(hBtnUseOgl, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        refreshLed(isChecked);
                        return TRUE;
                    }
                    case IDC_OGL_PICK_COORD: { // Coords picker
                        WPARAM toggleState;
                        if ((ogl::g_pickingCoords = !ogl::g_pickingCoords)) {
                            toggleState = BST_CHECKED;
                            ogl::g_hLocalKbdHook = SetWindowsHookExW(WH_KEYBOARD, LocalKbdHookOgl::Proc, NULL, GetCurrentThreadId());
                        } else {
                            toggleState = BST_UNCHECKED;
                            LocalKbdHookOgl::Disable();
                        }
                        SendMessageW(reinterpret_cast<HWND>(lp), BM_SETCHECK, toggleState, 0);
                        return TRUE;
                    }
                    case IDC_OGL_SCALE_SIZE: { // Size settings scaler (based on DPI)
                        const int trigIds[2] = { IDC_OGL_WIDTH, IDC_OGL_HEIGHT }; // Trigers to update (width, height)
                        int lengths[2];
                        // Loop with variable i as 0 by default, restarting as long as i is lower than 2, with i getting incremented each turn
                        for (int i = 0; i < 2; ++ i) {
                            lengths[i] = ScaleTo(PshUiScale, GetDlgItemInt(hwndDlg, trigIds[i], nullptr, FALSE)); // Calculate new lengths and store them
                            if (lengths[i] > 50000) return TRUE; // Ignore operation if one gets over 50k
                        }
                        // Loop with variable i as 0 by default, restarting as long as i is lower than 2, with i getting incremented each turn
                        for (int i = 0; i < 2; ++i) {
                            SetDlgItemInt(hwndDlg, trigIds[i], lengths[i], FALSE);
                        }
                        return TRUE;
                    }
                    case IDC_OGL_PICK_COLOR: { // Color picker
                        wchar_t colorStr[16];
                        GetWindowTextW(hEditColor, colorStr, 16);
                        const DWORD activeDWORD = wcstoul(colorStr, nullptr, 16);
                        ogl::PickColorContext* pCtx = new (std::nothrow) ogl::PickColorContext();
                        if (!pCtx) return TRUE;
                        pCtx->initialARGB = activeDWORD;
                        pCtx->ui_scale = PshUiScale;
                        DynamicColorPicker(hwndDlg, pCtx);
                        return TRUE;
                    }
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: { // Left click
            SetFocus(hwndDlg);
            return TRUE;
        }
        case WM_UPDATECOLORFIELD: { // Update color field
            const HWND hApplyBtn = GetDlgItem(GetParent(hwndDlg), ID_APPLY_NOW);
            const bool isPshChanged = (hApplyBtn && IsWindowEnabled(hApplyBtn));
            const DWORD val = *static_cast<const DWORD*>(g_cfg.get(SEC_OGLSPOOFER, KEY_OGLCOLOR));
            wchar_t numBuf[16];
            util::ultowhex(numBuf, val, 8); // Convert to text
            SetWindowTextW(hEditColor, numBuf);
            if (!isPshChanged) PropSheet_UnChanged(GetParent(hwndDlg), hwndDlg); // Disable apply button if it was disabled before
            return TRUE;
        }
        case WM_PICKCOORDS: { // Picking coords
            POINT cur; GetCursorPos(&cur); // Get cursor position
            const int coords[2] = { cur.x, cur.y }; // Coordinates
            const int trigIds[2] = { IDC_OGL_XCOORD, IDC_OGL_YCOORD }; // Trigers to update (x & y coords)
            // Loop with variable i as 0 by default, restarting as long as i is lower than 2, with i getting incremented each turn
            for (int i = 0; i < 2; ++i) { SetDlgItemInt(hwndDlg, trigIds[i], coords[i], FALSE); }
            LocalKbdHookOgl::Disable();
            ogl::g_pickingCoords = false;
            SendMessageW(hBtnPickCoord, BM_SETCHECK, BST_UNCHECKED, 0);
            return TRUE;
        }
        case WM_NOTIFY: { // Notification of an event with the window
            const LPNMHDR pNmhdr = reinterpret_cast<LPNMHDR>(lp);
            if (pNmhdr->code == PSN_APPLY) {
                bool hasChanged = false;
                // Loop that restarts for each entry in oglCfgTrigs
                for (SettingField& trig : oglCfgTrigs) {
                    if (!trig.hCtrlPtr) break; if (!trig.cfgKeyId || !trig.isDirty) continue;
                    if (trig.id < 4230) { // If it's a checkbox
                        if (trig.id < 4180) continue; // Bypass main toggle
                        // Update related runtime setting according to state
                        const bool checked = (SendMessageW(*(trig.hCtrlPtr), BM_GETCHECK, 0, 0) == BST_CHECKED);
                        cfgChangeAndApply(SEC_OGLSPOOFER, trig.cfgKeyId, &checked, TYPE_BOOL);
                    } else if (trig.id < 4565) { // If it's a field
                        if (trig.id == IDC_OGL_COLOR) { // Color field (DWORD)
                            wchar_t numBuf[16]; GetWindowTextW(*(trig.hCtrlPtr), numBuf, 16); // Get text
                            const DWORD colorVal = wcstoul(numBuf, nullptr, 16); // Convert
                            cfgChangeAndApply(SEC_OGLSPOOFER, trig.cfgKeyId, &colorVal, TYPE_DWORD); // Update runtime setting
                        } else if (trig.styles & ES_NUMBER) { // Number fields
                            const int intVal = GetDlgItemInt(hwndDlg, trig.id, nullptr, FALSE); // Get value
                            cfgChangeAndApply(SEC_OGLSPOOFER, trig.cfgKeyId, &intVal, TYPE_INT); // Update runtime setting
                        }
                    }
                    // Send new settings to OpenGL render thread
                    OglSettingLoad(trig.cfgKeyId);
                    hasChanged = true;
                    trig.isDirty = false;
                }
                if (hasChanged) g_OglSomethingChanged.store(true, std::memory_order_release);
                SetWindowLongPtrW(hwndDlg, DWLP_MSGRESULT, PSNRET_NOERROR); // Tell windows everything went right for ok button to close the window after applying
                return TRUE;
            }
            if (pNmhdr->code == PSN_KILLACTIVE) {
                if (ogl::g_pickingCoords) {
                    ogl::g_pickingCoords = false;
                    LocalKbdHookOgl::Disable();
                    SendMessageW(hBtnPickCoord, BM_SETCHECK, BST_UNCHECKED, 0);
                    SetWindowLongPtrW(hwndDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
                    return TRUE;
                }
            }
            break;
        }
        case WM_NCDESTROY: { // Destruction routine
            // Destroy any custom icons from that page
            if (ogl::g_hIconOglSync) { DestroyIcon(ogl::g_hIconOglSync); ogl::g_hIconOglSync = NULL; }
            if (ogl::g_hIconRedLight) { DestroyIcon(ogl::g_hIconRedLight); ogl::g_hIconRedLight = NULL; }
            if (ogl::g_hIconGreenLight) { DestroyIcon(ogl::g_hIconGreenLight); ogl::g_hIconGreenLight = NULL; }
            if (ogl::g_hIconMouse) { DestroyIcon(ogl::g_hIconMouse); ogl::g_hIconMouse = NULL; }
            if (ogl::g_hIconScale) { DestroyIcon(ogl::g_hIconScale); ogl::g_hIconScale = NULL; }
            if (ogl::g_hIconColor) { DestroyIcon(ogl::g_hIconColor); ogl::g_hIconColor = NULL; }
            ogl::g_pickingCoords = false; // Disable picking coords flag
            ogl::g_hOglTabDlg = NULL; // Invalidate global handle copy
            LocalKbdHookOgl::Disable();
            break;
        }
    }
    return FALSE;
}


// Preset settings page procedures - handles interactions
INT_PTR CALLBACK PresetsDlgProc(HWND hwndDlg, UINT msg, WPARAM wp, LPARAM lp) {
    static int targSecId = SEC_PRESET1 + prst::g_editingPreset;
    static std::wstring targPresetName = L"Preset 1";
    static HWND hComboPreset, hEditName, hEditCmd, hCheckStartup, hEditDelay, hComboPriv, hCheckConsole; // Define handles for UI triggers
    // Static list for UI triggers attributes based on structure above
    static SettingField PrstCfgTrigs[] = {
        { &hComboPreset, CLASS_COMBOBOX, L"Target:", IDC_SETTING_PRESET_COMBO, CBS_DROPDOWNLIST | WS_VSCROLL }, 
        { &hEditName, CLASS_EDIT, L"Preset name:", IDC_SETTING_NAME, ES_AUTOHSCROLL | WS_BORDER, KEY_PRESET_NAME, 63 }, 
        { &hEditCmd, CLASS_EDIT, L"Command:", IDC_SETTING_COMMANDS, ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER, KEY_PRESET_CMD, 511 }, 
        { &hCheckStartup, CLASS_BUTTON, L"Launch on app start", IDC_SETTING_STARTUP, BS_AUTOCHECKBOX, KEY_PRESET_STARTUP }, 
        { &hEditDelay, CLASS_EDIT, L"With delay (ms):", IDC_SETTING_DELAY, ES_NUMBER | WS_BORDER, KEY_PRESET_DELAY, 5 }, 
        { &hComboPriv, CLASS_COMBOBOX, L"Privileges:", IDC_SETTING_PRIVILEGES, CBS_DROPDOWNLIST | WS_VSCROLL, KEY_PRESET_PRIVILEGES }, 
        { &hCheckConsole, CLASS_BUTTON, L"Show console window", IDC_SETTING_CONSOLE, BS_AUTOCHECKBOX, KEY_PRESET_SHOWCONSOLE }, 
        { nullptr, nullptr, nullptr, 0, 0, 0, 0 }
    };
    struct { // Operator for refreshing all values when changing target preset
        __declspec(noinline) void operator()(const LPCWSTR prstName, const int targSecId, const HWND hwndTab) {
            // Loop that restarts for each entry in cfgTrigs
            for (SettingField& trig : PrstCfgTrigs) {
                if (!trig.hCtrlPtr) break; if (!trig.cfgKeyId) continue;
                trig.isDirty = false;
                if (trig.id < 4230) { // Checkboxes
                    SendMessageW(*(trig.hCtrlPtr), BM_SETCHECK, *static_cast<const bool*>(g_cfg.get(targSecId, trig.cfgKeyId)) ? BST_CHECKED : BST_UNCHECKED, 0); // Set state
                } else if (trig.id < 4300) { // Fields
                    if (trig.styles & ES_NUMBER) { // If it's a number field
                        const int intVal = *static_cast<const int*>(g_cfg.get(targSecId, trig.cfgKeyId)); // Get runtime setting
                        SetDlgItemInt(hwndTab, trig.id, intVal, FALSE);
                        continue;
                    }
                    const LPCWSTR text = (*static_cast<const std::wstring*>(g_cfg.get(targSecId, trig.cfgKeyId))).c_str();
                    SetWindowTextW(*(trig.hCtrlPtr), text); // Set text
                    setTooltip(*(trig.hCtrlPtr), text); // Set tooltip
                } else if (trig.id == IDC_SETTING_PRIVILEGES) { // Privileges setting combo box
                    SendMessageW(*(trig.hCtrlPtr), CB_SETCURSEL, (*static_cast<const int*>(g_cfg.get(targSecId, trig.cfgKeyId)) == 1) ? 1 : 0, 0); // Set current selection
                }
            }
            SendMessageW(hEditName, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(prstName)); // Set cue banner for preset name editor
        }
    } refreshUI;
    switch (msg) {
        case WM_INITDIALOG: { // Propsheet tab initialization
            { // Tabs font enhancer (create modded tabs font)
                const HWND hParent = GetParent(hwndDlg);
                const HWND hTabCtrl = PropSheet_GetTabControl(hParent); // Obtain handle to tab control
                if (hTabCtrl) {
                    const HFONT hCurrentFont = reinterpret_cast<HFONT>(SendMessageW(hTabCtrl, WM_GETFONT, 0, 0)); // Get current font
                    if (hCurrentFont) {
                        LOGFONTW lf; GetObjectW(hCurrentFont, sizeof(LOGFONTW), &lf); // Get font properties
                        // Edit size
                        lf.lfHeight = -Scale(12);
                        lf.lfWidth = 0;
                        g_hPshTabsFont = CreateFontIndirectW(&lf); // Make new font from edited properties
                        SendMessageW(hTabCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_hPshTabsFont), TRUE); // Apply new font
                    }
                }
                PostMessageW(hParent, WM_SHOW_STARTUP, 0, 0);
            }
            createGuiFont(g_hPshFont, Scale(14)); // Create GUI font for the propsheet
            // Draw the UI triggers
            RECT rcPage; GetClientRect(hwndDlg, &rcPage); // Obtain page's internal dimensions
            const int padding = Scale(8);
            const int space = Scale(2);
            const int labelHeight = Scale(20);
            const int comboSpacing = Scale(24);
            const int marginX = rcPage.left + padding;
            const int ctrlWidth = rcPage.right - rcPage.left - (padding * 2);
            int currentY = rcPage.top + padding;
            // Loop that restarts for each entry in cfgTrigs
            for (const SettingField& trig : PrstCfgTrigs) {
                if (!trig.hCtrlPtr) break;
                const bool isButton = (trig.className[0] == L'B');
                const bool isCombo = (trig.className[0] == L'C');
                if (!isButton) {
                    CreateWindowW( // Add label
                        CLASS_STATIC, trig.text, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 
                        marginX, currentY, ctrlWidth, labelHeight, hwndDlg, 
                        NULL, g_hInstRelay, 0
                    );
                    currentY += labelHeight; // Update height point for trigger after label
                }
                // Set height depending on the trigger's kind
                int ctrlHeight = 24;
                if (trig.id == IDC_SETTING_COMMANDS) {
                    ctrlHeight = 80;
                } else if (isCombo) {
                    ctrlHeight = 150;
                }
                ctrlHeight = Scale(ctrlHeight);
                // Create UI trigger, and set button text if applicable
                const LPCWSTR ctrlText = isButton ? trig.text : L"";
                *(trig.hCtrlPtr) = CreateWindowW(
                    trig.className, ctrlText, WS_CHILD | WS_VISIBLE | trig.styles, 
                    marginX, currentY, ctrlWidth, ctrlHeight, hwndDlg, 
                    reinterpret_cast<HMENU>(trig.id), g_hInstRelay, 0
                );
                currentY += (isCombo ? comboSpacing : ctrlHeight); // Update height point for next element
                if (trig.id != IDC_SETTING_STARTUP) currentY += space; // Add space before next element
                if (trig.className[0] == L'E') SendMessageW(*(trig.hCtrlPtr), EM_LIMITTEXT, trig.maxLength, 0); // Set input text limit if applicable
            }
            reloadTextFont(hwndDlg, g_hPshFont); // Set font
            // Make propsheet show itself
            // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 3
            for (int i = 0; i < PRESET_COUNT; ++i) {
                targPresetName[7] = L'1' + i; // Update target preset name
                SendMessageW(hComboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(targPresetName.c_str())); // Add it as an entry to the preset selector
            }
            targPresetName[7] = L'1' + prst::g_editingPreset; // Update target preset name to match current setting
            SendMessageW(hComboPreset, CB_SETCURSEL, prst::g_editingPreset, 0); // Update preset selector accordingly
            // Add entries for privileges selector
            SendMessageW(hComboPriv, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Same as app"));
            SendMessageW(hComboPriv, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Administrator"));
            refreshUI(targPresetName.c_str(), targSecId, hwndDlg); // Set UI triggers states to correspond with runtime settings
            return TRUE;
        }
        case WM_COMMAND: { // User interaction with the window
            const WORD wmId = LOWORD(wp), notifCode = HIWORD(wp);
            if (notifCode == BN_CLICKED || notifCode == EN_CHANGE || (notifCode == CBN_SELCHANGE && wmId < 4400)) { // If any UI trigger was adjusted (except preset selector)
                // Loop that restarts for each entry in cfgTrigs
                for (SettingField& trig : PrstCfgTrigs) { if (wmId == trig.id) { trig.isDirty = true; break; }} // Update "isDirty" flag when ID matches
                PropSheet_Changed(GetParent(hwndDlg), hwndDlg); // Enable apply button
                break;
            }
            if (wmId == IDC_SETTING_DELAY && notifCode == EN_KILLFOCUS) { // Delay field was edited and lost focus
                constexpr uint32_t max_val = 30000;
                const uint32_t value = GetDlgItemInt(hwndDlg, wmId, nullptr, FALSE);
                if (value > max_val) { SetDlgItemInt(hwndDlg, wmId, max_val, FALSE); }
                return TRUE;
            }
            if (wmId == IDC_SETTING_PRESET_COMBO && notifCode == CBN_SELCHANGE) { // Preset selector was adjusted
                const LRESULT sel = SendMessageW(reinterpret_cast<HWND>(lp), CB_GETCURSEL, 0, 0); // Get new selection
                if (sel != CB_ERR) { // Verify selection validity
                    prst::g_editingPreset = static_cast<int>(sel); // Update preset selector state var
                    targSecId = SEC_PRESET1 + prst::g_editingPreset;
                    targPresetName[7] = L'1' + prst::g_editingPreset; // Update target preset name
                    refreshUI(targPresetName.c_str(), targSecId, hwndDlg); // Set UI triggers states to correspond with runtime settings
                    PropSheet_UnChanged(GetParent(hwndDlg), hwndDlg); // Disable apply button
                    return TRUE;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: { // Left click
            SetFocus(hwndDlg);
            return TRUE;
        }
        case WM_NOTIFY: { // Notification of an event with the window
            const LPNMHDR pNmhdr = reinterpret_cast<LPNMHDR>(lp);
            if (pNmhdr->code == PSN_APPLY) {
                // Loop that restarts for each entry in cfgTrigs
                for (SettingField& trig : PrstCfgTrigs) {
                    if (!trig.hCtrlPtr) break; if (!trig.cfgKeyId || !trig.isDirty) continue;
                    if (trig.id < 4230) { // If it's a checkbox
                        // Update related runtime setting according to state
                        const bool checked = (SendMessageW(*(trig.hCtrlPtr), BM_GETCHECK, 0, 0) == BST_CHECKED);
                        cfgChangeAndApply(targSecId, trig.cfgKeyId, &checked, TYPE_BOOL);
                    } else if (trig.id < 4300) { // If it's a field
                        if (trig.styles & ES_NUMBER) { // If it's a number field
                            const int intVal = GetDlgItemInt(hwndDlg, trig.id, nullptr, FALSE);
                            cfgChangeAndApply(targSecId, trig.cfgKeyId, &intVal, TYPE_INT);
                            continue;
                        }
                        // Get text inside and update relevant runtime setting
                        wchar_t readBuf[512];
                        const uint32_t txtLength = static_cast<uint32_t>(GetWindowTextW(*(trig.hCtrlPtr), readBuf, 512));
                        cfgChangeAndApply(targSecId, trig.cfgKeyId, &readBuf[0], TYPE_WSTR_VIEW, txtLength);
                    } else if (trig.id == IDC_SETTING_PRIVILEGES) { // If it's the privileges setting combo box
                        // Update related runtime setting according to current selection
                        const LRESULT sel = SendMessageW(*(trig.hCtrlPtr), CB_GETCURSEL, 0, 0);
                        if (sel != CB_ERR) {
                            const int newVal = (sel == 1) ? 1 : 0;
                            cfgChangeAndApply(targSecId, trig.cfgKeyId, &newVal, TYPE_INT);
                        }
                    } else continue;
                    trig.isDirty = false;
                }
                SetWindowLongPtrW(hwndDlg, DWLP_MSGRESULT, PSNRET_NOERROR); // Tell windows everything went right for ok button to close the window after applying
                presetBtnsUpdate(); // Update preset buttons text
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}


// Function for initializing settings propsheet window
static void ShowCfgPropSheet(const HWND hParentWnd) {
    static WNDPROC pfnOldPshProc;
    struct Psh {
        // Propsheet window temporary subclass
        static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
            if (msg == WM_SHOW_STARTUP) { // The window was notified it is ready to show itself
                // Summon the window
                ShowWindow(hWnd, SW_SHOWNORMAL);
                SetForegroundWindow(hWnd);
                SetActiveWindow(hWnd);
                if (DwmSetWindowAttribute) { // Uncloak the window
                    BOOL cloak = FALSE;
                    DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                }
                SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pfnOldPshProc)); // Set original callback
                return 0;
            }
            return CallWindowProcW(pfnOldPshProc, hWnd, msg, wp, lp);
        }
        // Propsheet window creation procedures
        static int CALLBACK CreationProc(HWND hWnd, UINT msg, LPARAM lp) {
            if (msg == PSCB_PRECREATE && lp) { // Pre-creation stage
                DWORD *const pTemplate = reinterpret_cast<DWORD*>(lp);
                const bool isEx = ((pTemplate[0] >> 16) == 0xFFFF);
                pTemplate[isEx ? 3 : 0] &= ~WS_VISIBLE; // Remove WS_VISIBLE flag
            } else if (msg == PSCB_INITIALIZED && hWnd) { // The window is initialized (+ verify handle exists)
                if (DwmSetWindowAttribute) { // Cloak the window
                    BOOL cloak = TRUE;
                    DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                }
                // Set temporary subclass and keep pointer to original callback
                pfnOldPshProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassProc)));
            }
            return 0;
        }
    };
    PROPSHEETPAGEW psp[2]{}; // Prepare 2 propsheet pages
    // Setup page 1:
    psp[0].dwSize = sizeof(PROPSHEETPAGEW); // Set binary size
    psp[0].dwFlags = PSP_USETITLE; // Use custom title from relevant variable
    psp[0].hInstance = g_hInstRelay; // Assign parent process instance
    psp[0].pszTemplate = MAKEINTRESOURCEW(IDD_PAGE_PRESETS); // Assign ID
    psp[0].pfnDlgProc = PresetsDlgProc; // Assign callback function
    psp[0].pszTitle = L"Presets"; // Assign page title
    // Setup page 2:
    psp[1].dwSize = sizeof(PROPSHEETPAGEW); // Set binary size
    psp[1].dwFlags = PSP_USETITLE; // Use custom title from relevant variable
    psp[1].hInstance = psp[0].hInstance; // Set parent process instance
    psp[1].pszTemplate = MAKEINTRESOURCEW(IDD_PAGE_OGLSPOOF); // Assign ID
    psp[1].pfnDlgProc = OGLSpoofDlgProc; // Assign callback function
    psp[1].pszTitle = L"OpenGL"; // Assign page title
    PROPSHEETHEADERW psh{}; // Prepare propsheet header
    psh.dwSize = sizeof(PROPSHEETHEADERW); // Set binary size
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_USECALLBACK; // Set flags
    psh.hwndParent = hParentWnd; // Assign parent window
    psh.hInstance = psp[0].hInstance; // Assign parent process instance
    psh.pfnCallback = Psh::CreationProc; // Assign callback function
    psh.pszCaption = L"TTE - Settings"; // Assign window title
    psh.nPages = 2; // Set pages count
    psh.ppsp = psp; // Set pages reference
    PropertySheetW(&psh); // Summon the window (blocking as long as propsheet window stays open)
    // Destroy any custom fonts
    if (g_hPshTabsFont) { DeleteObject(g_hPshTabsFont); g_hPshTabsFont = NULL; }
    if (g_hPshFont && g_hPshFont != GetStockObject(DEFAULT_GUI_FONT)) { DeleteObject(g_hPshFont); g_hPshFont = NULL; }
}


// Dummy tray menu owner window procedures - handles tray icon interactions
LRESULT CALLBACK TrayWndProc(HWND hTrayWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TRAYICON: { // Interaction with the tray icon
            if (lp == WM_LBUTTONUP) { // left click
                PostMessageW(g_hMainWnd, WM_COMMAND, ID_TRAY_SHOW, 0); // Post message to show the main window
            } else if (lp == WM_RBUTTONUP) { // Right click
                POINT cur; GetCursorPos(&cur); // Get cursor position
                const HMENU hMenu = CreatePopupMenu(); // Create menu then add elements
                // Tray icon title, greyed out
                AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, g_trayName.c_str());
                // Separator line
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                // Add corresponding preset button
                // Loop with variable i as 0 by default, restarting as long as i is lower than 3, with i getting incremented each turn
                for (int i = 0; i < PRESET_COUNT; ++i) { AppendMenuW(hMenu, MF_STRING, ID_Preset1 + i, prst::g_presetNames[i].c_str()); }
                // Separator line
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                // Main window button (sends ID_TRAY_SHOW)
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"Main window");
                // Exit process button (sends ID_TRAY_EXIT)
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit process");
                // Focus dummy tray menu owner window so clicking an option properly kills the tray menu
                SetForegroundWindow(hTrayWnd);
                // Wait for some event to kill the tray menu before continuing
                TrackPopupMenuEx(hMenu, TPM_LEFTALIGN, cur.x, cur.y, hTrayWnd, nullptr);
                // Post empty message to flush queue so clicking away properly exits the tray menu
                PostMessageW(hTrayWnd, WM_NULL, 0, 0);
                // Make the menu destroy itself
                DestroyMenu(hMenu);
            }
            return 0;
        }
        case WM_COMMAND: { // User interaction with the tray menu
            const WORD wmId = LOWORD(wp);
            if (wmId < 320) { // Preset button
                const HWND hPrstBtn = GetDlgItem(g_hMainWnd, wmId); // Get equivalent button handle from the main window
                if (hPrstBtn && IsWindowEnabled(hPrstBtn)) { SendMessageW(hPrstBtn, BM_CLICK, 0, 0); } // Simulate click
            } else {
                PostMessageW(g_hMainWnd, msg, wp, lp); // Forward message to the main window
            }
            return 0;
        }
    }
    return DefWindowProcW(hTrayWnd, msg, wp, lp);
}


// Main window procedures - handles interactions
LRESULT CALLBACK mainWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    struct { // Operator for redrawing all UI triggers of the main window
        __declspec(noinline) void operator()(void) {
            if (!g_hMainWnd) return;
            RECT rc; GetClientRect(g_hMainWnd, &rc); // Obtain main window's internal dimensions
            // Define spatial organization points
            const int WndWidth = rc.right - rc.left;
            const int halfWndWidth = WndWidth / 2;
            const int BiggestChkboxWidth = Scale(200);
            const int chkboxHeight = Scale(25);
            const int btnWidth = Scale(180);
            const int presetHeight = Scale(64);
            const int space = Scale(12);
            int leftStartX = (halfWndWidth - BiggestChkboxWidth) / 2;
            int rightStartX = halfWndWidth + ((halfWndWidth - btnWidth) / 2);
            int leftY = Scale(20), rightY = leftY;
            // Draw UI triggers
            HWND hGear = NULL;
            // Loop that restarts for each entry in UI triggers as trig
            for (int i = 0; TrigsReg[i].hTrigPtr; ++i) {
                const trig::info& trig = TrigsReg[i];
                const HWND ctrlWnd = *(trig.hTrigPtr);
                if (!ctrlWnd) continue;
                if (trig.id < 300) { // If type is checkbox
                    MoveWindow(ctrlWnd, leftStartX, leftY, BiggestChkboxWidth, chkboxHeight, TRUE); // Set checkbox position and size
                    leftY += chkboxHeight + space; // Increment height offset for next UI trigger
                } else if (trig.id < 320) { // Else if it's a preset button
                    MoveWindow(ctrlWnd, rightStartX, rightY, btnWidth, presetHeight, TRUE); // Set button position and size
                    rightY += presetHeight + space; // Increment height offset for next UI trigger
                } else if (trig.id == ID_GearBtn) { // Else if it's the gear (settings) button
                    hGear = ctrlWnd; // Acquire its handle to reuse
                    continue;
                }
            }
            const int exitH = Scale(40), exitW = Scale(128);
            rightY -= (space + exitH);
            if (trig::g_hBtnExit) { MoveWindow(trig::g_hBtnExit, leftStartX, rightY, exitW, exitH, TRUE); } // Set exit button position and size
            leftStartX += exitW + space;
            if (hGear) {
                MoveWindow(hGear, leftStartX, rightY, exitH, exitH, TRUE);
                const int targIconSize = Scale(16);
                static int cachedIconSize = 0;
                if (targIconSize != cachedIconSize) { // If ideal gear icon size changed, redraw it
                    if (g_hGearIcon) DestroyIcon(g_hGearIcon);
                    g_hGearIcon = static_cast<HICON>(LoadImageW(
                        g_hInstRelay, 
                        MAKEINTRESOURCEW(IDI_GEAR_ICON), 
                        IMAGE_ICON, 
                        targIconSize, targIconSize, 
                        LR_DEFAULTCOLOR
                    ));
                    if (g_hGearIcon) {
                        SendMessageW(hGear, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(g_hGearIcon));
                        cachedIconSize = targIconSize;
                    }
                }
            }
        }
    } UpdateUIResources;
    switch (msg) {
        case WM_CREATE: { // Window initialization
            // Loop that restarts for each entry in UI triggers as trig
            for (int i = 0; TrigsReg[i].hTrigPtr; ++i) {
                const trig::info& trig = TrigsReg[i];
                if (!trig.hTrigPtr) continue; // Verify pointer validity
                // Create a trigger with the specified text and type, storing its handle in the address pointed by hCtrlPtr
                *(trig.hTrigPtr) = CreateWindowW(
                    CLASS_BUTTON, trig.text.data(), WS_VISIBLE | WS_CHILD | trig.styles | trig.trigType,
                    0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(trig.id), g_hInstRelay, 0
                );
                if ((trig.trigType & BS_TYPEMASK) == BS_AUTOCHECKBOX) { // If type is checkbox
                    // Set check state of hTrig based on value of associated setting in config
                    SendMessageW(*(trig.hTrigPtr), BM_SETCHECK, *static_cast<const bool*>(g_cfg.get(SEC_GENERAL, trig.cfgKeyId)) ? 
                        BST_CHECKED : BST_UNCHECKED, 0);
                }
            }
            // Create global tooltips window
            g_hTooltipWnd = CreateWindowExW(
                WS_EX_TOPMOST, TOOLTIPS_CLASSW, L"", WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hWnd, NULL, 
                g_hInstRelay, nullptr
            );
            presetBtnsUpdate(); // Update preset buttons text
            return 0;
        }
        case WM_COMMAND: { // Interaction with the window
            const WORD wmId = LOWORD(wp);
            switch (wmId) {
                case ID_TRAY_EXIT: { // Need to quit
                    DestroyWindow(hWnd);
                    return 0;
                }
                case ID_TRAY_SHOW: { // Need to reveal the window
                    const HWND hTargetWnd = GetLastActivePopup(hWnd); // Get the topmost window within the instance
                    ShowWindow(hTargetWnd, SW_RESTORE);
                    SetForegroundWindow(hTargetWnd); // Give the window focus
                    SetActiveWindow(hTargetWnd); // Ensure instance-wide focus
                    return 0;
                }
            }
            if (HIWORD(wp) == BN_CLICKED) { // A button was clicked
                if (wmId == ID_GearBtn) { // Gear button
                    ShowCfgPropSheet(hWnd); // Initialize settings tabs window
                    return 0;
                }
                if (wmId < 300) { // Checkbox
                    int key_id = 0;
                    for (int i = CHKBOX1_TRIG_IDX; i < CHKBOX1_TRIG_IDX + CHKBOX_COUNT; ++i) {
                        if (TrigsReg[i].id == wmId) key_id = TrigsReg[i].cfgKeyId;
                    }
                    const bool isChecked = (SendMessageW(reinterpret_cast<HWND>(lp), BM_GETCHECK, 0, 0) == BST_CHECKED);
                    cfgChangeAndApply(SEC_GENERAL, key_id, &isChecked, TYPE_BOOL); // update corresponding runtime setting
                    return 0;
                }
                if (wmId < 320) { // Preset button
                    // Trigger asynchronous 3 second wait before next preset launch is possible
                    _beginthreadex(nullptr, 0, &ButtonRestrictorThread, reinterpret_cast<HWND>(lp), 0, nullptr);
                    const int presetInd = wmId - ID_Preset1; // Get preset index
                    presetLaunch(SEC_PRESET1 + presetInd); // Launch the preset
                    return 0;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: { // Left click
            SetFocus(hWnd);
            return 0;
        }
        case WM_SIZE : { // The window was resized
            if (wp != SIZE_MINIMIZED) UpdateUIResources(); // Ensure it wasn't just minimized
            return 0;
        }
        case WM_DPICHANGED: { // Windows scale setting was changed while the app is running
            g_uiScale = LOWORD(wp); // Get DPI from Windows and update var
            const LPRECT lprcNewWindow = reinterpret_cast<LPRECT>(lp); // Get suggested new window rect from long param
            SetWindowPos( // Tell Windows to resize and reposition the window
                hWnd, NULL, lprcNewWindow->left, lprcNewWindow->top, 
                lprcNewWindow->right - lprcNewWindow->left, lprcNewWindow->bottom - lprcNewWindow->top, 
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
            );
            createGuiFont(g_uiFont, Scale(14)); reloadTextFont();
            UpdateUIResources();
            return 0;
        }
        case WM_GETMINMAXINFO: { // Windows is asking for minimum and maximum size for the window
            LPMINMAXINFO minmax = reinterpret_cast<LPMINMAXINFO>(lp);
            // Set minimum tracking size for the window (smallest it can be resized to) accounting for Windows scale
            minmax->ptMinTrackSize.x = Scale(512);
            minmax->ptMinTrackSize.y = Scale(300);
            return 0;
        }
        case WM_CLOSE: { // Window needs to be closed
            if (SendMessageW(trig::g_hCbMinOnClose, BM_GETCHECK, 0, 0) == BST_CHECKED) { // If "Minimize on close" is on
                ShowWindow(hWnd, SW_HIDE); // Hide the window (and also remove it from taskbar), leaving it only in system tray
            } else { DestroyWindow(hWnd); }
            return 0;
        }
        case WM_DESTROY: { // Destruction routine (quitting app)
            Shell_NotifyIconW(NIM_DELETE, &nid); // Remove entry in system tray
            // Destroy any icons in memory
            if (nid.hIcon) DestroyIcon(nid.hIcon);
            if (g_hGearIcon) DestroyIcon(g_hGearIcon);
            // Destroy any registered custom fonts
            if (g_uiFont && g_uiFont != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(g_uiFont);
            if (g_hPshTabsFont) DeleteObject(g_hPshTabsFont);
            // Close any handles to mutex or shared mem
            if (g_hSingleTonMutex) CloseHandle(g_hSingleTonMutex);
            if (g_hMapFileGlobal) CloseHandle(g_hMapFileGlobal);
            // Free dwmapi library if present
            if (g_hDwmapi) FreeLibrary(g_hDwmapi);
            timeEndPeriod(1); // Turn off 1ms windows scheduling precision
            PostQuitMessage(0); // Exit with code 0 (success)
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}


// Application entry point
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    {
        POINT cur; GetCursorPos(&cur); // Get cursor position
        { // Prepare environment for tooltips (global tooltips class)
            INITCOMMONCONTROLSEX iccex{};
            iccex.dwSize = sizeof(INITCOMMONCONTROLSEX); // Set binary size
            iccex.dwICC = ICC_TAB_CLASSES; // Make Windows load tooltips class
            InitCommonControlsEx(&iccex); // Register the class
            // Initialise runtime variables
            getExecName();
            getConfigPath();
            initCfg();
            if (g_vistaOrGreater = IsWindowsVistaOrGreater()) {
                g_hDwmapi = LoadLibraryW(L"dwmapi.dll");
                if (g_hDwmapi) DwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttribute_t>(GetProcAddress(g_hDwmapi, "DwmSetWindowAttribute"));
            }
            g_hInstRelay = hInst; // Set relay to program instance
        }
        struct { // Prepare shared mem region name
            __forceinline std::wstring operator()(void) const {
                std::wstring memRegion;
                memRegion.reserve(17 + g_execName.size());
                memRegion.append(L"Local\\").append(g_execName).append(L"_SharedMem0");
                return memRegion;
            }
        } getMemRegionName; const std::wstring memRegionName = getMemRegionName();
        if (isAnotherInstanceRunning(memRegionName.c_str())) { // If an instance from the same exec is already running
            // Close any handles to mutex or shared mem
            if (g_hSingleTonMutex) CloseHandle(g_hSingleTonMutex);
            if (g_hMapFileGlobal) CloseHandle(g_hMapFileGlobal);
            return EXIT_SUCCESS;
        }
        // Detect cleartype support for UI
        BOOL isClearType = TRUE;
        SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &isClearType, 0);
        g_fontQuality = (isClearType) ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY;
        // Prepare names for tray menu and error message boxes
        g_trayName.reserve(6 + g_execName.size());
        g_trayName.append(L"TTE (").append(g_execName).append(L")");
        g_errWndName.reserve(14 + g_execName.size());
        g_errWndName.append(L"TTE (").append(g_execName).append(L") - Error");
        // Build up the main window class:
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW); // Set binary size
        wc.style = CS_HREDRAW | CS_VREDRAW; // Set flags
        wc.lpfnWndProc = mainWndProc; // Assign callback function
        wc.hInstance = g_hInstRelay; // Assign parent process instance
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON)); // Set icon
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW); // Set default mouse cursor
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(COLOR_BTNFACE + 1)); // Set background color
        wc.lpszClassName = L"TTE_main_Class"; // Set unique class name
        RegisterClassExW(&wc); // Register the class
        SetLastError(ERROR_SUCCESS); // Reset last error
        struct { // Prepare main window name
            __forceinline std::wstring operator()(void) const {
                const std::wstring defaultName = DEFAULT_APP_NAME;
                if (g_execName == defaultName) return defaultName;
                std::wstring wndName;
                wndName.reserve(15 + g_execName.size());
                wndName.append(defaultName).append(L" (").append(g_execName).append(L")");
                return wndName;
            }
        } getWndName;
        if (!(g_hMainWnd = CreateWindowExW( // Create main window
            0, wc.lpszClassName, getWndName().c_str(), WS_OVERLAPPEDWINDOW, 
            0, 0, 0, 0, NULL, NULL, g_hInstRelay, nullptr
        ))) {
            char errBuf[ERRBUF_SIZE];
            std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
            g_errMsg.append(fatal_error).append(L"could not initialize the main window, failed with code: ").append(errBuf, r.ptr);
            errBox(g_errMsg.c_str(), true, NULL);
        }
        if (DwmSetWindowAttribute) { // Cloak the window
            BOOL cloak = TRUE;
            DwmSetWindowAttribute(g_hMainWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        }
        isAnotherInstanceRunning(memRegionName.c_str(), true); // Create shared mem region, so if another instance is launched it can call this one
        OglSpooferInit(); // Start OpenGL window logic if enabled
        // Obtain, store and apply system DPI
        if (const HMODULE hUser32 = GetModuleHandleW(L"user32.dll")) {
            typedef UINT(WINAPI* GetDpiForWindow_t)(HWND);
            if (const GetDpiForWindow_t GetDpiForWindow = reinterpret_cast<GetDpiForWindow_t>(GetProcAddress(hUser32, "GetDpiForWindow"))) {
                g_uiScale = GetDpiForWindow(g_hMainWnd);
            } else if (const HDC hdc = GetDC(g_hMainWnd)) {
                g_uiScale = GetDeviceCaps(hdc, LOGPIXELSX);
                ReleaseDC(g_hMainWnd, hdc);
            }
        }
        const int width = Scale(512), height = Scale(300); // Default resolution
        const int wndX = cur.x - MulDiv(width, 950, 1000), wndY = cur.y - MulDiv(height, 900, 1000); // Window spawn point
        const RECT rcWnd = { wndX, wndY, wndX + width, wndY + height };
        SendMessageW(g_hMainWnd, WM_DPICHANGED, MAKEWPARAM(g_uiScale, g_uiScale), reinterpret_cast<LPARAM>(&rcWnd));

        // Auto-launch presets when relevant setting requires it
        // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 3
        for (int i = 0; i < PRESET_COUNT; ++i) {
            const int targSecId = SEC_PRESET1 + i; // Calculate target section ID
            if (*static_cast<const bool*>(g_cfg.get(targSecId, KEY_PRESET_STARTUP))) presetLaunch(targSecId, true); // if setting is enabled, do auto-launch
        }
        WNDCLASSW wcTray{};
        wcTray.lpfnWndProc = TrayWndProc;
        wcTray.hInstance = g_hInstRelay;
        wcTray.lpszClassName = Tray_owner_Class;
        RegisterClassW(&wcTray);
        if (const HWND hTrayWnd = CreateWindowExW( // Create a dummy tray menu owner window
            0, wcTray.lpszClassName, L"", 0, 
            0, 0, 0, 0, NULL, NULL, 
            g_hInstRelay, nullptr
        )) {
            // Build up the tray icon class:
            nid.cbSize = sizeof(nid); // Set binary size
            nid.hWnd = hTrayWnd; // Assign parent window
            nid.uID = 1; // Assign unique tray menu ID (app-wide only)
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; // Assign flags (has icon, supports callback messages, has tooltip)
            nid.uCallbackMessage = WM_TRAYICON; // Assign custom callback message it will use
            nid.hIcon = wc.hIcon; // Set icon (same as main window)
            lstrcpyW(nid.szTip, g_trayName.c_str()); // Set tooltip
            Shell_NotifyIconW(NIM_ADD, &nid); // Make Windows add this tray icon
        } else {
            char errBuf[ERRBUF_SIZE];
            std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
            g_errMsg.append(msg_error)
                    .append(L"could not initialize the dummy tray menu owner window, failed with code: ")
                    .append(errBuf, r.ptr);
            errBox(g_errMsg.c_str());
        }
        // Summon main window (unless "Launch minimized" is enabled)
        if (!*(static_cast<const bool*>(g_cfg.get(SEC_GENERAL, KEY_LAUNCHMIN)))) {
            ShowWindow(g_hMainWnd, nShow);
            SetForegroundWindow(g_hMainWnd);
            SetActiveWindow(g_hMainWnd);
        }
        if (DwmSetWindowAttribute) { // Uncloak the window
            BOOL cloak = FALSE;
            DwmSetWindowAttribute(g_hMainWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        }
        g_errMsg.reserve(512);
    }
    // Message processing loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg); // Translate any keyboard codes
        DispatchMessageW(&msg); // Give message to Windows to process
    }
    // Return wParam in message. This will take the exit code passed through command PostQuitMessage
    return static_cast<int>(msg.wParam);
}
