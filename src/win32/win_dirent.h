#ifndef DS4_WIN_DIRENT_H
#define DS4_WIN_DIRENT_H

#ifndef _WIN32
#error "win_dirent.h is only for Windows builds"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../platform/os_path.h"

#include <stdlib.h>
#include <wchar.h>

#ifndef NAME_MAX
#define NAME_MAX 1024
#endif

struct dirent {
    char d_name[NAME_MAX + 1];
};

typedef struct {
    HANDLE h;
    WIN32_FIND_DATAW data;
    int first;
    struct dirent ent;
} DIR;

static inline DIR *opendir(const char *path_utf8) {
    if (!path_utf8) return NULL;
    wchar_t wpath[OS_MAX_PATH_W];
    if (os_utf8_to_wide(path_utf8, wpath, OS_MAX_PATH_W) <= 0) return NULL;

    size_t len = wcslen(wpath);
    if (len + 3 >= OS_MAX_PATH_W) return NULL;
    if (len > 0 && wpath[len - 1] != L'\\') wpath[len++] = L'\\';
    wpath[len++] = L'*';
    wpath[len] = L'\0';

    DIR *d = (DIR *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->h = FindFirstFileW(wpath, &d->data);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static inline struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (d->first) {
        d->first = 0;
    } else if (!FindNextFileW(d->h, &d->data)) {
        return NULL;
    }
    if (os_wide_to_utf8(d->data.cFileName, d->ent.d_name, sizeof(d->ent.d_name)) <= 0) {
        d->ent.d_name[0] = '\0';
    }
    return &d->ent;
}

static inline int closedir(DIR *d) {
    if (!d) return -1;
    FindClose(d->h);
    free(d);
    return 0;
}

#endif
