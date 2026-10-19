#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/glcorearb.h>
#include <GL/wglext.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
   DWORD code = ep->ExceptionRecord->ExceptionCode;
   uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
   HMODULE mod = nullptr;
   char path[MAX_PATH] = "?";
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)addr, &mod))
      GetModuleFileNameA(mod, path, MAX_PATH);
   const char *base = strrchr(path, '\\');
   base = base ? base + 1 : path;
   fprintf(stderr, "CRASH code=0x%08X at %s+0x%llx\n", (unsigned)code, base,
           (unsigned long long)(addr - (uintptr_t)mod));
   fflush(stderr);
   TerminateProcess(GetCurrentProcess(), code);
   return EXCEPTION_EXECUTE_HANDLER;
}

void platform_install_crash_handlers()
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
   _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
   _set_error_mode(_OUT_TO_STDERR);
   SetUnhandledExceptionFilter(crash_filter);
}

bool platform_create_context(std::string &error)
{
   HINSTANCE inst = GetModuleHandleA(nullptr);
   WNDCLASSA wc = {};
   wc.style = CS_OWNDC;
   wc.lpfnWndProc = DefWindowProcA;
   wc.hInstance = inst;
   wc.lpszClassName = "glprobe";
   RegisterClassA(&wc);
   // Never shown: the window exists only to own a DC with a pixel format.
   HWND hwnd = CreateWindowExA(0, "glprobe", "glprobe", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                               nullptr, nullptr, inst, nullptr);
   if (!hwnd) {
      error = "CreateWindowEx failed, error " + std::to_string(GetLastError());
      return false;
   }
   HDC dc = GetDC(hwnd);
   PIXELFORMATDESCRIPTOR pfd = {};
   pfd.nSize = sizeof(pfd);
   pfd.nVersion = 1;
   pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
   pfd.iPixelType = PFD_TYPE_RGBA;
   pfd.cColorBits = 32;
   pfd.cAlphaBits = 8;
   pfd.cDepthBits = 24;
   pfd.cStencilBits = 8;
   int pf = ChoosePixelFormat(dc, &pfd);
   if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
      error = "ChoosePixelFormat/SetPixelFormat failed, error " + std::to_string(GetLastError());
      return false;
   }
   HGLRC dummy = wglCreateContext(dc);
   if (!dummy || !wglMakeCurrent(dc, dummy)) {
      error = "legacy wglCreateContext failed, error " + std::to_string(GetLastError());
      return false;
   }
   auto create = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
   if (!create) {
      error = "wglCreateContextAttribsARB not available";
      return false;
   }
   const int attribs[] = {
      WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
      WGL_CONTEXT_MINOR_VERSION_ARB, 0,
      WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
      0,
   };
   HGLRC ctx = create(dc, nullptr, attribs);
   DWORD create_error = GetLastError();
   wglMakeCurrent(nullptr, nullptr);
   wglDeleteContext(dummy);
   if (!ctx) {
      error = "wglCreateContextAttribsARB(4.0 core) failed, error " + std::to_string(create_error);
      return false;
   }
   if (!wglMakeCurrent(dc, ctx)) {
      error = "wglMakeCurrent(4.0 core) failed, error " + std::to_string(GetLastError());
      return false;
   }
   return true;
}

void *platform_get_proc(const char *name)
{
   PROC p = wglGetProcAddress(name);
   intptr_t v = (intptr_t)p;
   // wglGetProcAddress returns these sentinels instead of null for OpenGL 1.1 entry points.
   if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1)
      p = GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
   return (void *)p;
}
