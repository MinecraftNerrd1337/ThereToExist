#include "global.h"
#include <windows.h>
#include <shlobj.h>
#include <combaseapi.h>
#include <fstream>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <variant>
#include <charconv>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// Link against essential Windows library
#pragma comment(lib, "user32.lib")


// Global state variables
cfgDict g_cfg{};
std::wstring g_pathToCfg;
std::wstring_view g_execName;
namespace {
    constexpr int MAX_STACK = 512;
    bool g_portableMode = false;
    std::wstring g_fullExePath;
}


// Functions for converting wstrings over to UTF8 and vice versa
struct {
    __forceinline int operator()(const std::wstring_view inp) const {
        if (inp.empty()) return 0;
        return WideCharToMultiByte( // Return needed buffer size for conversion
            CP_UTF8, 0, inp.data(), static_cast<int>(inp.size()), 
            nullptr, 0, nullptr, nullptr
        );
    }
    __forceinline int operator()(const std::wstring_view inp, char *const outBuf, const int outCap) const {
        if (inp.empty()) return 0;
        return WideCharToMultiByte( // Convert the wstring into the buffer
            CP_UTF8, 0, inp.data(), static_cast<int>(inp.size()), 
            outBuf, outCap, nullptr, nullptr
        );
    }
} wstring_to_UTF8;
struct {
    __forceinline int operator()(const std::string_view inp) const {
        if (inp.empty()) return 0;
        return MultiByteToWideChar( // Return needed buffer size for conversion
            CP_UTF8, 0, inp.data(), static_cast<int>(inp.size()), 
            nullptr, 0
        );
    }
    __forceinline int operator()(const std::string_view inp, wchar_t *const outBuf, const int outCap) const {
        if (inp.empty()) return 0;
        return MultiByteToWideChar( // Convert the string into the buffer
            CP_UTF8, 0, inp.data(), static_cast<int>(inp.size()), 
            outBuf, outCap
        );
    }
} UTF8_to_wstring;


// Utility function for suppressing spaces, tabs and newlines at beginning and end of a standard string
static inline std::string_view trim(const std::string_view s) noexcept {
    // Find first non space, tab or newline char
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    // Find last non space, tab or newline char
    const size_t last = s.find_last_not_of(" \t\r\n");
    // Return everything in between
    return s.substr(first, (1 + last - first));
}


// Functions for class cfgDict:
// 1. Get a value
const void* cfgDict::get(const int sec, const int key) {
    const std::unordered_map<uint16_t, runtimeVal>::const_iterator it = _storage.find(MAKE_CFGID(sec, key)); // Find item via IDs hash
    if (it == _storage.end()) return nullptr; // If entry wasn't found return invalid pointer
    return std::visit([](auto&& val) -> const void* { return &val; }, it->second); // Otherwise return a pointer to it
}
// 2. Get a value as a DWORD
DWORD cfgDict::getAsDWORD(const int sec, const int key) {
    const std::unordered_map<uint16_t, runtimeVal>::const_iterator it = _storage.find(MAKE_CFGID(sec, key)); // Find item via IDs hash
    if (it == _storage.end()) return 0xFFFFFFFF; // If entry wasn't found return error flag
    return std::visit([](auto&& val) -> DWORD { // Otherwise return it as a DWORD (or return error flag if it's a wstring)
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::wstring>) {
            __assume(0); // Tell the compiler this will just never happen
        } else return static_cast<DWORD>(val);
    }, it->second);
}
// 3. Set a value
bool cfgDict::set(const int sec, const int key, const void* valPtr, const int type, const uint32_t txtLen) {
    if (!sec || !key || !valPtr) return false;
    // Build variable based on input, accounting for type
    runtimeVal constructedVal;
    switch (type) {
        case TYPE_BOOL: {
            constructedVal = *static_cast<const bool*>(valPtr);
            break;
        }
        case TYPE_INT: {
            constructedVal = *static_cast<const int*>(valPtr);
            break;
        }
        case TYPE_WSTRING: {
            constructedVal = *static_cast<const std::wstring*>(valPtr);
            break;
        }
        case TYPE_DWORD: {
            constructedVal = *static_cast<const DWORD*>(valPtr);
            break;
        }
        case TYPE_WSTR_VIEW: {
            const wchar_t* txtPtr = static_cast<const wchar_t*>(valPtr);
            constructedVal = std::wstring(txtPtr, txtLen);
            break;
        }
        default: { return false; }
    }
    _storage[MAKE_CFGID(sec, key)] = std::move(constructedVal); // Add the value
    return true;
}
// 4. Verify a value from config file, convert it to the required type, then set the value
bool cfgDict::verifyAndSet(const int sec, const int key, const int type, std::string_view rawVal) {
    if (!sec || !key) return false;
    // Attempt conversion from string to indented type, validating the value's appropriateness for the target type in the process
    runtimeVal varConverted;
    switch (type) {
        case TYPE_BOOL: {
            if (rawVal == "Y" || rawVal == "y" || rawVal == "1") {
                varConverted = true;
                break;
            } else if (rawVal == "N" || rawVal == "n" || rawVal == "0") {
                varConverted = false;
                break;
            } else { return false; }
        }
        case TYPE_INT: {
            int value;
            std::from_chars_result r = std::from_chars(rawVal.data(), rawVal.data() + rawVal.size(), value);
            if (r.ec != std::errc{}) return false;
            varConverted = value;
            break;
        }
        case TYPE_DWORD: {
            DWORD value;
            int base = 10;
            if (rawVal[0] == '0' && (rawVal[1] == 'x' || rawVal[1] == 'X')) {
                base = 16;
                rawVal.remove_prefix(2);
            }
            std::from_chars_result r = std::from_chars(rawVal.data(), rawVal.data() + rawVal.size(), value, base);
            if (r.ec != std::errc{}) return false;
            varConverted = value;
            break;
        }
        case TYPE_WSTRING: {
            const uint32_t needed_size = static_cast<uint32_t>(UTF8_to_wstring(rawVal));
            std::wstring& wstr = varConverted.emplace<std::wstring>();
            wstr.reserve(needed_size >= 16 ? needed_size : 16);
            wstr.resize(needed_size);
            UTF8_to_wstring(rawVal, wstr.data(), needed_size);
            break;
        }
        default: { return false; }
    }
    _storage[MAKE_CFGID(sec, key)] = std::move(varConverted); // Add the converted value
    return true;
}


// Function to convert a given value of any type over to standard string
static __forceinline std::string_view cfgValToUTF8(const void *const valPtr, const int type, char* outBuf, const int outCap) {
    // Perform the right conversion accounting for passed type
    switch (type) {
        case TYPE_BOOL: {
            outBuf[0] = (*static_cast<const bool*>(valPtr)) ? 'Y' : 'N';
            return std::string_view(outBuf, 1);
        }
        case TYPE_INT: {
            const int val = *static_cast<const int*>(valPtr);
            const std::to_chars_result r = std::to_chars(outBuf, outBuf + outCap, val);
            return std::string_view(outBuf, r.ptr - outBuf);
        }
        case TYPE_DWORD: {
            const DWORD val = *static_cast<const DWORD*>(valPtr);
            outBuf[0] = '0', outBuf[1] = 'x';
            std::to_chars_result r = std::to_chars(outBuf + 2, outBuf + outCap, val, 16);
            outBuf += 2;
            const uint32_t hexLen = r.ptr - outBuf;
            // Convert lowercase hex digits (a-f) to uppercase (A-F)
            for (char *p = outBuf; p < r.ptr; ++p) {
                if (*p >= 'a' && *p <= 'f') *p -= 32;
            }
            if (hexLen < 8) {
                const uint32_t pad = 8 - hexLen;
                memmove(outBuf + pad, outBuf, hexLen);
                memset(outBuf, '0', pad);
                r.ptr += pad;
            }
            outBuf -= 2;
            return std::string_view(outBuf, r.ptr - outBuf);
        }
        case TYPE_WSTRING: {
            int written = wstring_to_UTF8(*static_cast<const std::wstring*>(valPtr), outBuf, outCap);
            return std::string_view(outBuf, written);
        }
    }
    return {};
}


// Helper function for cleaning up the syntax of a given path
void cleanupDir(std::wstring& path) {
    if (path.length() < 2) return;
    wchar_t* const pStart = path.data();
    const wchar_t* pRead = pStart;
    const wchar_t* pEnd = pStart + path.length() - 1;
    wchar_t* pWrite = pStart;
    // If path both begins and ends with quotes, move pointers to ignore those quotes when parsing later
    if (*pStart == L'"' && *pEnd == L'"') {pRead++; pEnd--;}
    bool wasBackslash = false;
    // Loop that restarts as long as pRead hasn't caught up with pEnd, with pRead getting incremented each turn
    for (; pRead <= pEnd; ++pRead) {
        wchar_t c = *pRead;
        if (c == L'/') c = L'\\';
        if (c == L'\\') {
            if ((pWrite - pStart) > 1 && wasBackslash) continue;
            wasBackslash = true;
        } else wasBackslash = false;
        *pWrite = c; // Write c into path at pWrite
        pWrite++; // Increment pWrite
    }
    pWrite--;
    // Remove any backslashes, spaces or tabs at the end
    if (!path.empty()) {
        while (pWrite >= pStart && (*pWrite == L'\\' || *pWrite == L' ' || *pWrite == L'\t')) {
            pWrite--;
        }
    }
    path.resize(pWrite - pStart + 1);
}


// Function for fetching the executable's current name and path
void getExecName(void) {
    // If path to executable was never fetched, run fetching logic below
    if (g_fullExePath.empty()) {
        g_errMsg.clear(); // Empty g_errMsg
        constexpr DWORD STACK_BUF_SIZE = 261;
        constexpr DWORD LONGPATH_BUF_SIZE = 32768;
        DWORD bufSize = STACK_BUF_SIZE;
        wchar_t targBuf[STACK_BUF_SIZE];
        wchar_t* pBuf = &targBuf[0];
        std::unique_ptr<wchar_t[]> heapBuf;
        for (bool firstTurn = true; ; firstTurn = false) {
            const uint32_t result = GetModuleFileNameW(NULL, pBuf, bufSize);
            if (result == 0) fatal_error: {
                // Throw a fatal error to console, summon a message box then exit the program
                char errBuf[ERRBUF_SIZE];
                const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
                g_errMsg.append(fatal_error).append(L"couldn't get path to executable from WinAPI, failed with error code: ").append(errBuf, r.ptr);
                errBox(g_errMsg.c_str(), true);
            }
            if (result >= bufSize - 1) {
                if (firstTurn) goto long_path;
                goto fatal_error;
            }
            g_fullExePath.assign(pBuf, result);
            cleanupDir(g_fullExePath);
            fwprintf(stdout, L"[*] Acquired path to executable: %ls\n", g_fullExePath.c_str());
            break;
            long_path:
            bufSize = LONGPATH_BUF_SIZE;
            heapBuf = std::make_unique<wchar_t[]>(bufSize);
            pBuf = heapBuf.get();
        }
        g_execName = std::wstring_view(g_fullExePath); // Get view of full path string (store in another global var)
        const size_t lastBackslash = g_execName.find_last_of(L'\\'); // Find last backslash
        if (lastBackslash !=std::wstring_view::npos) g_execName.remove_prefix(lastBackslash + 1); // Move view start to right after that last backslash (if it exists)
        const size_t lastDot = g_execName.rfind(L'.'); // Find last dot
        if (lastDot != std::wstring_view::npos) g_execName.remove_suffix(g_execName.size() - lastDot); // Move view end to right before that last dot (if it exists)
    }
}


// Helper function for creating missing folders
static bool mkdirTasks(const std::wstring_view fullDir, const bool forceMkdir = false) {
    fwprintf(stdout, L"[*] Executing creation of config file dir (or verification that it exists)\n");
    bool fullSuccess = true;
    size_t CURRpos = 0;
    // The conditions block below allows avoiding to attempt creating long path roots or server locations as folders by skipping their positions in the text
    if (fullDir.size() >= 4 && fullDir.compare(0, 2, L"\\\\") == 0) {
        if (fullDir.size() >= 8 && fullDir.compare(2, 6, L"?\\UNC\\") == 0) CURRpos = 7;
        else {
            CURRpos = fullDir.find(L'\\', 2);
            if (CURRpos == std::wstring::npos) {
                fwprintf(stderr, L"[!] Error: invalid path: %.*ls\nThis path starts with two backslashes but doesn't have any other, which will very likely cause issues\n", fullDir.size(), fullDir.data());
                CURRpos = fullDir.size();
                fullSuccess = false;
            }
        }
    }
    // The for loop below allows avoiding to attempt creating drive letters or roots as folders by skipping their positions in the text
    // Loop that restarts as long as variable i (starting at CURRpos and getting incremented each turn) is less than fullDirSize
    for (uint32_t i = CURRpos; i < fullDir.size(); ++i) {
        const wchar_t c = fullDir[i];
        if (c == L'\\' || c == L':') CURRpos = i + 1;
        else if (i + 1 >= fullDir.size() || (fullDir[i + 1] != L':' && c != L'.')) break;
    }
    const size_t ENpos = fullDir.rfind(g_execName); // Find execName string in fullDir
    while ((CURRpos = fullDir.find(L'\\', CURRpos)) != std::wstring::npos) { // Find next backslash char from current position
        const std::wstring_view currentDir = fullDir.substr(0, CURRpos);
        // Check if current folder to create is execName string
        const bool isENfolder = (ENpos != std::wstring::npos && ENpos == currentDir.rfind(g_execName));
        if (forceMkdir || isENfolder) {
            const std::wstring currentDirWStr(currentDir);
            SetLastError(ERROR_SUCCESS); // Reset last error
            // Attempt creating directory stored in currentDir
            if (!CreateDirectoryW(currentDirWStr.c_str(), NULL)) {
                if (GetLastError() == ERROR_ALREADY_EXISTS) {
                    fwprintf(stdout, L"[*] Skipping already existing folder at: %ls\n", currentDirWStr.c_str());
                } else {
                    fwprintf(stderr, L"[!] Warning: Folder doesn't exist and couldn't be created at: %ls\nThis could easily lead to errors later on\n", currentDirWStr.c_str());
                    fullSuccess = false;
                }
            } else {
                fwprintf(stdout, L"[*] Folder was successfully created at: %ls\n", currentDirWStr.c_str());
            }
        }
        CURRpos++;
    }
    return fullSuccess;
}


// Helper function for managing and giving the path to the configuration file (cfg.ini)
void getConfigPath(void) {
    static constexpr wchar_t CFGFILE_NAME[] = L"\\cfg.ini";
    g_errMsg.clear(); // Empty g_errMsg
    bool forceMkdir = false;
    std::wstring_view customPathView;
    // Parse launch arguments one by one
    if (__wargv) for (int i = 1; i < __argc; ++i) {
        std::wstring_view checkedArg(__wargv[i]); // Get current arg
        if (checkedArg.size() < 2) continue;
        if (checkedArg[0] == L'-') { // If arg starts with a dash
            if (checkedArg[1] == L'-') { // If it's followed by a second dash
                size_t eqPos = checkedArg.find(L'='); // Find "=" in arg
                if (eqPos == std::wstring_view::npos) continue;
                // Extract both key and value
                std::wstring_view key = checkedArg.substr(0, eqPos);
                std::wstring_view val = checkedArg.substr(eqPos + ((checkedArg[eqPos + 1] == L'=') ? 2 : 1));
                if (key == L"--configpath") customPathView = val;
            } else {
                if (checkedArg == L"-forcemkdir") forceMkdir = true;
                else if (checkedArg == L"-portable") {
                    g_portableMode = true;
                    fwprintf(stdout, L"[*] Ensured portable mode is enabled, as per argument \"-portable\"");
                }
            }
        }
    }
    std::wstring pathToGive;
    if (!customPathView.empty()) { // A custom path was passed via arg "--configpath"
        pathToGive = customPathView;
        cleanupDir(pathToGive);
        // Verifying that the path exists or can be created (doing so in the process)
        if (!mkdirTasks(pathToGive, forceMkdir) && forceMkdir) {
            // Throw a fatal error to console, summon a message box then exit the program
            g_errMsg.append(fatal_error)
                    .append(L"argument -forceMkdir was passed, but cfg directory doesn't exist and couldn't be created, aborting!\ncfg dir: ")
                    .append(pathToGive);
            errBox(g_errMsg.c_str(), true);
        }
    } else if (pathToGive.empty()) run_it_back: { // No custom path was passed via arg "--configpath"
        if (g_portableMode) {
            fwprintf(stdout, L"[*] Portable mode found to be enabled, proceeding accordingly for config path\n");
            const size_t lastBackslash = g_fullExePath.rfind(L'\\'); // Find last backslash in path to executable
            // If last backslash exists (which it always should), strip everything starting from last backslash and throw result in pathToGive
            if (lastBackslash != std::wstring::npos) pathToGive = g_fullExePath.substr(0, lastBackslash);
            else {
                fwprintf(stderr, L"[!] Warning: obtained seemingly invalid path as path to the executable: %ls\n"
                    L"Using the directory the application was started in/from instead\n", g_fullExePath.c_str());
                pathToGive = L"."; // Use the directory the system shell was at when it launched the app
            }
            pathToGive.append(CFGFILE_NAME);
        } else {
            int pathType = 1;
            wchar_t szPath[261];
            // Get path to AppData\Roaming
            HRESULT hrPath = SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, szPath);
            if (!SUCCEEDED(hrPath)) { // If operation failed
                // Get path to user's home folder
                hrPath = SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, szPath);
                if (SUCCEEDED(hrPath)) { // If this operation succeeded
                    pathType = 2;
                } else { // If it also failed
                    pathType = 0;
                    g_portableMode = true; // Fall back on portable mode
                    g_errMsg.append(msg_error).append(L"couldn't join user appdata or home folder, using portable mode instead");
                    errBox(g_errMsg.c_str());
                    goto run_it_back;
                }
            }
            pathToGive.append(szPath); // Throw result into pathToGive
            cleanupDir(pathToGive);
            if (pathToGive.back() != L'\\') pathToGive += L'\\'; // Ensure pathToGive ends with a backslash
            if (pathType == 2) pathToGive.append(L".config\\"); // %userprofile%\.config mode
            pathToGive.append(g_execName).append(CFGFILE_NAME);
            // Verifying that the path exists or can be created (doing so in the process)
            if (!mkdirTasks(pathToGive, true)) {
                // Throw a fatal error to console, summon a message box then exit the program
                g_errMsg.append(fatal_error).append(L"cfg directory doesn't exist and couldn't be created, aborting!\ncfg dir: ").append(pathToGive);
                errBox(g_errMsg.c_str(), true);
            }
        }
    }
    g_pathToCfg = std::move(pathToGive);
}


// Utility function for saving runtime settings to config file
bool cfgSave(void) {
    static bool firstCfgSave = true;
    SetLastError(ERROR_SUCCESS); // Reset last error
    std::ofstream cfgFile(g_pathToCfg, std::ios::binary | std::ios::out | std::ios::trunc); // Open config file in write mode
    if (!cfgFile.is_open()) { // If operation failed
        g_errMsg.clear(); // Empty g_errMsg
        char errBuf[ERRBUF_SIZE];
        const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
        g_errMsg.append(msg_error)
                .append(L"couldn't open config file in write mode, failed with code (if present): ")
                .append(errBuf, r.ptr)
                .append(L"\nThis means the app will be unable to retain settings");
        fwprintf(stderr, L"[!] %ls\n", g_errMsg.c_str());
        if (firstCfgSave) MessageBoxW(g_hMainWnd, g_errMsg.c_str(), g_errWndName.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        firstCfgSave = false;
        return false;
    }
    firstCfgSave = false;
    bool is1stSec = true;
    int lastSecId = 0;
    char stackBuf[MAX_STACK];
    // Loop that restarts for each entry in config map
    for (uint32_t i = 0; cfgMap::registry[i].secId; ++i) {
        const cfgSetting& entry = cfgMap::registry[i];
        if (entry.secId != lastSecId) { // If entering new section
            // Write section header to cfgFile
            if (!is1stSec) cfgFile.write("\r\n", 2);
            cfgFile.write("[", 1);
            cfgFile.write(entry.section.data(), entry.section.size());
            cfgFile.write("]\r\n", 3);
            lastSecId = entry.secId; // Set last section for next turn
            is1stSec = false;
        }
        const void* currValPtr = g_cfg.get(entry.secId, entry.keyId);
        // If somehow setting doesn't exist in runtime
        if (!currValPtr) {
            currValPtr = entry.defaultVal;
            g_cfg.set(entry.secId, entry.keyId, currValPtr, entry.type);
        }
        int needed_size;
        char* outBuf = stackBuf;
        if (entry.type == TYPE_WSTRING) {
            needed_size = wstring_to_UTF8(*static_cast<const std::wstring*>(currValPtr));
            if (needed_size > MAX_STACK) { outBuf = static_cast<char*>(malloc(needed_size)); }
        }
        // Write line to cfgFile
        const std::string_view val = cfgValToUTF8(currValPtr, entry.type, outBuf, (outBuf == stackBuf) ? MAX_STACK : needed_size);
        cfgFile.write(entry.cfgKey.data(), entry.cfgKey.size());
        cfgFile.write("=", 1);
        cfgFile.write(val.data(), val.size());
        cfgFile.write("\r\n", 2);
        if (outBuf != stackBuf) free(outBuf);
    }
    cfgFile.close();
    return true;
}


// Utility function for loading settings from config file to runtime, and applying default for any missing settings
bool cfgLoad(const bool createNeeded = false) {
    g_errMsg.clear(); // Empty g_errMsg
    std::unique_ptr<char[], decltype(&std::free)> fileBuf(nullptr, std::free);
    std::vector<std::string_view> lines;
    if (!createNeeded) {
        SetLastError(ERROR_SUCCESS); // Reset last error
        // Open config file in read mode
        std::ifstream cfgFile(g_pathToCfg, std::ios::binary | std::ios::in | std::ios::ate);
        if (!cfgFile.is_open()) { // If operation failed
            char errBuf[16];
            const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, GetLastError());
            g_errMsg.append(msg_error)
                    .append(L"config file was tested to have valid attributes, yet can't be read. Error code (if present): ")
                    .append(errBuf, r.ptr);
            errBox(g_errMsg.c_str());
            return false;
        }
        const size_t fileSize = static_cast<size_t>(cfgFile.tellg());
        fileBuf.reset(static_cast<char*>(std::malloc(fileSize)));
        cfgFile.seekg(0);
        cfgFile.read(fileBuf.get(), fileSize); // Read file contents
        cfgFile.close();
        uint32_t line_count = 0;
        const char* const pEnd = fileBuf.get() + fileSize;
        for (const char *p = fileBuf.get(); p < pEnd; ++p) {
            if (*p == '\n') line_count++;
        }
        lines.reserve(line_count + 1);
        // Collect in-memory positions of all lines in file contents
        std::string_view remaining(fileBuf.get(), fileSize);
        while (!remaining.empty()) {
            const size_t pos = remaining.find_first_of("\r\n");
            std::string_view currentLine = (pos == std::string_view::npos) ? remaining : remaining.substr(0, pos);
            currentLine = trim(currentLine);
            if (!currentLine.empty() && currentLine[0] != ';') { lines.push_back(currentLine); } // Ignore empty commented lines
            if (pos == std::string_view::npos) break;
            const size_t next = remaining.find_first_not_of("\r\n", pos);
            remaining = (next == std::string_view::npos) ? std::string_view{} : remaining.substr(next);
        }
    }
    bool mustResave = false;
    if (createNeeded) {
        // Loop that restarts for each entry of config map
        for (uint32_t i = 0; cfgMap::registry[i].secId; ++i) {
            const cfgSetting& entry = cfgMap::registry[i];
            g_cfg.set(entry.secId, entry.keyId, entry.defaultVal, entry.type);
        }
        mustResave = true;
    } else {
        int lastSecId = 0;
        uint32_t beginInd;
        // Loop that restarts for each entry of config map
        for (uint32_t i = 0; cfgMap::registry[i].secId; ++i) {
            const cfgSetting& entry = cfgMap::registry[i];
            bool foundAndValid = false, secMatch = false;
            std::string_view currentSecInFile; // Renew cfgFile section tracker every time we look for a new setting
            if (lastSecId != entry.secId) {
                lastSecId = entry.secId;
                beginInd = 0;
            }
            // Loop that restarts for each entry of lines (view of file content)
            for (uint32_t ind = beginInd; ind < lines.size(); ++ind) {
                std::string_view& line = lines[ind];
                if (line.empty()) continue; // Skip blank lines
                if (!secMatch) {
                    if (line[0] == '[' && line[line.size() - 1] == ']') { // If line is a section
                        currentSecInFile = line.substr(1, line.size() - 2); // Set current section
                        if (currentSecInFile == entry.section) {
                            beginInd = ind;
                            secMatch = true;
                        }
                    }
                    continue;
                } else {
                    if (line[0] == '[' && line[line.size() - 1] == ']') break; // If line is next section just leave, that setting is to be reset
                    const size_t sep = line.find('='); // Find "=" as separator
                    if (sep != std::string_view::npos && (entry.cfgKey == trim(line.substr(0, sep)))) { // Obtain key and check if it matches
                        const std::string_view rawVal = trim(line.substr(sep + 1)); // Obtain raw value
                        line = std::string_view{}; // Disqualify used config line for next cycle, purely for optimization
                        if (g_cfg.verifyAndSet(entry.secId, entry.keyId, entry.type, rawVal)) foundAndValid = true; // Attempt applying setting
                        break;
                    }
                }
            }
            if (!foundAndValid) { // Couldn't find a valid value to apply
                g_cfg.set(entry.secId, entry.keyId, entry.defaultVal, entry.type); // Apply default value
                mustResave = true;
            }
        }
    }
    return mustResave ? cfgSave() : true;
}


// Utility function for initializing config file and loading settings
bool initCfg(void) {
    g_errMsg.clear(); // Empty g_errMsg
    bool createNeeded = false;
    SetLastError(ERROR_SUCCESS); // Reset last error
    const DWORD attribs = GetFileAttributesW(g_pathToCfg.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES) { // If file couldn't be reached
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) { // If file doesn't yet exist
            fwprintf(stdout, L"[*] Config file does not yet exist, triggering creation at %ls\n", g_pathToCfg.c_str());
            createNeeded = true; // Trigger creation
        } else {
            char errBuf[ERRBUF_SIZE];
            const std::to_chars_result r = std::to_chars(errBuf, errBuf + ERRBUF_SIZE, err); // Convert the DWORD
            g_errMsg.append(fatal_error)
                    .append(L"cannot reach or create config file, aborting!\ncfg location: ")
                    .append(g_pathToCfg)
                    .append(L"\nError code: ")
                    .append(errBuf, r.ptr);
            goto fatal_error;
        }
    } else if (attribs & FILE_ATTRIBUTE_DIRECTORY) { // File is actually a dir
        g_errMsg.append(fatal_error)
                .append(L"specified path already exists as a folder, aborting!\nPlease remove from its location the folder at: ")
                .append(g_pathToCfg);
        goto fatal_error;
    } else if (attribs & FILE_ATTRIBUTE_READONLY) { // File is read-only
        g_errMsg.append(fatal_error).append(L"config file has a read-only attribute, aborting!\ncfg location: ").append(g_pathToCfg);
        goto fatal_error;
    }
    return cfgLoad(createNeeded); // Load contents of config and set fullSuccess to false if that doesn't complete without errors
    fatal_error: { // Throw a fatal error to console, summon a message box then exit the program
        errBox(g_errMsg.c_str(), true);
        return false;
    }
}
