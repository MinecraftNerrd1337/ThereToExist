#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX


enum OglThreadStates : int { // IDs for OpenGL thread states
    IDGL_STATE_STOPPED = 0, 
    IDGL_STATE_FAILED, 
    IDGL_STATE_INIT, 
    IDGL_STATE_RUNNING
};
enum OglKeyInd : int { // IDs for OpenGL thread settings
    IDGL_XCOORD = 0, 
    IDGL_YCOORD, 
    IDGL_WIDTH, 
    IDGL_HEIGHT, 
    IDGL_COLOR, 
    IDGL_FRAMETIME, 
    IDGL_SHOW, 
    IDGL_TOPMOST, 
    IDGL_HIGHPRECIS, 
    OGLIND_MAX
};


static constexpr wchar_t OGL_TOGGLE_START[] = L"Start";
static constexpr wchar_t OGL_TOGGLE_STOP[] = L"Stop";

// Make the following variables visible externally:
extern std::atomic<bool> g_OglSomethingChanged; // Changes flag for OpenGL renderer thread


// Make the following functions visible externally:
unsigned __stdcall OpenGLSpooferRenderThread(void*);
bool OglSpooferManage(const int _op = 0, const HWND _hCheck = NULL);
bool OglSettingLoad(const int _keyToLoad = 0);
bool OglSpooferInit(void);
