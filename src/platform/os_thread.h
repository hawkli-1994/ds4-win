#ifndef DS4_OS_THREAD_H
#define DS4_OS_THREAD_H

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>

typedef SRWLOCK os_mutex_t;
typedef CONDITION_VARIABLE os_cond_t;
typedef INIT_ONCE os_once_t;
typedef HANDLE os_thread_t;
typedef void *(*os_thread_fn)(void *);

#define OS_ONCE_INIT INIT_ONCE_STATIC_INIT

static inline int os_mutex_init(os_mutex_t *m) {
    InitializeSRWLock(m);
    return 0;
}
static inline void os_mutex_lock(os_mutex_t *m) { AcquireSRWLockExclusive(m); }
static inline void os_mutex_unlock(os_mutex_t *m) { ReleaseSRWLockExclusive(m); }
static inline void os_mutex_destroy(os_mutex_t *m) { (void)m; }

static inline int os_cond_init(os_cond_t *c) {
    InitializeConditionVariable(c);
    return 0;
}
static inline int os_cond_wait(os_cond_t *c, os_mutex_t *m) {
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1;
}
static inline void os_cond_signal(os_cond_t *c) { WakeConditionVariable(c); }
static inline void os_cond_broadcast(os_cond_t *c) { WakeAllConditionVariable(c); }
static inline void os_cond_destroy(os_cond_t *c) { (void)c; }

void os_once(os_once_t *once, void (*init_fn)(void));
int os_thread_create(os_thread_t *t, os_thread_fn fn, void *arg);
void os_thread_join(os_thread_t t);
void os_thread_detach(os_thread_t t);
long os_cpu_count(void);

#else
#include <pthread.h>
#include <unistd.h>

typedef pthread_mutex_t os_mutex_t;
typedef pthread_cond_t os_cond_t;
typedef pthread_once_t os_once_t;
typedef pthread_t os_thread_t;
typedef void *(*os_thread_fn)(void *);

#define OS_ONCE_INIT PTHREAD_ONCE_INIT

static inline int os_mutex_init(os_mutex_t *m) { return pthread_mutex_init(m, NULL); }
static inline void os_mutex_lock(os_mutex_t *m) { pthread_mutex_lock(m); }
static inline void os_mutex_unlock(os_mutex_t *m) { pthread_mutex_unlock(m); }
static inline void os_mutex_destroy(os_mutex_t *m) { pthread_mutex_destroy(m); }
static inline int os_cond_init(os_cond_t *c) { return pthread_cond_init(c, NULL); }
static inline int os_cond_wait(os_cond_t *c, os_mutex_t *m) { return pthread_cond_wait(c, m); }
static inline void os_cond_signal(os_cond_t *c) { pthread_cond_signal(c); }
static inline void os_cond_broadcast(os_cond_t *c) { pthread_cond_broadcast(c); }
static inline void os_cond_destroy(os_cond_t *c) { pthread_cond_destroy(c); }
static inline void os_once(os_once_t *once, void (*fn)(void)) { pthread_once(once, fn); }
static inline int os_thread_create(os_thread_t *t, os_thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
static inline void os_thread_join(os_thread_t t) { pthread_join(t, NULL); }
static inline void os_thread_detach(os_thread_t t) { pthread_detach(t); }
static inline long os_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}
#endif

#endif
