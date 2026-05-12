#ifndef DS4_OS_RANDOM_H
#define DS4_OS_RANDOM_H

#include <stddef.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
static inline int os_random_bytes(void *dst, size_t len) {
    return BCryptGenRandom(NULL, (PUCHAR)dst, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static inline int os_random_bytes(void *dst, size_t len) {
    unsigned char *p = (unsigned char *)dst;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}
#endif

#endif
