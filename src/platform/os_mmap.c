#include "os_mmap.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#else
#include <sys/mman.h>
#endif

void os_mmap_init(os_mmap_t *m) {
    if (!m) return;
    m->addr = NULL;
    m->size = 0;
    os_file_init(&m->file);
#ifdef _WIN32
    m->hMapping = NULL;
#endif
}

int os_mmap_open(const char *path_utf8, os_mmap_t *m) {
    return os_mmap_open_read(path_utf8, 0, m);
}

int os_mmap_open_read(const char *path_utf8, int shared, os_mmap_t *m) {
    (void)shared;
    if (!m) {
        errno = EINVAL;
        return -1;
    }
    os_mmap_init(m);
    if (os_file_open_read(&m->file, path_utf8) != 0) return -1;

    const uint64_t size = os_file_size(&m->file);
    if (size == UINT64_MAX || size == 0) {
        os_file_close(&m->file);
        errno = size == 0 ? EINVAL : errno;
        return -1;
    }

#ifdef _WIN32
    HANDLE hMap = CreateFileMappingW(m->file.h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        os_file_close(&m->file);
        return -1;
    }
    void *addr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!addr) {
        CloseHandle(hMap);
        os_file_close(&m->file);
        return -1;
    }
    m->hMapping = hMap;
    m->addr = (const uint8_t *)addr;
    m->size = size;
#else
    if (size > (uint64_t)SIZE_MAX) {
        os_file_close(&m->file);
        errno = EOVERFLOW;
        return -1;
    }
    const int flags = shared ? MAP_SHARED : MAP_PRIVATE;
    void *addr = mmap(NULL, (size_t)size, PROT_READ, flags, m->file.fd, 0);
    if (addr == MAP_FAILED) {
        os_file_close(&m->file);
        return -1;
    }
    m->addr = (const uint8_t *)addr;
    m->size = size;
#endif
    return 0;
}

void os_mmap_close(os_mmap_t *m) {
    if (!m) return;
#ifdef _WIN32
    if (m->addr) UnmapViewOfFile(m->addr);
    if (m->hMapping) CloseHandle(m->hMapping);
    os_file_close(&m->file);
#else
    if (m->addr && m->size <= (uint64_t)SIZE_MAX) {
        munmap((void *)m->addr, (size_t)m->size);
    }
    os_file_close(&m->file);
#endif
    os_mmap_init(m);
}

void os_mmap_warm(const os_mmap_t *m) {
    if (!m || !m->addr || m->size == 0) return;
#ifdef _WIN32
    (void)m;
#elif defined(POSIX_MADV_WILLNEED)
    if (m->size <= (uint64_t)SIZE_MAX) {
        (void)posix_madvise((void *)m->addr, (size_t)m->size, POSIX_MADV_WILLNEED);
    }
#endif
}

void os_mmap_cold(const os_mmap_t *m, uint64_t off, uint64_t len) {
    if (!m || !m->addr || len == 0 || off >= m->size) return;
    if (len > m->size - off) len = m->size - off;
#ifdef _WIN32
    (void)m;
    (void)off;
    (void)len;
#elif defined(POSIX_MADV_DONTNEED)
    if (off <= (uint64_t)SIZE_MAX && len <= (uint64_t)SIZE_MAX) {
        (void)posix_madvise((void *)(m->addr + off), (size_t)len, POSIX_MADV_DONTNEED);
    }
#endif
}
