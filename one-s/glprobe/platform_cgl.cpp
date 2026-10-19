#include "platform.h"

#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <csignal>
#include <cstdio>
#include <unistd.h>

static void on_fatal_signal(int sig)
{
   fprintf(stderr, "CRASH signal=%d\n", sig);
   fflush(stderr);
   _exit(128 + sig);
}

void platform_install_crash_handlers()
{
   signal(SIGSEGV, on_fatal_signal);
   signal(SIGBUS, on_fatal_signal);
   signal(SIGILL, on_fatal_signal);
   signal(SIGFPE, on_fatal_signal);
}

bool platform_create_context(std::string &error)
{
   const CGLPixelFormatAttribute attrs[] = {
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
      kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
      kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
      (CGLPixelFormatAttribute)0,
   };
   CGLPixelFormatObj pix = nullptr;
   GLint npix = 0;
   CGLError err = CGLChoosePixelFormat(attrs, &pix, &npix);
   if (err != kCGLNoError || !pix) {
      error = std::string("CGLChoosePixelFormat: ") + CGLErrorString(err);
      return false;
   }
   CGLContextObj ctx = nullptr;
   err = CGLCreateContext(pix, nullptr, &ctx);
   CGLReleasePixelFormat(pix);
   if (err != kCGLNoError) {
      error = std::string("CGLCreateContext: ") + CGLErrorString(err);
      return false;
   }
   err = CGLSetCurrentContext(ctx);
   if (err != kCGLNoError) {
      error = std::string("CGLSetCurrentContext: ") + CGLErrorString(err);
      return false;
   }
   return true;
}

void *platform_get_proc(const char *name)
{
   return dlsym(RTLD_DEFAULT, name);
}
