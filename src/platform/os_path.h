#ifndef DS4_OS_PATH_H
#define DS4_OS_PATH_H

#include <limits.h>
#include <stddef.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>

#define OS_MAX_PATH_W 32768

static inline int os_wide_to_utf8(const wchar_t *wide, char *out, size_t out_cap) {
    if (!wide || !out || out_cap == 0) return -1;
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_cap, NULL, NULL);
    return n > 0 ? n : -1;
}

static inline int os_utf8_to_wide(const char *utf8, wchar_t *out, size_t out_cap) {
    if (!utf8 || !out || out_cap == 0 || out_cap > (size_t)INT_MAX) return -1;

    wchar_t tmp[OS_MAX_PATH_W];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, tmp, OS_MAX_PATH_W);
    if (n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        if (tmp[i] == L'/') tmp[i] = L'\\';
    }

    const int already_extended =
        (n >= 5 && tmp[0] == L'\\' && tmp[1] == L'\\' &&
         (tmp[2] == L'?' || tmp[2] == L'.') && tmp[3] == L'\\');
    const int drive_abs =
        (n >= 4 && ((tmp[0] >= L'A' && tmp[0] <= L'Z') ||
                    (tmp[0] >= L'a' && tmp[0] <= L'z')) &&
         tmp[1] == L':' && tmp[2] == L'\\');
    const int unc_abs =
        (n >= 3 && tmp[0] == L'\\' && tmp[1] == L'\\');

    if (!already_extended && n > 248 && drive_abs) {
        if ((size_t)(n + 4) > out_cap) return -2;
        wcscpy(out, L"\\\\?\\");
        wmemcpy(out + 4, tmp, (size_t)n);
        return n + 4;
    }
    if (!already_extended && n > 248 && unc_abs) {
        if ((size_t)(n + 6) > out_cap) return -2;
        wcscpy(out, L"\\\\?\\UNC\\");
        wmemcpy(out + 8, tmp + 2, (size_t)(n - 2));
        return n + 6;
    }
    if ((size_t)n > out_cap) return -2;
    wmemcpy(out, tmp, (size_t)n);
    return n;
}

#else
#define OS_MAX_PATH_W 4096
#endif

#endif
