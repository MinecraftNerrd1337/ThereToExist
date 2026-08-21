#include "resource.h"
#include "global.h"
#include <windows.h>
#include <atomic>
#include <string_view>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WM_TRAYICON (WM_USER + 1)
#define WM_PICKCOORDS (WM_USER + 101)
#define WM_UPDATECOLORFIELD (WM_USER + 102)
using namespace std::string_view_literals;


namespace {
    // Define arbitrary structure for UI triggers attributes
    struct SettingField {
        HWND *const hCtrlPtr;
        const LPCWSTR className;
        const LPCWSTR text;
        const int id;
        const DWORD styles;
        const int cfgKeyId;
        const UINT maxLength;
        bool isDirty = false;
    };
    // Define arbitrary structure for toast animation context
    struct ToastAnimContext {
        LPCWSTR text;
        COLORREF bg;
        BYTE opacity;
        WNDPROC pfnOldProc = nullptr;
        std::atomic<bool> bOrphaned{ false };
    };
}
namespace trig {
    HWND g_hCbLaunchMin = NULL;
    HWND g_hCbMinOnClose = NULL;
    HWND g_hBtnExit = NULL;
    HWND g_hPreset1 = NULL;
    HWND g_hPreset2 = NULL;
    HWND g_hPreset3 = NULL;
    HWND g_hGearBtn = NULL;

    // Define arbitrary structure for UI triggers info
    struct info {
        HWND *const hTrigPtr;
        std::wstring_view text;
        int id;
        DWORD trigType;
        DWORD styles = 0;
        int cfgKeyId = 0;
    };
}
namespace prst {
    int g_editingPreset = 0;
    std::wstring g_presetNames[3] = { L"Preset 1", L"Preset 2", L"Preset 3" };
}
namespace ogl {
    HWND g_hOglTabDlg = NULL;
    HHOOK g_hLocalKbdHook = NULL;
    HICON g_hIconOglSync = NULL;
    HICON g_hIconRedLight = NULL, g_hIconGreenLight = NULL;
    HICON g_hIconMouse = NULL, g_hIconScale = NULL, g_hIconColor = NULL;
    bool g_pickingCoords = false;

    // Define arbitrary structure for color selector context
    struct PickColorContext {
        DWORD initialARGB;
        DWORD currentARGB;
        HBRUSH hPreviewBrush;
        HBITMAP hSquareBmp;
        HFONT txtFont;
        bool isUpdatingSync;
        bool isDragging;
        int ui_scale, ptX, ptY;
        bool wasApplied;
    };
}


// Static list for UI triggers info, utitlizing global structure TrigInfo
constexpr trig::info TrigsReg[] = {
    { &trig::g_hCbLaunchMin, L"Launch minimized to tray"sv, ID_CbLaunchMin, BS_AUTOCHECKBOX, 0, KEY_LAUNCHMIN }, 
    { &trig::g_hCbMinOnClose, L"Minimize to tray on close"sv, ID_CbMinOnClose, BS_AUTOCHECKBOX, 0, KEY_MINONCLOSE }, 
    { &trig::g_hBtnExit, L"Exit process"sv, ID_TRAY_EXIT, BS_PUSHBUTTON }, 
    { &trig::g_hPreset1, L"Preset 1"sv, ID_Preset1, BS_PUSHBUTTON, BS_MULTILINE }, 
    { &trig::g_hPreset2, L"Preset 2"sv, ID_Preset2, BS_PUSHBUTTON, BS_MULTILINE }, 
    { &trig::g_hPreset3, L"Preset 3"sv, ID_Preset3, BS_PUSHBUTTON, BS_MULTILINE }, 
    { &trig::g_hGearBtn, L""sv, ID_GearBtn, BS_PUSHBUTTON, BS_ICON }, 
    { nullptr, L""sv, 0, 0, 0, 0 }
};


// Compile time counters and coherence verifier
static constexpr int cs_sliders[] = { IDC_SLIDER_R, IDC_SLIDER_G, IDC_SLIDER_B, IDC_SLIDER_A }; // Color selector sliders IDs
static constexpr int cs_edits[] = { IDC_EDIT_R, IDC_EDIT_G, IDC_EDIT_B, IDC_EDIT_A }; // Color selector fields IDs
static constexpr int TRIGS_COUNT = std::size(TrigsReg) - 1; // Don't count NULL terminator row
static constexpr int CHKBOX1_TRIG_IDX = []() -> int {
    for (int i = 0; i < TRIGS_COUNT; ++i) {
        if ((TrigsReg[i].trigType & BS_TYPEMASK) == BS_AUTOCHECKBOX) { return i; }
    }
    return 0;
}();
static constexpr int CHKBOX_COUNT = []() -> int {
    for (int i = CHKBOX1_TRIG_IDX; i < TRIGS_COUNT; ++i) {
        if ((TrigsReg[i].trigType & BS_TYPEMASK) != BS_AUTOCHECKBOX) { return i - CHKBOX1_TRIG_IDX; }
    }
    return 0;
}();
static constexpr int PRST1_TRIG_IDX = []() -> int {
    for (int i = 0; i < TRIGS_COUNT; ++i) {
        if (TrigsReg[i].text.substr(0, 7) == L"Preset ") { return i; }
    }
    return 0;
}();
static constexpr int PRESET_COUNT = []() -> int {
    for (int i = PRST1_TRIG_IDX; i < TRIGS_COUNT; ++i) {
        if (TrigsReg[i].text.substr(0, 7) != L"Preset ") { return i - PRST1_TRIG_IDX; }
    }
    return 0;
}();
static_assert(
    PRESET_COUNT > 0 && 
    []() -> bool {
        for (int i = 0; i < PRESET_COUNT; ++i) {
            if (TrigsReg[PRST1_TRIG_IDX + i].id != ID_Preset1 + i) { return false; }
        }
        return true;
    }(),
    "UI triggers registry does not include preset buttons in the correct order"
);
static_assert(
    IDC_PICKCOLOR_APPLY == IDC_PICKCOLOR_RESET + 1, 
    "Color selector's apply button ID must be 1 higher than reset button ID"
);
static_assert(
    []() -> bool {
        for (int i = 1; i < std::size(cs_sliders); ++i) {
            if (cs_sliders[i] != cs_sliders[i - 1] + 1) { return false; }
        }
        return true;
    }(), 
    "Color selector sliders IDs must be consecutive numbers!"
);
static_assert(
    []() -> bool {
        for (int i = 1; i < std::size(cs_edits); ++i) {
            if (cs_edits[i] != cs_edits[i - 1] + 1) { return false; }
        }
        return true;
    }(), 
    "Color selector fields IDs must be consecutive numbers!"
);
static_assert(
    []() -> bool {
        const int presets_secIds[] = { SEC_PRESET1, SEC_PRESET2, SEC_PRESET3 };
        for (int i = 1; i < std::size(presets_secIds); ++i) {
            if (presets_secIds[i] != presets_secIds[i - 1] + 1) { return false; }
        }
        return true;
    }(), 
    "Preset IDs must be consecutive numbers!"
);
