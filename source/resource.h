#pragma once


// --- Icon Identifiers ---
// IDI_APPICON is used for both the executable file icon and the small icon displayed in the System Tray
#define IDI_APPICON                     101
#define IDI_GEAR_ICON                   111
#define IDI_SYNC_ICON                   121
#define IDI_RED_LIGHT_ICON              122
#define IDI_GREEN_LIGHT_ICON            123
#define IDI_MOUSE_CLICK_ICON            124
#define IDI_TRIANGLE_RULER_ICON         125
#define IDI_PAINT_BRUSH_ICON            126

// Define any missing messages
#ifndef WM_DPICHANGED
    #define WM_DPICHANGED 0x02E0
#endif
#ifndef DWMWA_CLOAK
    #define DWMWA_CLOAK 0x0D
#endif
#ifndef ID_APPLY_NOW
    #define ID_APPLY_NOW 0x3021
#endif

// --- Command Identifiers ---
#define ID_CbLaunchMin                  220
#define ID_CbMinOnClose                 221
#define ID_Preset1                      300
#define ID_Preset2                      301
#define ID_Preset3                      302
#define ID_GearBtn                      321
#define ID_TRAY_EXIT                    3000
#define ID_TRAY_SHOW                    3001
#define WM_SHOW_STARTUP                 3002
// Same but for settings window
#define IDD_PAGE_PRESETS                4100
#define IDD_PAGE_OGLSPOOF               4101
#define IDC_SETTING_STARTUP             4180
#define IDC_SETTING_CONSOLE             4181
#define IDC_SETTING_NAME                4260
#define IDC_SETTING_COMMANDS            4261
#define IDC_SETTING_DELAY               4262
#define IDC_SETTING_PRIVILEGES          4300
#define IDC_SETTING_PRESET_COMBO        4400
#define IDC_OGL_USEOGL                  4140
#define IDC_OGL_SYNCSTATE               4141
#define IDC_OGL_SHOW                    4180
#define IDC_OGL_TOP                     4181
#define IDC_OGL_PRECIS                  4182
#define IDC_OGL_XCOORD                  4230
#define IDC_OGL_YCOORD                  4231
#define IDC_OGL_WIDTH                   4232
#define IDC_OGL_HEIGHT                  4233
#define IDC_OGL_FRAMETIME               4260
#define IDC_OGL_COLOR                   4540
#define IDC_OGL_PICK_COORD              4565
#define IDC_OGL_SCALE_SIZE              4566
#define IDC_OGL_PICK_COLOR              4567
// Same but for color selector window
#define IDD_COLOR_SELECTOR              5001
#define IDC_COLOR_PREVIEW               5002
#define IDC_COLOR_SQUARE                5003
#define IDC_PICKCOLOR_RESET             5010
#define IDC_PICKCOLOR_APPLY             5011
#define IDC_PICKCOLOR_HEX               5016
#define IDC_SLIDER_R                    5030
#define IDC_SLIDER_G                    5031
#define IDC_SLIDER_B                    5032
#define IDC_SLIDER_A                    5033
#define IDC_EDIT_R                      5040
#define IDC_EDIT_G                      5041
#define IDC_EDIT_B                      5042
#define IDC_EDIT_A                      5043
