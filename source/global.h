#pragma once
#include <windows.h>
#include <cstdint>
#include <unordered_map>
#include <variant>
#include <string>
#include <string_view>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define MAKE_CFGID(sec, key) (static_cast<uint16_t>(((sec) << 8 | (key))))
using runtimeVal = std::variant<bool, int, std::wstring, DWORD>;
typedef HRESULT(WINAPI* DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);


enum _type : int { // IDs for variable types in runtime settings
    TYPE_BOOL = 0, 
    TYPE_INT, 
    TYPE_WSTRING, 
    TYPE_DWORD, 
    TYPE_WSTR_VIEW
};
enum _secId : int { // IDs for config sections
    SEC_GENERAL = 1, 
    SEC_OGLSPOOFER, 
    SEC_PRESET1, 
    SEC_PRESET2, 
    SEC_PRESET3
};
enum _keyId : int { // IDs for config keys
    KEY_LAUNCHMIN = 1, 
    KEY_MINONCLOSE, 

    KEY_USEOGL, 
    KEY_OGLXCOORD, 
    KEY_OGLYCOORD, 
    KEY_OGLWIDTH, 
    KEY_OGLHEIGHT, 
    KEY_OGLCOLOR, 
    KEY_OGLFRAMETIME, 
    KEY_OGLSHOW, 
    KEY_OGLTOP, 
    KEY_OGLPRECIS, 

    KEY_PRESET_NAME, 
    KEY_PRESET_CMD, 
    KEY_PRESET_STARTUP, 
    KEY_PRESET_DELAY, 
    KEY_PRESET_PRIVILEGES, 
    KEY_PRESET_SHOWCONSOLE
};


// Define arbitrary structure for supported config file settings
struct cfgSetting {
    const int secId;
    const int keyId;
    const std::string_view section;
    const std::string_view cfgKey;
    const void *const defaultVal;
    const int type; // 0 = Bool, 1 = Int, 2 = wstring, 3 = DWORD
};


// Define class for storage and usage of runtime settings
class cfgDict {
private:
    std::unordered_map<uint16_t, runtimeVal> _storage; // Runtime settings data
public:
    cfgDict() = default; // Default class constructor
    // Get and set functions for data (bodies in configmgmt.cpp)
    DWORD getAsDWORD(const int _sec, const int _key);
    const void* get(const int _sec, const int _key);
    bool set(const int _sec, const int _key, const void* _valPtr, const int _type, const uint32_t _txtLen = 0);
    bool verifyAndSet(const int _sec, const int _key, const int _type, std::string_view _rawVal);
};


// Global constant expression variables
constexpr uint32_t ERRBUF_SIZE = 16;
constexpr uint32_t MAX_BYTE = 0xFF;
static constexpr wchar_t DEFAULT_APP_NAME[] = L"ThereToExist";
static constexpr wchar_t reboot_recommend[] = L"If this issue persists, try restarting the program or even your computer\n"; // Message to recommend rebooting the program or even the machine
static constexpr wchar_t fatal_error[] = L"Fatal error: ";
static constexpr wchar_t msg_error[] = L"Error: ";


// Make the following variables visible externally:
extern cfgDict g_cfg; // cfgDict class variable g_cfg (config runtime settings)
extern std::wstring g_errMsg; // Global variable for error messages
extern std::wstring g_errWndName; // Name for error message windows
extern std::wstring_view g_execName; // Name of the executable
extern HWND g_hMainWnd; // Main window handle
extern HINSTANCE g_hInstRelay; // Relay to process instance

// Make the config map visible externally
namespace cfgMap {
    extern const cfgSetting registry[];
}


// Make the following functions visible externally:
void getExecName(void);
void getConfigPath(void);
bool cfgSave(void);
bool initCfg(void);
void presetLaunch(const int _presetSecId, const bool _autoLaunch = false);


// Global error handler when a message box is to be summoned
inline void errBox(LPCWSTR msgStr, const bool abort = false, const HWND hWnd = g_hMainWnd) {
    // Throw the error present in g_errMsg to console, and summon a message box
    fwprintf(stderr, L"[!] %ls\n", msgStr);
    MessageBoxW(hWnd, msgStr, g_errWndName.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    if (abort) exit(EXIT_FAILURE);
}
// Helper function that combines updating a runtime setting and applying the changes to the config file
__forceinline bool cfgChangeAndApply(const int secId, const int keyId, const void* newVal, const int type, const uint32_t txtLen = 0) {
    bool succeded = g_cfg.set(secId, keyId, newVal, type, txtLen);
    cfgSave();
    return succeded;
}
// Utility function for flushing any click messages for a UI trigger that could've occured while the trigger was disabled
__forceinline void flushBtnClicks(const HWND hBtn) {
    MSG msg;
    while(PeekMessageW(&msg, hBtn, WM_LBUTTONDOWN, WM_LBUTTONUP, PM_REMOVE)) {}
    while(PeekMessageW(&msg, hBtn, WM_COMMAND, WM_COMMAND, PM_REMOVE)) {}
}
