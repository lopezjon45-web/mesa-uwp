/* Platform shim so ShaderBinary builds outside the WDK (MSVC without strsafe, and Linux). */
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <strsafe.h>
#else
#include <wsl/winadapter.h>
#include <string.h>
#include <stdio.h>
typedef char *LPSTR;
typedef const char *LPCSTR;
#ifndef ZeroMemory
#define ZeroMemory(p, n) memset((p), 0, (n))
#endif
#ifndef __out_ecount
#define __out_ecount(n)
#endif
#ifndef __in_ecount
#define __in_ecount(n)
#endif
static inline HRESULT StringCchCopyA(char *dst, size_t n, const char *src)
{
   if (!n) return E_INVALIDARG;
   strncpy(dst, src, n - 1);
   dst[n - 1] = 0;
   return S_OK;
}
#endif
#include <directx/D3D12TokenizedProgramFormat.hpp>
