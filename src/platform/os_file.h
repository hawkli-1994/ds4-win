#ifndef DS4_OS_FILE_H
#define DS4_OS_FILE_H

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
} os_file_t;

void os_file_init(os_file_t *f);
int os_file_open_read(os_file_t *f, const char *path_utf8);
void os_file_close(os_file_t *f);
int os_file_valid(const os_file_t *f);
uint64_t os_file_size(const os_file_t *f);
int64_t os_pread(const os_file_t *f, void *buf, uint64_t len, uint64_t off);
FILE *os_fopen(const char *path_utf8, const char *mode);

#endif
