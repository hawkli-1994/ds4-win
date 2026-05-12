#include "os_file.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#include "os_path.h"

#include <stdlib.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#endif

#ifdef _WIN32
static int os_win_error_to_errno(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
        return EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return EBUSY;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_HANDLE_EOF:
        return 0;
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_FUNCTION:
        return EINVAL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    default:
        return EIO;
    }
}
#endif

void os_file_init(os_file_t *f) {
    if (!f) return;
#ifdef _WIN32
    f->h = INVALID_HANDLE_VALUE;
#else
    f->fd = -1;
#endif
}

int os_file_valid(const os_file_t *f) {
    if (!f) return 0;
#ifdef _WIN32
    return f->h != NULL && f->h != INVALID_HANDLE_VALUE;
#else
    return f->fd >= 0;
#endif
}

int os_file_open_read(os_file_t *f, const char *path_utf8) {
    if (!f || !path_utf8) {
        errno = EINVAL;
        return -1;
    }
    os_file_init(f);
#ifdef _WIN32
    wchar_t wpath[OS_MAX_PATH_W];
    if (os_utf8_to_wide(path_utf8, wpath, OS_MAX_PATH_W) <= 0) {
        errno = EINVAL;
        return -1;
    }
    HANDLE h = CreateFileW(wpath,
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL |
                               FILE_FLAG_RANDOM_ACCESS |
                               FILE_FLAG_OVERLAPPED,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = os_win_error_to_errno(GetLastError());
        return -1;
    }
    f->h = h;
    return 0;
#else
    int fd = open(path_utf8, O_RDONLY);
    if (fd < 0) return -1;
    f->fd = fd;
    return 0;
#endif
}

int os_file_dup(os_file_t *dst, const os_file_t *src) {
    if (!dst || !os_file_valid(src)) {
        errno = EINVAL;
        return -1;
    }
    os_file_init(dst);
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, src->h, self, &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        errno = os_win_error_to_errno(GetLastError());
        return -1;
    }
    dst->h = h;
    return 0;
#else
    int fd = dup(src->fd);
    if (fd < 0) return -1;
    dst->fd = fd;
    return 0;
#endif
}

FILE *os_fopen(const char *path_utf8, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[OS_MAX_PATH_W];
    wchar_t wmode[32];
    if (os_utf8_to_wide(path_utf8, wpath, OS_MAX_PATH_W) <= 0) {
        errno = EINVAL;
        return NULL;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, wmode, 32);
    if (n <= 0) {
        errno = EINVAL;
        return NULL;
    }
    FILE *fp = _wfopen(wpath, wmode);
    if (!fp) errno = os_win_error_to_errno(GetLastError());
    return fp;
#else
    return fopen(path_utf8, mode);
#endif
}

void os_file_close(os_file_t *f) {
    if (!f || !os_file_valid(f)) return;
#ifdef _WIN32
    CloseHandle(f->h);
    f->h = INVALID_HANDLE_VALUE;
#else
    close(f->fd);
    f->fd = -1;
#endif
}

uint64_t os_file_size(const os_file_t *f) {
    if (!os_file_valid(f)) {
        errno = EBADF;
        return UINT64_MAX;
    }
#ifdef _WIN32
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->h, &sz) || sz.QuadPart < 0) {
        errno = os_win_error_to_errno(GetLastError());
        return UINT64_MAX;
    }
    return (uint64_t)sz.QuadPart;
#else
    struct stat st;
    if (fstat(f->fd, &st) != 0 || st.st_size < 0) return UINT64_MAX;
    return (uint64_t)st.st_size;
#endif
}

int64_t os_pread(const os_file_t *f, void *buf, uint64_t len, uint64_t off) {
    if (!os_file_valid(f) || (!buf && len != 0)) {
        errno = EINVAL;
        return -1;
    }
    uint64_t done = 0;
#ifdef _WIN32
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev) {
        errno = os_win_error_to_errno(GetLastError());
        return -1;
    }
    while (done < len) {
        const uint64_t remaining = len - done;
        DWORD chunk = (DWORD)(remaining > 0x40000000ull ? 0x40000000ul : remaining);
        OVERLAPPED ol;
        memset(&ol, 0, sizeof(ol));
        const uint64_t cur = off + done;
        ol.Offset = (DWORD)(cur & 0xffffffffu);
        ol.OffsetHigh = (DWORD)(cur >> 32);
        ol.hEvent = ev;
        ResetEvent(ev);

        DWORD got = 0;
        BOOL ok = ReadFile(f->h, (char *)buf + done, chunk, NULL, &ol);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                ok = GetOverlappedResult(f->h, &ol, &got, TRUE);
                if (!ok) err = GetLastError();
            }
            if (!ok) {
                CloseHandle(ev);
                if (err == ERROR_HANDLE_EOF) return (int64_t)done;
                errno = os_win_error_to_errno(err);
                return -1;
            }
        } else if (!GetOverlappedResult(f->h, &ol, &got, FALSE)) {
            DWORD err = GetLastError();
            CloseHandle(ev);
            if (err == ERROR_HANDLE_EOF) return (int64_t)done;
            errno = os_win_error_to_errno(err);
            return -1;
        }
        if (got == 0) break;
        done += got;
    }
    CloseHandle(ev);
#else
    while (done < len) {
        const uint64_t remaining = len - done;
        size_t chunk = remaining > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)remaining;
        ssize_t n = pread(f->fd, (char *)buf + done, chunk, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        done += (uint64_t)n;
    }
#endif
    return (int64_t)done;
}
