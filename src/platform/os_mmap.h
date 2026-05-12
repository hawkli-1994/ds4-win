#ifndef DS4_OS_MMAP_H
#define DS4_OS_MMAP_H

#include <stdint.h>

#include "os_file.h"

typedef struct {
    const uint8_t *addr;
    uint64_t size;
    os_file_t file;
#ifdef _WIN32
    HANDLE hMapping;
#endif
} os_mmap_t;

void os_mmap_init(os_mmap_t *m);
int os_mmap_open(const char *path_utf8, os_mmap_t *m);
int os_mmap_open_read(const char *path_utf8, int shared, os_mmap_t *m);
void os_mmap_close(os_mmap_t *m);
void os_mmap_warm(const os_mmap_t *m);
void os_mmap_cold(const os_mmap_t *m, uint64_t off, uint64_t len);

#endif
