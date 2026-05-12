# ds4 Windows CUDA 移植技术方案

## 一、总体策略

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 编译器 | MSVC 2022 + CUDA 12.x | Windows CUDA 标准栈 |
| 构建系统 | 新增 `CMakeLists.txt`，保留 Linux Makefile | CMake 原生支持 CUDA 和 MSVC |
| 线程 | `pthread-win32` (MinGW) 或 C11 `<threads.h>` 薄封装 | 避免全量改写 200+ 行线程调用 |
| mmap | 自建 `os_mmap.c` 抽象层 | 将 `mmap/munmap/madvise` 封装为 3 个跨平台函数 |
| 网络 | `#ifdef _WIN32` + Winsock2 | 改动集中且机械 |
| KV cache 磁盘 | 跳过 `O_DIRECT`，走带缓冲 I/O | 已有 fallback 路径，零性能影响 |
| 交互 CLI | 首版砍掉 `linenoise`，只保留 server 模式 | 砍掉 400 行最大改动量 |

---

## 二、新建/修改文件清单

```
新增:
  src/platform/
    os_mmap.h            — mmap 抽象接口 (~20行)
    os_mmap.c            — Linux/Win 双实现 (~80行)
    os_thread.h          — 线程/互斥/条件变量 薄封装 (~60行)
    os_clock.h           — 高精度时钟封装 (~15行)
    os_random.h          — 随机数封装 (~10行)
  CMakeLists.txt         — MSVC+CUDA 构建 (~80行)
  src/win32/
    win_dirent.h         — dirent 兼容头(opendir/readdir/closedir) (~60行)

修改:
  ds4.c                  — mmap→os_mmap, pthread→os_thread, clock→os_clock
  ds4_cuda.cu            — sysconf→GetSystemInfo, pread→ReadFile, 跳过O_DIRECT
  ds4_server.c           — socket→Winsock, pthread→os_thread, /dev/urandom→os_random
  ds4_cli.c              — clock→os_clock, getpid→GetCurrentProcessId, HOME→USERPROFILE

删除/禁用:
  linenoise.c/h          — 首版不编译 (ds4_cli.c 中 #ifndef _WIN32 包裹)
```

---

## 三、分模块方案

### 3.1 构建系统 — `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(ds4 LANGUAGES C CXX CUDA)

# CUDA 后端
add_library(ds4_cuda STATIC ds4_cuda.cu)
target_compile_features(ds4_cuda PRIVATE cxx_std_17)
set_target_properties(ds4_cuda PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
target_compile_options(ds4_cuda PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-arch=sm_80>)

# 平台抽象层
add_library(ds4_platform STATIC
    src/platform/os_mmap.c
    src/platform/os_thread.c
    src/platform/os_clock.c
    src/platform/os_random.c
)

# 主库
add_library(ds4 STATIC ds4.c rax.c)
target_link_libraries(ds4 PUBLIC ds4_platform)
target_compile_definitions(ds4 PRIVATE DS4_CUDA)

# Server
add_executable(ds4_server ds4_server.c)
target_link_libraries(ds4_server ds4 ds4_cuda ws2_32 bcrypt)

# CLI (可选，首版可砍)
if(NOT WIN32)
    add_executable(ds4_cli ds4_cli.c linenoise.c)
    target_link_libraries(ds4_cli ds4 ds4_cuda)
endif()
```

依赖：`CUDA Toolkit 12.x` + `Visual Studio 2022 Build Tools`。

### 3.2 mmap 抽象层 — `os_mmap.h / os_mmap.c`

核心接口只暴露 4 个函数，把 Linux/Windows 差异全部收敛到实现文件：

```c
// os_mmap.h
typedef struct {
    void   *addr;       // 映射后的首地址
    size_t  size;       // 文件大小
#ifdef _WIN32
    HANDLE  hFile;
    HANDLE  hMapping;
#else
    int     fd;
#endif
} os_mmap_t;

int  os_mmap_open (const char *path, os_mmap_t *m);   // 打开+映射
void os_mmap_close(os_mmap_t *m);                      // 解映射+关闭
void os_mmap_warm (os_mmap_t *m);                      // 预触碰所有页(madvise WILLNEED/PrefetchVirtualMemory)
void os_mmap_cold (os_mmap_t *m, size_t off, size_t len); // 释放指定页(madvise DONTNEED/DiscardVirtualMemory)
```

**Linux 实现**：`open + fstat + mmap(MAP_SHARED) + posix_madvise`

**Windows 实现**：
```
CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
    → GetFileSizeEx → 
CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL)
    → MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0)
```

**ds4.c 中对 `model->map`, `model->fd`, `model->map_size` 的引用全部改为 `model->mmap.addr`, `model->mmap.size`。** 改动约 30 处，机械替换。

### 3.3 CUDA 后端 — `ds4_cuda.cu`

共 5 类改动，均已在现有代码中留有 `#if defined(__linux__)` 分支：

**a) 页面大小**（4处）

```c
// 前:
long ps = sysconf(_SC_PAGESIZE);
// 后:
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si); long ps = si.dwPageSize;
#else
    long ps = sysconf(_SC_PAGESIZE);
#endif
```

**b) 文件预读 (`pread`) → `ReadFile`**（2处，行713、1295）

```c
#ifdef _WIN32
    OVERLAPPED ol = { .Offset = offset & 0xFFFFFFFF, .OffsetHigh = offset >> 32 };
    DWORD n;
    ReadFile(hFile, buf, len, &n, &ol);
#else
    ssize_t n = pread(fd, buf, len, offset);
#endif
```

**c) O_DIRECT 路径** — 全部跳过。现有 `#if defined(__linux__) && defined(O_DIRECT)` 分支在 Windows 下自然走到 `#else` 的普通 `pread`。**零改动**。

**d) madvise/fadvise**（4处）— 无操作宏：

```c
#ifdef _WIN32
    // No equivalent on Windows; pages already freed on UnmapViewOfFile
#else
    posix_madvise(ptr, len, POSIX_MADV_DONTNEED);
#endif
```

**e) cudaHostRegister on mmap pages** — 关键决策：Windows WDDM 下 `cudaHostRegister(MapViewOfFile)` 不稳定。**强制走 `cuda_model_copy_chunked()` 路径**（ds4_cuda.cu:927-1012已有完整实现），权重用 `cudaMallocManaged` 或 `cudaMalloc` + `cudaMemcpy` 搬进显存。改动约 15 行，在 `ds4_gpu_set_model_map()` 入口加平台判断。

### 3.4 线程 — `os_thread.h`

为现有代码中约 30 处 pthread 调用提供薄封装，避免改散落各文件的锁逻辑：

```c
// os_thread.h — 直接做 typedef + 宏，零运行时开销
#ifdef _WIN32
  #include <windows.h>
  typedef CRITICAL_SECTION os_mutex_t;
  #define os_mutex_init(m)    InitializeCriticalSection(m)
  #define os_mutex_lock(m)    EnterCriticalSection(m)
  #define os_mutex_unlock(m)  LeaveCriticalSection(m)
  #define os_mutex_destroy(m) DeleteCriticalSection(m)

  typedef CONDITION_VARIABLE os_cond_t;
  #define os_cond_init(c)     InitializeConditionVariable(c)
  #define os_cond_wait(c,m)   SleepConditionVariableCS(c,m,INFINITE)
  #define os_cond_signal(c)   WakeConditionVariable(c)
  #define os_cond_broadcast(c) WakeAllConditionVariable(c)

  // 线程创建/join 用更轻量的 _beginthreadex 或 CreateThread
  // 线程局部存储: __declspec(thread) 替代 __thread
#else
  typedef pthread_mutex_t os_mutex_t;
  typedef pthread_cond_t  os_cond_t;
  // ...pthread 原生调用
#endif
```

`ds4.c` 和 `ds4_server.c` 中将 `pthread_*` 替换为 `os_*`，约 70 处替换。

### 3.5 网络 — `ds4_server.c`

改动集中在文件头部和 socket 创建/关闭处，约 12 处：

```c
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")

  #define socklen_t        int
  #define SHUT_RDWR        SD_BOTH
  #define close_socket(s)  closesocket(s)
  static int wsock_init() { WSADATA d; return WSAStartup(MAKEWORD(2,2), &d); }
#else
  // 原生 BSD socket
  #define close_socket(s)  close(s)
#endif
```

`poll()` 用 `WSAPoll()` 替代（Windows Vista+ 原生支持）。**注意**：Windows 的 `SOCKET` 是 `UINT_PTR` 而非 `int`，但 `WSAPoll` 接受 `SOCKET`。现有代码中 `int fd` 类型需改为 `socket_t` typedef。

### 3.6 杂项平台调用

| 文件 | 原调用 | Windows 替换 | 处数 |
|------|--------|-------------|------|
| ds4.c | `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter` + `QueryPerformanceFrequency` | 4 |
| ds4.c | `__thread` | `__declspec(thread)` | 3 |
| ds4.c | `flock` | `LockFileEx` 或直接 `CreateFileW(dwShareMode=0)` | 1 |
| ds4_server.c | `/dev/urandom` + `read` | `BCryptGenRandom` | 2 |
| ds4_server.c | `getpid()` | `GetCurrentProcessId()` | 3 |
| ds4_server.c | `stat()` | `_stat64()` | 2 |
| ds4_server.c | `dirent.h` (`opendir`等) | `FindFirstFileW`/`FindNextFileW` | 2 |
| ds4_cli.c | `getenv("HOME")` | `getenv("USERPROFILE")` | 1 |
| ds4_cli.c | `sigaction` | `signal()` | 1 |

### 3.7 linenoise — 砍掉

```c
// ds4_cli.c
#ifdef _WIN32
  int main(int argc, char **argv) {
      fprintf(stderr, "Interactive CLI not supported on Windows. "
              "Use ds4_server.exe --help\n");
      return 1;
  }
#else
  // 原有 CLI 代码
#endif
```

首版只提供 `ds4_server.exe`（HTTP API 服务），CLI 后续单独补。

---

## 四、文件改动量总览

| 文件 | 类型 | 大约行数 | 难度 |
|------|------|---------|------|
| `CMakeLists.txt` | **新增** | ~80 | 低 |
| `src/platform/os_mmap.h` | **新增** | ~20 | 低 |
| `src/platform/os_mmap.c` | **新增** | ~80 | 中 |
| `src/platform/os_thread.h` | **新增** | ~60 | 低 |
| `src/platform/os_clock.h` | **新增** | ~20 | 低 |
| `src/platform/os_random.h` | **新增** | ~15 | 低 |
| `src/win32/win_dirent.h` | **新增** | ~60 | 低 |
| `ds4.c` | 修改 | ~80 | 中 |
| `ds4_cuda.cu` | 修改 | ~60 | 中 |
| `ds4_server.c` | 修改 | ~100 | 中 |
| `ds4_cli.c` | 修改 | ~25 | 低 |
| `linenoise.c/h` | **不编译** | 0 | — |
| **合计新增** | | **~335** | |
| **合计修改** | | **~265** | |
| **总计** | | **~600** | |

---

## 五、实施步骤（5 天）

| 天 | 任务 | 产出 |
|----|------|------|
| **D1** | 搭建 CMake + MSVC + CUDA 构建；新增 `os_mmap`，改 `ds4.c` 中所有 mmap 调用 | ds4.lib 可编译 |
| **D2** | 新增 `os_thread` / `os_clock` / `os_random`；改 `ds4.c` / `ds4_server.c` 线程调用 | ds4_server.lib 可编译 |
| **D3** | 改 `ds4_server.c` Winsock + dirent + stat；改 `ds4_cuda.cu` 平台调用 | ds4_cuda.lib 可编译 |
| **D4** | 改 `ds4_cli.c` 平台调用；砍掉 linenoise；链接全量 | ds4_server.exe 可链接 |
| **D5** | 在 Windows+CUDA 环境上真机编译、跑通模型加载→一轮推理 | 第一个 token 生成 |

---

## 六、风险与对策

| 风险 | 概率 | 对策 |
|------|------|------|
| `MapViewOfFile` 48bit VA 不够存 80GB 文件 | 低 | `CreateFileMapping` 支持最大 16TB；逐段 `MapViewOfFile` 分片映射 |
| MSVC 对 C99/C11 特性不兼容（如 VLA、designated initializer） | 中 | ds4.c 已非常克制使用 C99 特性，主要用 `{0}` 零初始化。检查 `-std=c11` 等效 `/std:c11` |
| NVCC 与 MSVC 版本兼容 | 低 | CUDA 12.x 官方支持 VS 2022 17.x |
| 双 GPU 设备选择逻辑 | 低 | CUDA Runtime API `cudaSetDevice`/`cudaGetDeviceCount` 是跨平台的，无需改动 |
