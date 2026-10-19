#pragma once
#include <string>

// The only code that differs per OS. It never includes GL headers, so scenes compile anywhere.
void platform_install_crash_handlers();
bool platform_create_context(std::string &error);
void *platform_get_proc(const char *name);
