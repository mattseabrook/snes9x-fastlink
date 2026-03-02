#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
static void VDbg(const char *fmt, ...)
{
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "[SNES9X-VULKAN-DBG] ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

