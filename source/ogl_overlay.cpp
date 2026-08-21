#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "global.h"
#include "ogl_overlay.h"
#include <process.h>
#include <timeapi.h>
#include <windows.h>
#include <gl/GL.h>
#include <unordered_map>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <string>
#include <memory>

#pragma comment(lib, "winmm.lib")


std::atomic<bool> g_OglSomethingChanged{ true };
namespace {
    static constexpr wchar_t OGL_SPOOFER_CLASS_NAME[] = L"TTE_OGLSpoof_Class"; // OpenGL Spoofer class
    static constexpr wchar_t ogl_err_base[] = L"Error (OpenGL): ";
    const std::unordered_map<int, int> OglSettingsMap = {
        { KEY_OGLXCOORD , IDGL_XCOORD }, 
        { KEY_OGLYCOORD, IDGL_YCOORD }, 
        { KEY_OGLWIDTH, IDGL_WIDTH }, 
        { KEY_OGLHEIGHT, IDGL_HEIGHT }, 
        { KEY_OGLCOLOR, IDGL_COLOR }, 
        { KEY_OGLFRAMETIME, IDGL_FRAMETIME }, 
        { KEY_OGLSHOW, IDGL_SHOW }, 
        { KEY_OGLTOP, IDGL_TOPMOST }, 
        { KEY_OGLPRECIS, IDGL_HIGHPRECIS }
    };
    // Define arbitrary structure for messages to OpenGL thread, in order to allow on-the-fly settings adjustment
    struct MailboxCell {
        DWORD rawVal;
        std::atomic<bool> isDirty{ false };
    };
    std::atomic<HANDLE> hOglThread{NULL};
    MailboxCell g_OglMailbox[OglKeyInd::OGLIND_MAX];
    std::atomic<int> g_OglThreadState{ IDGL_STATE_STOPPED };
    std::atomic<bool> g_OglStopThread;
}


// Function running in separate thread, rendering an overlay with OpenGL so the app can be spotted by graphics drivers or related
unsigned __stdcall OpenGLSpooferRenderThread(void* pArg) {
    std::unique_ptr<DWORD[]> pParams(static_cast<DWORD*>(pArg));
    // Tables for local copies from OpenGL mailbox
    DWORD localValues[OglKeyInd::OGLIND_MAX]; std::memcpy(localValues, pParams.get(), sizeof(localValues));
    bool localDirty[OglKeyInd::OGLIND_MAX];

    const HMODULE hOglDriver = LoadLibraryW(L"opengl32.dll");
    if (!hOglDriver) goto just_get_out;
    typedef HGLRC(WINAPI* wglCreateContext_t)(HDC);
    typedef BOOL(WINAPI* wglMakeCurrent_t)(HDC, HGLRC);
    typedef BOOL(WINAPI* wglDeleteContext_t)(HGLRC);
    typedef void(WINAPI* glClearColor_t)(GLclampf, GLclampf, GLclampf, GLclampf);
    typedef void(WINAPI* glClear_t)(GLbitfield);
    const wglCreateContext_t wglCreateContext = reinterpret_cast<wglCreateContext_t>(GetProcAddress(hOglDriver, "wglCreateContext"));
    const wglMakeCurrent_t wglMakeCurrent = reinterpret_cast<wglMakeCurrent_t>(GetProcAddress(hOglDriver, "wglMakeCurrent"));
    const wglDeleteContext_t wglDeleteContext = reinterpret_cast<wglDeleteContext_t>(GetProcAddress(hOglDriver, "wglDeleteContext"));
    const glClearColor_t glClearColor = reinterpret_cast<glClearColor_t>(GetProcAddress(hOglDriver, "glClearColor"));
    const glClear_t glClear = reinterpret_cast<glClear_t>(GetProcAddress(hOglDriver, "glClear"));
    if (!wglCreateContext || !wglMakeCurrent || !wglDeleteContext || !glClearColor || !glClear) goto just_get_out;

    pParams.reset();
    int local_stateVar;
    g_OglThreadState.store(local_stateVar = IDGL_STATE_INIT, std::memory_order_release); // Set OpenGL thread state to "initializing"
    HWND hOglWnd = NULL;
    // Create the window (if it fails, get out)
    if(!(hOglWnd = CreateWindowExW(
        WS_EX_LAYERED, OGL_SPOOFER_CLASS_NAME, L"TTE_Overlay", WS_POPUP, 
        0, 0, 400, 300, NULL, NULL, g_hInstRelay, 0
    ))) {
        std::wstring wnd_error;
        wnd_error.reserve(256);
        wnd_error.append(ogl_err_base).append(L"couldn't create the window for the OpenGL renderer").append(reboot_recommend);
        errBox(wnd_error.c_str());
        goto just_get_out;
    }
    const HDC hdc = GetDC(hOglWnd); // New device context for window
    if (!hdc) { // If it couldn't be created, get out
        std::wstring hdc_error;
        hdc_error.reserve(256);
        hdc_error.append(ogl_err_base).append(L"couldn't create a device context for the OpenGL renderer\n").append(reboot_recommend);
        errBox(hdc_error.c_str());
        goto cleanup_window;
    }
    { // Define pixel format for rendering
        const PIXELFORMATDESCRIPTOR pfd = {
            sizeof(PIXELFORMATDESCRIPTOR), 1, 
            PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, 
            PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
            0, 0, 0, 0, PFD_MAIN_PLANE, 0, 0, 0, 0
        };
        const int pf = ChoosePixelFormat(hdc, &pfd); // Select closest available in graphics driver
        if (!pf || !SetPixelFormat(hdc, pf, &pfd)) { // If no selectable pixel format could be obtained, get out
            std::wstring pf_error;
            pf_error.reserve(256);
            pf_error.append(ogl_err_base).append(L"couldn't set pixel format for OpenGL renderer's device context\n").append(reboot_recommend);
            errBox(pf_error.c_str());
            goto cleanup_dc;
        }
    }
    const HGLRC hglrc = wglCreateContext(hdc); // New rendering context
    if (!hglrc) { // If it couldn't be created, get out
        std::wstring hglrc_error;
        hglrc_error.reserve(256);
        hglrc_error.append(ogl_err_base).append(L"couldn't create OpenGL rendering context\n").append(reboot_recommend);
        errBox(hglrc_error.c_str());
        goto cleanup_dc;
    }
    if (!wglMakeCurrent(hdc, hglrc)) { // If the rendering context couldn't be activated for OGL-related commands, get out
        std::wstring hglrc_error;
        hglrc_error.reserve(256);
        hglrc_error.append(ogl_err_base).append(L"couldn't activate created OpenGL rendering context\n").append(reboot_recommend);
        errBox(hglrc_error.c_str());
        goto cleanup_context;
    }
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Set default plane color from arg flag
    glClear(GL_COLOR_BUFFER_BIT); // Render next frame as empty plane (with that default color)
    SwapBuffers(hdc); // Update the window to that next frame
    SetLayeredWindowAttributes(hOglWnd, 0, MAX_BYTE, LWA_ALPHA);
    { // Make the window a toolwindow
        const LONG_PTR exStyle = GetWindowLongPtrW(hOglWnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hOglWnd, GWL_EXSTYLE, exStyle | (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT)); // Set styles
        SetWindowPos(hOglWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED); // Apply
    }
    const HANDLE hTimer = CreateWaitableTimerW(NULL, FALSE, L"OGL-Timer"); // Create a timer, later used to dictate the frametime
    if (!hTimer) { // If it couldn't be created, get out
        std::wstring timer_error;
        timer_error.reserve(256);
        timer_error.append(ogl_err_base).append(L"couldn't create frametime timer, OpenGL renderer not usable\n").append(reboot_recommend);
        errBox(timer_error.c_str());
        goto cleanup_wgl;
    }
    {
        LARGE_INTEGER liDueTime{}; // Timer interval
        g_OglThreadState.store(IDGL_STATE_RUNNING, std::memory_order_relaxed); // Set OpenGL thread state to "running"
        g_OglSomethingChanged.store(false, std::memory_order_relaxed); // Set "something changed" flag to false
        // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than highest limit index for OpenGL mailbox
        for (int i = 0; i < OglKeyInd::OGLIND_MAX; ++i) {
            localDirty[i] = true; // Set "isDirty" flag to true so setting gets read from local values
        }
        local_stateVar = IDGL_STATE_RUNNING;
        std::atomic_thread_fence(std::memory_order_release);
        goto init_settings;
        while (true) { // Keep rendering unless loop is broken
            if (g_OglSomethingChanged.exchange(false, std::memory_order_acq_rel)) { // If thread is notified that something changed
                if (g_OglStopThread.load(std::memory_order_relaxed)) break; // If thread must stop, exit the rendering loop
                // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than highest limit index for OpenGL mailbox
                for (int i = 0; i < OglKeyInd::OGLIND_MAX; ++i) {
                    localDirty[i] = g_OglMailbox[i].isDirty.exchange(false, std::memory_order_relaxed); // Copy "isDirty" flag locally and set original to false
                    if (localDirty[i]) localValues[i] = g_OglMailbox[i].rawVal; // Copy setting from mailbox locally if "isDirty" flag is true
                }
                init_settings:
                if (localDirty[IDGL_XCOORD] || localDirty[IDGL_YCOORD]) { // If coordinates setting changed
                    // Move the window accordingly
                    const int x = static_cast<int>(localValues[IDGL_XCOORD]);
                    const int y = static_cast<int>(localValues[IDGL_YCOORD]);
                    SetWindowPos(hOglWnd, HWND_TOP, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                if (localDirty[IDGL_WIDTH] || localDirty[IDGL_HEIGHT]) { // If size setting changed
                    // Resize the window accordingly
                    const int width = static_cast<int>(localValues[IDGL_WIDTH]);
                    const int height = static_cast<int>(localValues[IDGL_HEIGHT]);
                    SetWindowPos(hOglWnd, HWND_TOP, 0, 0, width, height, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
                }
                if (localDirty[IDGL_COLOR]) { // If color setting changed
                    constexpr float divider = 255.0f;
                    // Make float values for rgb colors
                    const float r = ((localValues[IDGL_COLOR] >> 16) & 0xFF) / divider;
                    const float g = ((localValues[IDGL_COLOR] >> 8) & 0xFF) / divider;
                    const float b = (localValues[IDGL_COLOR] & 0xFF) / divider;
                    SetLayeredWindowAttributes(hOglWnd, 0, (localValues[IDGL_COLOR] >> 24) & 0xFF, LWA_ALPHA); // Update opacity
                    glClearColor(r, g, b, 1.0f); // Update plane color (rgb)
                }
                if (localDirty[IDGL_SHOW]) { // If window show state setting changed
                    // Update show state
                    const DWORD showState = static_cast<bool>(localValues[IDGL_SHOW]) ? SW_SHOWNOACTIVATE : SW_HIDE;
                    ShowWindow(hOglWnd, showState);
                }
                if (localDirty[IDGL_TOPMOST]) { // If window's "always-on-top" flag changed
                    LONG_PTR exStyle = GetWindowLongPtrW(hOglWnd, GWL_EXSTYLE); // Get current extended styles from the window
                    HWND hwnd_insert;
                    // Edit styles var according to new state
                    if (static_cast<bool>(localValues[IDGL_TOPMOST])) { // If topmost flag is enabled
                        hwnd_insert = HWND_TOPMOST;
                        exStyle |= WS_EX_TOPMOST; // Add topmost style and enable click-through
                    } else {
                        hwnd_insert = HWND_NOTOPMOST;
                        exStyle &= ~WS_EX_TOPMOST; // Remove topmost style and disable click-through
                    }
                    SetWindowLongPtrW(hOglWnd, GWL_EXSTYLE, exStyle); // Set new extended styles combination
                    SetWindowPos(hOglWnd, hwnd_insert, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE); // Apply styles and set always on top if required
                }
                if (localDirty[IDGL_HIGHPRECIS]) { // If "high precision" flag changed
                    // Toggle 1ms Windows scheduling precision
                    if (static_cast<bool>(localValues[IDGL_HIGHPRECIS])) {
                        timeBeginPeriod(1);
                    } else { timeEndPeriod(1); }
                }
            }
            DWORD totalElapsedMs = 0;
            while (totalElapsedMs < 300) { // Exit this subloop only every 300ms
                const DWORD frameStart = GetTickCount();
                glClear(GL_COLOR_BUFFER_BIT); // Render next frame as empty plane
                if (!SwapBuffers(hdc)) { // Update the window to that next frame
                    // If it failed, wait a bit then re-activate OpenGL rendering context
                    Sleep(16);
                    wglMakeCurrent(hdc, hglrc);
                }
                const DWORD elapsedMs = GetTickCount() - frameStart;
                if (elapsedMs < localValues[IDGL_FRAMETIME]) {
                    liDueTime.QuadPart = -static_cast<LONGLONG>(localValues[IDGL_FRAMETIME] - elapsedMs) * 10000; // Set waiting time accounting for the time that was already spend generating the frame
                    SetWaitableTimer(hTimer, &liDueTime, 0, NULL, nullptr, FALSE); // Launch the timer
                    WaitForSingleObject(hTimer, INFINITE); // Wait for the timer to go off
                }
                totalElapsedMs += (GetTickCount() - frameStart);
                if (elapsedMs > 2500) { // Compare the timestamps before and after frame gen to detect wake from sleep or freeze
                    // Re-activate OpenGL rendering context
                    fwprintf(stdout, L"[*] OGL renderer: system wake or freeze detected, re-activating OpenGL rendering context\n");
                    wglMakeCurrent(hdc, hglrc);
                }
            }
        }
    }
    // CLeanup sequence (can be joined at any stage)
    CloseHandle(hTimer);
      cleanup_wgl: {
        wglMakeCurrent(NULL, NULL);
    } cleanup_context: {
        wglDeleteContext(hglrc);
    } cleanup_dc: {
        ReleaseDC(hOglWnd, hdc);
    } cleanup_window: {
        DestroyWindow(hOglWnd);
        hOglWnd = NULL;
    } just_get_out: {
        // Verify the loop was reached as intended (if yes, set thread state to "stopped", else set it to "failed")
        g_OglThreadState.store((local_stateVar == IDGL_STATE_INIT) ? IDGL_STATE_FAILED : IDGL_STATE_STOPPED, std::memory_order_release);
        if (hOglDriver) FreeLibrary(hOglDriver);
        timeEndPeriod(1); // Turn off 1ms windows scheduling precision
        return 0;
    }
}


// Utiltiy function for starting the OpenGL renderer thread
__declspec(noinline) static bool OglSpooferStart(void) {
    DWORD *const pParams = static_cast<DWORD*>(malloc(sizeof(DWORD) * OglKeyInd::OGLIND_MAX)); // Allocate space for initial OpenGL params
    if (!pParams) return false;
    for (int i = 0; i < OglKeyInd::OGLIND_MAX; ++i) { pParams[i] = g_OglMailbox[i].rawVal; } // Set initial OpenGL params
    HANDLE hNew = NULL;
    hNew = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &OpenGLSpooferRenderThread, pParams, 0, nullptr));
    hOglThread.store(hNew, std::memory_order_release);
    if (!hNew) {
        free(pParams);
        g_errMsg.clear(); // Empty g_errMsg
        g_errMsg.append(msg_error).append(L"OpenGL renderer thread couldn't spawn\n").append(reboot_recommend);
        errBox(g_errMsg.c_str());
        return false;
    }
    return true;
}
// Main manager function for the OpenGL renderer thread
bool OglSpooferManage(const int op, const HWND hCheck) {
    bool toReturn = true;
    struct { // UI trigger validity checker
        __forceinline bool operator()(const HWND hBtn) {
            return (hBtn && IsWindow(hBtn));
        }
    } btnValid;
    const int currOglState = g_OglThreadState.load(std::memory_order_acquire);
    switch (op) {
        case 0: { sync_state: // Sync state block
            if (btnValid(hCheck)) {
                EnableWindow(hCheck, FALSE);
                SetWindowTextW(hCheck, L"Syncing...");
            }
            Sleep(250);
            if (currOglState <= IDGL_STATE_FAILED) { // Thread is stopped
                const HANDLE hOld = hOglThread.exchange(NULL, std::memory_order_acq_rel);
                if (hOld) CloseHandle(hOld);
                thread_stopped:
                if (btnValid(hCheck)) {
                    SendMessageW(hCheck, BM_SETCHECK, BST_UNCHECKED, 0);
                    SetWindowTextW(hCheck, OGL_TOGGLE_START);
                    goto re_enable;
                }
                return toReturn;
            }
            // Thread is running (currOglState > IDGL_STATE_FAILED)
            if (hOglThread.load(std::memory_order_relaxed) == NULL) {
                g_OglThreadState.store(IDGL_STATE_STOPPED, std::memory_order_release);
                goto thread_stopped;
            }
            if (btnValid(hCheck)) {
                SendMessageW(hCheck, BM_SETCHECK, BST_CHECKED, 0);
                SetWindowTextW(hCheck, OGL_TOGGLE_STOP);
                goto re_enable;
            }
            return toReturn;
        }
        case 1: { // Stop thread block
            if (currOglState <= IDGL_STATE_FAILED) { toReturn = false; goto sync_state; } // If already stopped, sync state
            g_OglStopThread.store(true, std::memory_order_relaxed); // Toggle thread stop flag on
            g_OglSomethingChanged.store(true, std::memory_order_relaxed); // Ensure thread will read state update
            // Block button and update its text to reflect thread stopping process
            if (btnValid(hCheck)) {
                EnableWindow(hCheck, FALSE);
                SetWindowTextW(hCheck, L"Stopping...");
            }
            std::atomic_thread_fence(std::memory_order_release);
            Sleep(400);
            // Update button text
            if (btnValid(hCheck)) {
                SetWindowTextW(hCheck, L"Sync /\nRequest stop again");
                goto re_enable;
            }
            return toReturn;
        }
        case 2: { // Start thread block
            if (currOglState > IDGL_STATE_FAILED) { toReturn = false; goto sync_state; } // If already running, sync state
            g_OglStopThread.store(false, std::memory_order_relaxed); // Ensure thread stop flag is disabled
            // Block button and update its text to reflect thread starting process
            if (btnValid(hCheck)) {
                EnableWindow(hCheck, FALSE);
                SetWindowTextW(hCheck, L"Starting...");
            }
            std::atomic_thread_fence(std::memory_order_release);
            // Initialize OpenGL Spoofer into a separate thread
            if (hOglThread.load(std::memory_order_relaxed) != NULL) {
                g_errMsg.clear(); // Empty g_errMsg
                g_errMsg.append(msg_error).append(L"OpenGL renderer thread appears to already be running\n").append(reboot_recommend);
                errBox(g_errMsg.c_str());
            } else if (!OglSpooferStart()) {
                toReturn = false;
                // Restore UI state
                if (btnValid(hCheck)) {
                    SetWindowTextW(hCheck, OGL_TOGGLE_START);
                    goto re_enable;
                }
                return toReturn;
            }
            Sleep(400);
            std::atomic_thread_fence(std::memory_order_release);
            // Unblock button and update its text
            if (btnValid(hCheck)) {
                SendMessageW(hCheck, BM_SETCHECK, BST_CHECKED, 0);
                SetWindowTextW(hCheck, OGL_TOGGLE_STOP);
                re_enable:
                flushBtnClicks(hCheck);
                EnableWindow(hCheck, TRUE);
            }
            return toReturn;
        }
    }
    __assume(0); // Tell the compiler this will just never happen
}


// Function for loading one or all of the OpenGL related settings to the OGL mailbox
bool OglSettingLoad(const int keyToLoad) {
    if (keyToLoad == 0) { // "Load all settings" block
        // Loop that restarts for each entry in OGL settings map
        for (const std::pair<int, int> pair : OglSettingsMap) {
            const DWORD val = g_cfg.getAsDWORD(SEC_OGLSPOOFER, pair.first); // Get value from runtime settings
            g_OglMailbox[pair.second].rawVal = val; // Store it into the OGL mailbox
        }
        return true;
    }
    { // "Load requested setting" block
        const std::unordered_map<int, int>::const_iterator it = OglSettingsMap.find(keyToLoad); // Find item via ID hash
        if (it == OglSettingsMap.end()) return false; // If entry wasn't found return false
        const DWORD val = g_cfg.getAsDWORD(SEC_OGLSPOOFER, keyToLoad); // Get value from runtime settings
        g_OglMailbox[it->second].rawVal = val; // Store it into the OGL mailbox
        g_OglMailbox[it->second].isDirty.store(true, std::memory_order_release); // Set "isDirty" flag to true
    }
    return true;
}


// Function for initializing the OpenGL rendered, creating the class and then launching the thread if required
bool OglSpooferInit(void) {
    // Build up window class:
    WNDCLASSEXW oglDum{};
    oglDum.cbSize = sizeof(WNDCLASSEXW); // Set binary size
    oglDum.lpfnWndProc = DefWindowProcW; // Assign callback function
    oglDum.lpszClassName = OGL_SPOOFER_CLASS_NAME; // Set unique class name
    oglDum.hInstance = g_hInstRelay; // Assign parent process instance
    RegisterClassExW(&oglDum); // Register the class
    OglSettingLoad();
    return (*static_cast<const bool*>(g_cfg.get(SEC_OGLSPOOFER, KEY_USEOGL))) ? OglSpooferStart() : true; // Start the thread if necessary
}
