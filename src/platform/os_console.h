#ifndef DS4_OS_CONSOLE_H
#define DS4_OS_CONSOLE_H

#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static inline void os_console_init(void) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}
#else
static inline void os_console_init(void) {}
#endif

#endif
