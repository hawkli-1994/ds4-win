#include "os_thread.h"

#ifdef _WIN32
#include <stdlib.h>

typedef struct {
    os_thread_fn fn;
    void *arg;
} os_thread_start;

static BOOL CALLBACK os_once_trampoline(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)ctx;
    ((void (*)(void))param)();
    return TRUE;
}

void os_once(os_once_t *once, void (*init_fn)(void)) {
    InitOnceExecuteOnce(once, os_once_trampoline, (PVOID)init_fn, NULL);
}

static unsigned __stdcall os_thread_trampoline(void *arg) {
    os_thread_start *start = (os_thread_start *)arg;
    os_thread_fn fn = start->fn;
    void *fn_arg = start->arg;
    free(start);
    (void)fn(fn_arg);
    _endthreadex(0);
    return 0;
}

int os_thread_create(os_thread_t *t, os_thread_fn fn, void *arg) {
    os_thread_start *start = (os_thread_start *)malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, os_thread_trampoline, start, 0, NULL);
    if (h == 0) {
        free(start);
        return -1;
    }
    *t = (HANDLE)h;
    return 0;
}

void os_thread_join(os_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

void os_thread_detach(os_thread_t t) {
    CloseHandle(t);
}

long os_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (long)si.dwNumberOfProcessors : 1;
}
#endif
