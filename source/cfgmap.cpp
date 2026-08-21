#include "global.h"
#include <cstdint>
#include <string>
#include <string_view>
using namespace std::string_view_literals;


namespace sec_string {
    // Variables for section names
    constexpr std::string_view general = "General"sv;
    constexpr std::string_view oglSpoofer = "OGL Spoofer"sv;
    constexpr std::string_view preset1 = "Preset 1"sv, preset2 = "Preset 2"sv, preset3 = "Preset 3"sv;
}
namespace key_string {
    // Variables for key names that get used several times
    constexpr std::string_view name = "Name"sv;
    constexpr std::string_view cmd = "Command"sv;
    constexpr std::string_view startup = "Startup"sv;
    constexpr std::string_view delay = "Delay"sv;
    constexpr std::string_view priv = "Privileges"sv;
    constexpr std::string_view console = "ShowConsole"sv;
}
namespace cfgMap {
    // Variables with values that get used multiple times, declared only once here
    const bool boolTrue = true, boolFalse = false;
    const int intZero = 0, intFourHundred = 400, intThreeHundred = 300;
    const int intDefaultFrametime = 16;
    const std::wstring wstringEmpty;
    const DWORD dwDefaultOverlayColor = 0x80000000;

    // Config map, utitlizing global structure cfgSetting
    const cfgSetting registry[] = {
        { SEC_GENERAL, KEY_LAUNCHMIN, sec_string::general, "LaunchMinimized"sv, &boolFalse, TYPE_BOOL }, 
        { SEC_GENERAL, KEY_MINONCLOSE, sec_string::general, "MinimizeOnClose"sv, &boolTrue, TYPE_BOOL }, 

        { SEC_OGLSPOOFER, KEY_USEOGL, sec_string::oglSpoofer, "UseOGL"sv, &boolFalse, TYPE_BOOL }, 
        { SEC_OGLSPOOFER, KEY_OGLXCOORD, sec_string::oglSpoofer, "OGLxCoord"sv, &intZero, TYPE_INT }, 
        { SEC_OGLSPOOFER, KEY_OGLYCOORD, sec_string::oglSpoofer, "OGLyCoord"sv, &intZero, TYPE_INT }, 
        { SEC_OGLSPOOFER, KEY_OGLWIDTH, sec_string::oglSpoofer, "OGLwidth"sv, &intFourHundred, TYPE_INT }, 
        { SEC_OGLSPOOFER, KEY_OGLHEIGHT, sec_string::oglSpoofer, "OGLheight"sv, &intThreeHundred, TYPE_INT }, 
        { SEC_OGLSPOOFER, KEY_OGLCOLOR, sec_string::oglSpoofer, "OGLcolorARGB"sv, &dwDefaultOverlayColor, TYPE_DWORD }, 
        { SEC_OGLSPOOFER, KEY_OGLFRAMETIME, sec_string::oglSpoofer, "FrametimeCap100ns"sv, &intDefaultFrametime, TYPE_INT }, 
        { SEC_OGLSPOOFER, KEY_OGLSHOW, sec_string::oglSpoofer, "OGLshowState"sv, &boolTrue, TYPE_BOOL }, 
        { SEC_OGLSPOOFER, KEY_OGLTOP, sec_string::oglSpoofer, "OGLalwaysOnTop"sv, &boolTrue, TYPE_BOOL }, 
        { SEC_OGLSPOOFER, KEY_OGLPRECIS, sec_string::oglSpoofer, "HighConsistency"sv, &boolFalse, TYPE_BOOL }, 

        { SEC_PRESET1, KEY_PRESET_NAME, sec_string::preset1, key_string::name, &wstringEmpty, TYPE_WSTRING }, 
        { SEC_PRESET1, KEY_PRESET_CMD, sec_string::preset1, key_string::cmd, &wstringEmpty, TYPE_WSTRING }, 
        { SEC_PRESET1, KEY_PRESET_STARTUP, sec_string::preset1, key_string::startup, &boolFalse, TYPE_BOOL }, 
        { SEC_PRESET1, KEY_PRESET_DELAY, sec_string::preset1, key_string::delay, &intZero, TYPE_INT }, 
        { SEC_PRESET1, KEY_PRESET_PRIVILEGES, sec_string::preset1, key_string::priv, &intZero, TYPE_INT }, 
        { SEC_PRESET1, KEY_PRESET_SHOWCONSOLE, sec_string::preset1, key_string::console, &boolFalse, TYPE_BOOL }, 

        { SEC_PRESET2, KEY_PRESET_NAME, sec_string::preset2, key_string::name, &wstringEmpty, TYPE_WSTRING }, 
        { SEC_PRESET2, KEY_PRESET_CMD, sec_string::preset2, key_string::cmd, &wstringEmpty, TYPE_WSTRING }, 
        { SEC_PRESET2, KEY_PRESET_STARTUP, sec_string::preset2, key_string::startup, &boolFalse, TYPE_BOOL }, 
        { SEC_PRESET2, KEY_PRESET_DELAY, sec_string::preset2, key_string::delay, &intZero, TYPE_INT }, 
        { SEC_PRESET2, KEY_PRESET_PRIVILEGES, sec_string::preset2, key_string::priv, &intZero, TYPE_INT }, 
        { SEC_PRESET2, KEY_PRESET_SHOWCONSOLE, sec_string::preset2, key_string::console, &boolFalse, TYPE_BOOL }, 

        { SEC_PRESET3, KEY_PRESET_NAME, sec_string::preset3, key_string::name, &wstringEmpty, TYPE_WSTRING }, 
        { SEC_PRESET3, KEY_PRESET_CMD, sec_string::preset3, key_string::cmd, &wstringEmpty, TYPE_WSTRING }, 
        { SEC_PRESET3, KEY_PRESET_STARTUP, sec_string::preset3, key_string::startup, &boolFalse, TYPE_BOOL }, 
        { SEC_PRESET3, KEY_PRESET_DELAY, sec_string::preset3, key_string::delay, &intZero, TYPE_INT }, 
        { SEC_PRESET3, KEY_PRESET_PRIVILEGES, sec_string::preset3, key_string::priv, &intZero, TYPE_INT }, 
        { SEC_PRESET3, KEY_PRESET_SHOWCONSOLE, sec_string::preset3, key_string::console, &boolFalse, TYPE_BOOL }, 

        { 0, 0, {}, {}, nullptr, 0 }
    };
}
