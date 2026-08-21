#include "global.h"
#include <process.h>
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <gl/GL.h>
#include <charconv>
#include <cwchar>
#include <string>
#include <string_view>
#include <memory>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX


namespace {
    // Arbitrary structure for preset launcher thread args (so they can all be passed as one pointer)
    struct launchContext {
        int presetSecId;
        int privileges;
        int delay;
        bool showConsole;
        std::wstring rawCommands;
        std::wstring targPresetName;
    };
}


// Helper function obtaining delay setting for auto-launch of a preset
static int getDelay(const int presetSecId, const std::wstring_view targPresetName) {
    int delayVar = -1;
    const int delayMax = 30000;
    const int zero = 0;
    // Loop that restarts as long as variable i (starting at 0 and getting incremented each turn) is less than 3, AND delayVar is negative
    for (int i = 0; i < 3 && delayVar < 0 ; ++i) {
        delayVar = *static_cast<const int*>(g_cfg.get(presetSecId, KEY_PRESET_DELAY)); // Get delay setting
        if (delayVar > delayMax) { // If delay setting is higher than maximum allowed
            fwprintf(stdout, L"[*] Delay set for %.*ls exceeds maximum of 30000ms, lowering it to that\n", targPresetName.size(), targPresetName.data());
            cfgChangeAndApply(presetSecId, KEY_PRESET_DELAY, &delayMax, TYPE_INT); // Set delay setting to max value
            delayVar = -1;
        } else if (delayVar < zero) {
            fwprintf(stdout, L"[*] Resetting delay set for %.*ls due to an invalid value\n", targPresetName.size(), targPresetName.data());
            cfgChangeAndApply(presetSecId, KEY_PRESET_DELAY, &zero, TYPE_INT); // Reset delay setting to zero
        }
    }
    if (delayVar < 0) { // If obtaining delay failed
        g_errMsg.clear(); // Empty g_errMsg
        g_errMsg.append(msg_error)
                .append(L"delay setting is invalid and somehow couldn't be reset in runtime, cannot auto-launch ")
                .append(targPresetName);
        errBox(g_errMsg.c_str());
    }
    return delayVar;
}


// Utility function for running a given command with optional elevation
static __forceinline DWORD runCommand(std::wstring_view rawCommands, const int privileges = 0, const bool showConsole = false) {
    // Prepare cmd launching header
    std::wstring cmdLine;
    cmdLine.reserve(8 + rawCommands.size());
    cmdLine.append(L"/c \"").append(rawCommands).append(L"\"");
    // Build up new SHELLEXECUTEINFOW structure
    SHELLEXECUTEINFOW sei = { sizeof(sei) }; // Set size
    sei.fMask = SEE_MASK_FLAG_NO_UI; // No UI flag
    sei.lpVerb = (privileges == 1) ? L"runas" : L"open"; // Account for privileges setting
    sei.lpFile = L"cmd.exe"; // Set cmd.exe
    sei.lpParameters = cmdLine.c_str(); // Set args
    sei.nShow = showConsole ? SW_SHOWNORMAL : SW_HIDE; // Apply "Show Console" setting
    SetLastError(ERROR_SUCCESS); // Reset last error
    if (!ShellExecuteExW(&sei)) return (GetLastError()); // Execute command and get result
    return ERROR_SUCCESS;
}


// Preset launcher function running in a separate thread
static unsigned __stdcall presetLauncherThread(void* pArgs) {
    std::unique_ptr<launchContext> pCtx(static_cast<launchContext*>(pArgs));
    if (!pCtx) return 1;
    // Do the delay (attempt precise method, if it fails revert to regular sleep)
    if (pCtx->delay > 0) {
        const HANDLE hTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        if (hTimer) {
            LARGE_INTEGER liDueTime;
            liDueTime.QuadPart = -static_cast<LONGLONG>(pCtx->delay) * 10000;
            if (SetWaitableTimer(hTimer, &liDueTime, 0, NULL, nullptr, FALSE)) {
                WaitForSingleObject(hTimer, INFINITE);
            } else goto normal_sleep;
            CloseHandle(hTimer);
        } else normal_sleep: { Sleep(pCtx->delay); }
    }
    const DWORD result = runCommand(pCtx->rawCommands, pCtx->privileges, pCtx->showConsole); // Run commands via relevant function
    if (result != ERROR_SUCCESS) { // If operation returned an error
        if (result == ERROR_CANCELLED) { // If user cancelled UAC prompt in admin mode
            fwprintf(stdout, L"[*] User cancelled the UAC prompt for %ls\n", pCtx->targPresetName.c_str());
        } else {
            char errBuf[ERRBUF_SIZE];
            const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, result); // Convert the DWORD
            std::wstring pLaunchErr;
            pLaunchErr.reserve(128 + pCtx->rawCommands.size());
            pLaunchErr.append(msg_error)
                        .append(L"Failed to execute command: ")
                        .append(pCtx->rawCommands)
                        .append(L"\nFailed with error code: ")
                        .append(errBuf, r.ptr);
            errBox(pLaunchErr.c_str());
        }
    }
    return 0;
}


// Utility function creating a separate thread launching a given preset
void presetLaunch(const int presetSecId, const bool autoLaunch) {
    std::wstring targPresetName = L"Preset 1";
    targPresetName[7] = L'1' + (presetSecId - SEC_PRESET1);
    std::wstring rawCommands = *static_cast<const std::wstring*>(g_cfg.get(presetSecId, KEY_PRESET_CMD)); // Get commands string
    if (rawCommands.empty()) {
        fwprintf(stdout, L"[*] No command is set for %ls, cancelling launch\n", targPresetName.c_str());
        return;
    }
    const int privileges = *static_cast<const int*>(g_cfg.get(presetSecId, KEY_PRESET_PRIVILEGES)); // Get privileges setting
    const int delay = (autoLaunch) ? getDelay(presetSecId, targPresetName) : 0; // Apply delay only for auto-launch
    const bool showConsole = *static_cast<const bool*>(g_cfg.get(presetSecId, KEY_PRESET_SHOWCONSOLE)); // Get "Show Console" setting
    // Prepare variables for preset launcher thread
    std::unique_ptr<launchContext> pCtx(new (std::nothrow) launchContext{
        presetSecId, 
        privileges, 
        delay, 
        showConsole, 
        std::move(rawCommands), 
        std::move(targPresetName)
    });
    // Start preset launcher thread
    const HANDLE hLaunchThread = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, 
        0, 
        &presetLauncherThread, 
        pCtx.get(), 
        0, 
        nullptr
    ));
    if (hLaunchThread) { // If the thread was launched, leave it be
        pCtx.release();
        CloseHandle(hLaunchThread);
    } else { // Else throw an error to console and summon a message box
        g_errMsg.clear(); // Empty g_errMsg
        g_errMsg.append(msg_error).append(L"unable to start preset launcher thread\n").append(reboot_recommend);
        errBox(g_errMsg.c_str());
    }
}
