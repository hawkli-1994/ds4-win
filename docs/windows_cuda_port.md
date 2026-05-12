# ds4 Windows CUDA 移植技术方案

> 状态：技术方案 / 实施前。代码尚未实现，本文档作为 Windows + CUDA 12.x 端口的执行计划。

## 一、总体策略

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 编译器 | MSVC 2022 (17.x) + CUDA 12.x | Windows CUDA 标准栈；CUDA 12.x 官方支持 VS 2022 |
| 构建系统 | 新增 `CMakeLists.txt` (≥3.24)，保留 Linux Makefile | CMake 3.24 支持 `CUDA_ARCHITECTURES native`；`find_package(CUDAToolkit)` 统一管理 cudart/cuBLAS |
| 互斥 | `SRWLOCK`（默认）+ `CRITICAL_SECTION`（仅在确认需要重入时） | `pthread_mutex_t` 默认非递归；CRITICAL_SECTION 是递归的，会掩盖重入 bug。SRWLOCK 是更精确的非递归对等物 |
| 条件变量 | `CONDITION_VARIABLE` + `SleepConditionVariableSRW` | 与 SRWLOCK 配套 |
| 一次性初始化 | `INIT_ONCE` + `InitOnceExecuteOnce` | 替换 `pthread_once`（签名差异较大，不可宏化为一行） |
| 线程创建 | `_beginthreadex` + `_endthreadex` | 比 `CreateThread` 多做 CRT TLS 初始化；返回的 HANDLE 调用方负责 `CloseHandle` |
| 线程局部存储 | `_Thread_local` (C11) | 替换 `__thread`；比 `__declspec(thread)` 在静态/动态链接下行为更可预测 |
| ❌ 不选 | `pthread-win32`、C11 `<threads.h>` | 前者引入第三方依赖；后者 MSVC 历史支持不全。统一直接走 Win32 包装 |
| 文件句柄 | 新增 `os_file_t`（含 `HANDLE`/`int fd`） + `os_pread` | 不能把 Windows `HANDLE` 强塞进 `int fd`；现有 `ds4_gpu_set_model_fd(int)` API 必须改造 |
| mmap | 自建 `os_mmap.c` 抽象层 | 把 `mmap/munmap/madvise` 收敛为 4 个跨平台函数，并在接口里直接处理 UTF-8 路径 |
| 网络 | `#ifdef _WIN32` + Winsock2 + WSAPoll | 注意 SOCKET 是 `UINT_PTR` 不是 `int`；socket 错误走 `WSAGetLastError()` 不是 `errno` |
| 控制台与文本 IO | UTF-8 stdout + `SetConsoleOutputCP(CP_UTF8)` + `_IONBF` | 推理 server 流式输出 token，行缓冲会被管道吞掉；中文/Emoji 必须 UTF-8 |
| 信号 | `SetConsoleCtrlHandler` | Windows 上 `signal(SIGINT, …)` 的回调在独立线程跑，`SIGTERM` 不会被自然触发，不能直接照搬 |
| 路径编码 | 统一 UTF-8 入口，内部 `MultiByteToWideChar(CP_UTF8, …)` 转 UTF-16 喂给 Win32 W-API | 中文用户路径常见；CreateFileA / `_stat` 走 ANSI 代码页，无法承载 Unicode |
| 文件大小 | `_stat64` / `_fstat64` + `struct __stat64` | MSVC 默认 `struct stat::st_size` 是 32-bit，80GB 模型直接溢出 |
| KV cache 磁盘 | v1 走 buffered I/O，跳过 `O_DIRECT` 等价路径；性能与 working set 列入验收指标 | 不预先断言"零性能影响"。若实测下 page cache 压力显著，再评估 `FILE_FLAG_NO_BUFFERING` |
| 随机数 | `BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)` | 替换 `/dev/urandom` |
| CUDA 内存模型 | v1 仅走 `cudaMalloc` + staged `cudaMemcpy`（已有 `cuda_model_copy_chunked` 路径） | WDDM 下 mmap host pointer direct GPU access / HMM prefetch 不可用；`cudaMallocManaged` 与 `cudaHostRegisterReadOnly` 作为后续可选实验 |
| 交互 CLI | v1 只构建 `ds4_server.exe`；one-shot / diagnostics 可保留为后续扩展 | linenoise 是唯一硬阻塞（依赖 termios），整个 `ds4_cli.c` 在 Windows 上不参与构建（详见 §3.8） |

---

## 二、新建/修改文件清单

```
新增:
  src/platform/
    os_file.h            — 文件句柄抽象 + os_pread (~30行)
    os_file.c            — Linux/Win 双实现，含 DWORD 分块读 (~80行)
    os_mmap.h            — mmap 抽象接口 (~25行)
    os_mmap.c            — Linux/Win 双实现（含 UTF-8→UTF-16 路径转换） (~110行)
    os_thread.h          — 线程/互斥/条件变量/once 薄封装 (~80行)
    os_thread.c          — Windows 一次性初始化包装（InitOnceExecuteOnce 回调适配） (~30行)
    os_clock.h           — 高精度时钟封装 (~20行)
    os_random.h          — 随机数封装 (~15行)
    os_path.h            — UTF-8 ↔ UTF-16 路径转换辅助 (~25行)
    os_console.h         — Windows UTF-8 stdout / Ctrl 处理器 (~30行)
  CMakeLists.txt         — MSVC+CUDA 构建（含 cudart/cuBLAS 链接） (~100行)
  src/win32/
    win_dirent.h         — dirent 兼容头（UTF-16 → UTF-8 文件名转换） (~90行)

修改:
  ds4_gpu.h              — ds4_gpu_set_model_fd(int) → ds4_gpu_set_model_file(os_file_t*)
  ds4.c                  — mmap→os_mmap, pthread→os_thread, fstat→_fstat64, clock→os_clock, __thread→_Thread_local
  ds4_cuda.cu            — sysconf→GetSystemInfo, pread→os_pread, 跳过 O_DIRECT，关掉 cudaHostRegister 路径并默认走 chunked copy
  ds4_server.c           — socket→Winsock + socket_t + WSAGetLastError, pthread→os_thread, /dev/urandom→BCryptGenRandom,
                            signal→SetConsoleCtrlHandler, stat→_stat64, opendir/dirent→win_dirent

Windows 构建中不参与编译:
  linenoise.c/h          — 依赖 termios，无 Windows 等价
  ds4_cli.c              — v1 不构建 ds4.exe；后续如要保留 one-shot/diagnostics 单独立项
```

---

## 三、分模块方案

### 3.1 构建系统 — `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.24)
project(ds4 LANGUAGES C CXX CUDA)

# CUDA 架构旋钮：与 Makefile 的 CUDA_ARCH 同口径，不再硬编码 sm_80。
# 默认值覆盖 Ampere/Ada/Hopper/Blackwell 常见 SM。
# 单卡开发可 -DDS4_CUDA_ARCHITECTURES=native（CMake 3.24+）。
set(DS4_CUDA_ARCHITECTURES "80;86;89;90" CACHE STRING
    "CUDA architectures (semicolon-separated SM list, or 'native')")

# 统一拉取 cudart / cuBLAS。MSVC 不会像 nvcc 那样隐式链接，必须显式给出。
find_package(CUDAToolkit REQUIRED)

# 平台抽象层
add_library(ds4_platform STATIC
    src/platform/os_file.c
    src/platform/os_mmap.c
    src/platform/os_thread.c
    src/platform/os_clock.c
    src/platform/os_random.c
)
target_include_directories(ds4_platform PUBLIC src/platform src/win32)

# CUDA 后端
add_library(ds4_cuda STATIC ds4_cuda.cu)
target_compile_features(ds4_cuda PRIVATE cxx_std_17)
set_target_properties(ds4_cuda PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "${DS4_CUDA_ARCHITECTURES}")

# 主库
add_library(ds4 STATIC ds4.c rax.c)
target_link_libraries(ds4 PUBLIC ds4_platform)
target_compile_definitions(ds4 PRIVATE DS4_CUDA)

# Server
add_executable(ds4_server ds4_server.c)
target_link_libraries(ds4_server PRIVATE
    ds4 ds4_cuda
    CUDA::cudart CUDA::cublas        # 关键：MSVC 必须显式链
    ws2_32 bcrypt                    # Winsock2 + BCryptGenRandom
)

# CLI 首版只在非 Windows 上编译（linenoise + ds4_cli.c 不参与 Windows 构建）
if(NOT WIN32)
    add_executable(ds4_cli ds4_cli.c linenoise.c)
    target_link_libraries(ds4_cli PRIVATE ds4 ds4_cuda CUDA::cudart CUDA::cublas)
endif()

# 测试：把 tests/ 接入 CTest，便于 D7 验证
enable_testing()
add_subdirectory(tests)
```

依赖：`CUDA Toolkit 12.x` + `Visual Studio 2022 Build Tools 17.x`。

### 3.2 文件句柄抽象 — `os_file.h`（新增，关键）

现有 `ds4_gpu.h` 暴露的 `ds4_gpu_set_model_fd(int fd)` 在 Windows 上无法直接迁移——Windows 文件 handle 是 `HANDLE` (void*)，不是 `int`。若仅在 `os_mmap_t` 里藏一个 `HANDLE`，CUDA 后端里 fd cache、O_DIRECT 兜底、`pread` 路径都会变成局部补丁。

统一抽象一个文件源：

```c
// os_file.h
typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
} os_file_t;

int  os_file_open_read (os_file_t *f, const char *path_utf8);
void os_file_close     (os_file_t *f);

// 跨平台 pread。Windows 实现内部处理 DWORD 分块、EOF、GetLastError 映射。
// 返回实际读取字节数，<0 表示错误。
int64_t os_pread(os_file_t *f, void *buf, uint64_t len, uint64_t off);

// 文件大小。失败返回 UINT64_MAX（用 errno / GetLastError 拿具体错误）。
// 80GB 模型必须保证返回值是 uint64_t 而非 32-bit off_t。
// Linux: fstat(f->fd, &st) -> (uint64_t)st.st_size
// Windows: _fstat64(_open_osfhandle((intptr_t)f->h, _O_RDONLY), &st64) -> (uint64_t)st64.st_size
//          或更直接：GetFileSizeEx(f->h, &li) -> (uint64_t)li.QuadPart
uint64_t os_file_size(const os_file_t *f);
```

`os_mmap_open` 内部调 `os_file_size` 拿尺寸，避免在 `os_mmap.c` 里重复一份 `_fstat64` / `fstat`。

`ds4_gpu.h` API 改造：

```c
// 旧：
int ds4_gpu_set_model_fd(int fd);
// 新：
int ds4_gpu_set_model_file(const os_file_t *f);
```

CUDA 后端所有 `pread(fd, …)`、`fstat(fd, …)`、fd cache 全部走 `os_file_t` + `os_pread`，不在 `ds4_cuda.cu` 里散落 `HANDLE` 特判。

### 3.3 mmap 抽象层 — `os_mmap.h / os_mmap.c`

```c
// os_mmap.h
typedef struct {
    void   *addr;
    size_t  size;
#ifdef _WIN32
    HANDLE  hFile;
    HANDLE  hMapping;
#else
    int     fd;
#endif
} os_mmap_t;

int  os_mmap_open (const char *path_utf8, os_mmap_t *m);
void os_mmap_close(os_mmap_t *m);
void os_mmap_warm (os_mmap_t *m);
void os_mmap_cold (os_mmap_t *m, size_t off, size_t len);
```

**Linux 实现**：`open + fstat + mmap(MAP_PRIVATE 或 MAP_SHARED) + posix_madvise`。`ds4.c:1220` 在非 Metal 路径上用的是 `MAP_PRIVATE`，需要保留这一语义。

**Windows 实现**：
```c
wchar_t wpath[MAX_PATH_W];   // os_utf8_to_wide(path_utf8, wpath, ...)
HANDLE hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
LARGE_INTEGER sz; GetFileSizeEx(hFile, &sz);
HANDLE hMap  = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
void *addr   = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
```

UTF-8 → UTF-16 抽到 `os_path.h::os_utf8_to_wide(const char*, wchar_t*, size_t)`。

**长路径处理（v1 实施）**：`os_utf8_to_wide` 内部对 UTF-16 长度 > 248 字符的绝对路径自动加 `\\?\` 前缀（保留 12 字符冗余给文件名扩展）：

```c
// os_path.h
int os_utf8_to_wide(const char *utf8, wchar_t *out, size_t out_cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return -1;

    // 绝对路径且长度逼近 MAX_PATH 时加 \\?\ 前缀
    int needs_prefix = (n > 248)
                    && (utf8[0] != '\\' || utf8[1] != '\\')   // 不重复加
                    && ((utf8[1] == ':') || (utf8[0] == '/'));

    if (needs_prefix) {
        if ((size_t)(n + 4) > out_cap) return -2;
        wcscpy(out, L"\\\\?\\");
        return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out + 4, (int)(out_cap - 4));
    }
    if ((size_t)n > out_cap) return -2;
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)out_cap);
}
```

注意 `\\?\` 前缀会禁用 `/` → `\` 规范化，因此调用方传入的 UTF-8 路径必须已经是 Windows 形式（或在 `os_path.h` 里再加一个 `os_normalize_slashes`）。模型路径 / KV cache 目录在配置阶段统一规范化一次即可。`MAX_PATH_W` 取 32768（Windows 单段最大路径）。

**`os_mmap_warm`**：Windows 用 `PrefetchVirtualMemory`（Win8+），Linux 用 `posix_madvise(WILLNEED)`。

**`os_mmap_cold` 关键决策——Windows v1 实现为 no-op：**

```c
void os_mmap_cold(os_mmap_t *m, size_t off, size_t len) {
#ifdef _WIN32
    (void)m; (void)off; (void)len;
    // v1: no-op。
    // 不使用 DiscardVirtualMemory：MSDN 明确该 API 让内容变为 undefined，
    // 且内存保护必须是 PAGE_READWRITE；我们的映射是 PAGE_READONLY，会直接失败。
    // 如确实需要释放工作集，后续走 "分段 MapViewOfFile + UnmapViewOfFile + remap" 模型，
    // 而不是 DiscardVirtualMemory。
#else
    posix_madvise((char *)m->addr + off, len, POSIX_MADV_DONTNEED);
#endif
}
```

**ds4.c 中对 `model->map`, `model->fd`, `model->map_size` 的引用全部改为 `model->mmap.addr`, `model->mmap.size`。** 改动约 30 处，机械替换。

### 3.4 CUDA 后端 — `ds4_cuda.cu`

共 5 类改动。前 4 类已经在现有代码中留有 `#if defined(__linux__)` 分支，第 5 类是 Windows 特有的内存模型策略切换。

**a) 页面大小**（3 处：`ds4_cuda.cu:210`, `:554`, `:625`）

```c
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si); long ps = si.dwPageSize;
#else
    long ps = sysconf(_SC_PAGESIZE);
#endif
```

**b) 文件预读 (`pread`) → `os_pread`**（2 处，`ds4_cuda.cu:713`、`:1295`）

直接调用 `os_pread(&os_file, buf, len, off)`，不在 `.cu` 里手写 OVERLAPPED。原因：

- `ds4_cuda.cu` 是 NVCC + MSVC 的 C++17 host code，C99 designated initializer (`{ .Offset = … }`) 在 NVCC + MSVC C++ 模式下不可靠
- `ReadFile` 的 `nNumberOfBytesToRead` 是 `DWORD`（32-bit），单次最多 4GiB；权重预读必须分块
- 同步 handle 上的 `OVERLAPPED` 语义会更新文件指针，与异步 handle 不一致——需要在 helper 内固化

`os_file.c` 的 Windows 实现里把这些细节封装一次：

```c
int64_t os_pread(os_file_t *f, void *buf, uint64_t len, uint64_t off) {
    uint64_t done = 0;
    while (done < len) {
        DWORD chunk = (DWORD)((len - done > (uint64_t)0x40000000)
                              ? 0x40000000 : (len - done));  // 1GiB 分块
        OVERLAPPED ol = {0};
        const uint64_t cur = off + done;
        ol.Offset     = (DWORD)(cur & 0xFFFFFFFFu);
        ol.OffsetHigh = (DWORD)(cur >> 32);
        DWORD got = 0;
        if (!ReadFile(f->h, (char *)buf + done, chunk, &got, &ol)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            return -(int64_t)e;
        }
        if (got == 0) break;          // EOF
        done += got;
    }
    return (int64_t)done;
}
```

CUDA 后端只调 `os_pread`，不感知 platform。

**c) O_DIRECT 路径** — 全部跳过。现有 `#if defined(__linux__) && defined(O_DIRECT)` 分支在 Windows 下自然走 `#else` 的 `os_pread`。

**d) madvise/fadvise**（4 处）— 走 `os_mmap_cold` (Windows 上 no-op)。`cudaMemAdvise` / `cudaMemPrefetchAsync` 不在 v1 启用（与 unified memory 策略一致，见 e）。

**e) Windows CUDA 内存模型——v1 明确策略**

WDDM 驱动（Windows 桌面 GPU 全部使用 WDDM，仅数据中心 Tesla/A100/H100 在 Windows Server 可切 TCC）下，以下路径都不可靠或不可用：

- `cudaHostRegister(MapViewOfFile, …)` — 对文件映射视图返回 `cudaErrorInvalidValue`，确定性失败
- Linux HMM / ATS 风格的"系统内存指针直接给 GPU 访问"
- `cudaMemPrefetchAsync` 用于 mmap host pointer

NVIDIA CUDA 文档明确：Windows 上 managed memory 需通过 `cudaMallocManaged` 显式分配，Windows unified memory 支持有限。

**v1 策略**：

| 路径 | Windows v1 | 备注 |
|------|-----------|------|
| `cudaMalloc` + chunked `cudaMemcpy`（staged via `cudaMallocHost`） | ✅ **默认** | 走已有 `cuda_model_copy_chunked` (`ds4_cuda.cu:927-1012`) |
| `cudaHostRegister(mmap_view, …)` | ❌ 三处分支全 `#ifdef` 关闭 | `:217` `:1251` `:1272` |
| `cudaMallocManaged` | ⚠️ 可选实验，env 旋钮启用 | 后续 benchmark 决定 |
| `cudaHostRegisterReadOnly` | ⚠️ 后续评估 | 必须先查设备属性，且只能用于自己分配的 buffer，不能用于 mmap view |

具体代码改动：

1. **`ds4_cuda.cu:1272` `ds4_gpu_set_model_map_range`**：去掉 `getenv("DS4_CUDA_COPY_MODEL_CHUNKED")` 门控；Windows 默认调 `cuda_model_copy_chunked`。
2. **`ds4_cuda.cu:1251-1268` `ds4_gpu_set_model_map` 兜底分支**：Windows `#ifdef` 跳过 `cudaHostRegister(model_map, …)`。
3. **`ds4_cuda.cu:217` `ds4_cuda_register_range`**：reg_addr 来自 `MapViewOfFile`，Windows 同样 `#ifdef` 跳过该分支。

改动估计 **30~50 行**，触及 3 个函数的控制流分支。

**附注：Windows chunk 大小策略**

WDDM 下 pinned host memory (`cudaMallocHost`) 上限受 OS 主导分页约束，比 Linux 低；BAR1 在桌面 GPU 通常 256MiB（4090 等消费卡）到 4-8GiB（A100/H100 在 Windows Server 上）。固定值不能覆盖所有目标硬件。

`cuda_model_copy_chunked` 的 chunk 大小走两级策略：

```c
// 1. 自动探测（默认）：基于 BAR1 大小给出保守值
size_t pick_default_chunk_bytes(void) {
    size_t free_b = 0, total_b = 0;
    cudaMemGetInfo(&free_b, &total_b);
    // 桌面 WDDM 卡 BAR1 通常 256MiB；按 1/4 ~ 1/2 取一个不会饿死 GPU 的值
    size_t chunk = 64ull * 1024 * 1024;            // 64MiB 起步
    if (total_b >= (16ull << 30)) chunk = 256ull * 1024 * 1024;  // >=16GiB 显存放大到 256MiB
    return chunk;
}

// 2. 用户旋钮：env DS4_CUDA_COPY_CHUNK_MIB=128 覆盖（同时存在于 Linux 路径）
```

Windows v1 默认从 64MiB 起步；用户可通过 `DS4_CUDA_COPY_CHUNK_MIB` 环境变量调高。chunk 大小是 §六 验收指标里"加载时间 / 显存峰值"的直接影响变量，应在 D7 记录至少 64MiB / 128MiB / 256MiB 三档基线，写入 follow-up issue 决定后续默认值。

### 3.5 线程 — `os_thread.h / os_thread.c`

```c
// os_thread.h
#ifdef _WIN32
  #include <windows.h>

  // ⚠ 用 SRWLOCK 而非 CRITICAL_SECTION：CRITICAL_SECTION 是递归锁，
  //    会掩盖代码中意外的重入 deadlock bug。
  // ⚠ 前置审查项（D2 必做）：grep 所有 pthread_mutex_lock 调用点，
  //    确认没有同一线程内嵌套对同一把锁加锁的模式。若发现，单独
  //    typedef os_recursive_mutex_t（基于 CRITICAL_SECTION），不要
  //    把整体默认改回递归锁——会让其它 99% 的非递归用法失去 deadlock
  //    早暴露的好处。当前 grep 结果（ds4.c + ds4_server.c）显示无明显
  //    递归模式，但 D2 实施时必须二次确认 worker pool + KV cache 两处
  //    锁的调用栈。
  typedef SRWLOCK os_mutex_t;
  #define os_mutex_init(m)    InitializeSRWLock(m)
  #define os_mutex_lock(m)    AcquireSRWLockExclusive(m)
  #define os_mutex_unlock(m)  ReleaseSRWLockExclusive(m)
  #define os_mutex_destroy(m) ((void)0)

  typedef CONDITION_VARIABLE os_cond_t;
  #define os_cond_init(c)      InitializeConditionVariable(c)
  #define os_cond_wait(c,m)    SleepConditionVariableSRW(c, m, INFINITE, 0)
  #define os_cond_signal(c)    WakeConditionVariable(c)
  #define os_cond_broadcast(c) WakeAllConditionVariable(c)

  // 一次性初始化：InitOnceExecuteOnce 回调签名与 pthread_once 不同，走函数封装。
  typedef INIT_ONCE os_once_t;
  #define OS_ONCE_INIT INIT_ONCE_STATIC_INIT
  void os_once(os_once_t *once, void (*init_fn)(void));

  // 线程创建走 _beginthreadex（含 CRT TLS 初始化；返回 HANDLE 由调用方 CloseHandle）
  typedef HANDLE os_thread_t;
  int  os_thread_create(os_thread_t *t, unsigned (__stdcall *fn)(void*), void *arg);
  void os_thread_join  (os_thread_t  t);

  // TLS 用 C11 _Thread_local（兼容 GCC/Clang/MSVC）
#else
  typedef pthread_mutex_t os_mutex_t;
  typedef pthread_cond_t  os_cond_t;
  typedef pthread_once_t  os_once_t;
  typedef pthread_t       os_thread_t;
  #define OS_ONCE_INIT PTHREAD_ONCE_INIT
  static inline void os_once(os_once_t *once, void (*fn)(void)) { pthread_once(once, fn); }
  // ...其余直接走 pthread_*
#endif
```

`os_thread.c` Windows 实现：

```c
#ifdef _WIN32
static BOOL CALLBACK os_once_trampoline(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)ctx;
    ((void (*)(void))param)();
    return TRUE;
}
void os_once(os_once_t *once, void (*init_fn)(void)) {
    InitOnceExecuteOnce(once, os_once_trampoline, (PVOID)init_fn, NULL);
}

int os_thread_create(os_thread_t *t, unsigned (__stdcall *fn)(void*), void *arg) {
    uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
    if (h == 0) return -1;
    *t = (HANDLE)h;
    return 0;
}
void os_thread_join(os_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);   // _beginthreadex 返回的 handle 必须显式关闭
}
#endif
```

`ds4.c` 和 `ds4_server.c` 中替换数（基于实际 grep）：

- `pthread_mutex_*` → `os_mutex_*` 约 50 处
- `pthread_cond_*` → `os_cond_*` 约 15 处
- `pthread_create/join/detach` → `os_thread_*` 约 8 处
- `pthread_once` → `os_once` 共 1 处（`ds4.c:305`, `:681`）
- `__thread` → `_Thread_local` 共 1 处（`ds4.c:633`）

合计约 75 处替换。

### 3.6 网络 — `ds4_server.c`

```c
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")

  typedef SOCKET socket_t;          // SOCKET 是 UINT_PTR，不是 int
  #define OS_INVALID_SOCKET INVALID_SOCKET
  #define socklen_t        int
  #define SHUT_RDWR        SD_BOTH
  #define close_socket(s)  closesocket(s)
  #define sock_errno()     WSAGetLastError()

  static int wsock_init(void) {
      WSADATA d;
      return WSAStartup(MAKEWORD(2, 2), &d);
  }
  static void wsock_cleanup(void) { WSACleanup(); }

  static int os_set_nonblocking(SOCKET s) {
      u_long nb = 1;
      return ioctlsocket(s, FIONBIO, &nb);  // 不是 fcntl
  }
#else
  typedef int socket_t;
  #define OS_INVALID_SOCKET (-1)
  #define close_socket(s)  close(s)
  #define sock_errno()     errno
  static inline int  wsock_init(void)    { return 0; }
  static inline void wsock_cleanup(void) {}
  static inline int  os_set_nonblocking(int fd) {
      int fl = fcntl(fd, F_GETFL, 0);
      return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
#endif
```

**改动触面**（比初版"12 处"显著更大）：

- `int fd` 用作 socket 的所有位置改为 `socket_t`（含 `send_all` / `recv` / 各 handler 函数签名），约 30 处
- 监听/连接 socket 的关闭点统一 `close_socket(s)` 宏
- **socket 错误码全部从 `errno` 改为 `sock_errno()`**（Windows 上 `errno` 与 socket 无关）
- **`send()` / `recv()` 长度参数是 `int`，单次最多 ~2GiB**：当前 `send_all` 已是循环；为防隐式截断，把 helper 签名显式收窄到 `int`，调用方处的 `size_t` 必须在循环里手动 clamp：

```c
// 显式收窄，调用方传入 size_t 会触发编译器 narrowing 警告（/W3 下）
static bool send_all(socket_t fd, const void *p, int n);

// 调用方循环（典型用法）：
size_t remain = total;
const char *cur = buf;
while (remain > 0) {
    int chunk = (remain > (size_t)INT_MAX) ? INT_MAX : (int)remain;
    if (!send_all(fd, cur, chunk)) return false;
    cur += chunk; remain -= (size_t)chunk;
}
```

- **最低 Windows 版本要求**：Winsock2 + WSAPoll 要求 **Windows Vista / Server 2008 及以上**；CONDITION_VARIABLE / SRWLOCK 同样 Vista+；BCryptGenRandom 是 Vista+；`SetThreadDescription` 等更新的 API 我们不使用。README 与 `CMakeLists.txt` 顶部都明示 `WINVER=_WIN32_WINNT_WIN7 (0x0601)` 作为最低支持。
- `main` 入口先 `wsock_init`，正常退出前 `wsock_cleanup`
- `poll()` → `WSAPoll()`。当前 `ds4_server.c:2541` 只用 `POLLOUT + timeout` 做发送超时，**不踩 WSAPoll 已知 bug**（WSAPoll 不上报 connect 失败的 POLLERR/POLLHUP，Microsoft 公开承认）；代码注释中明示，避免后续误用
- **Ctrl+C / 退出路径**：不能在 Windows signal handler 里直接 `close(int fd)`（fd 是 socket）。通过 `SetConsoleCtrlHandler` 设置 atomic shutdown flag，让 main loop 在下一次 `WSAPoll` timeout 时检查并退出

### 3.7 杂项平台调用

| 文件 | 原调用 | Windows 替换 | 处数 | 备注 |
|------|--------|-------------|------|------|
| ds4.c | `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter` + `QueryPerformanceFrequency` | 4 | 走 `os_clock.h` |
| ds4.c | `__thread` | `_Thread_local` | **1** | `ds4.c:633` |
| ds4.c | `flock(fd, LOCK_EX\|LOCK_NB)` | `LockFileEx(hFile, LOCKFILE_EXCLUSIVE_LOCK\|LOCKFILE_FAIL_IMMEDIATELY, …)` | 1 | `ds4.c:15197`；需要 HANDLE 不是 fd，走 `os_file_t` |
| ds4.c | `fstat(fd, &st)` 配合 `st_size` | `os_file_size(&f)` 内部 `_fstat64` | 2 | `ds4.c:1205` 是 80GB 模型大小读取入口，**必须 64-bit** |
| ds4.c | `dprintf(fd, …)` | `_write` + `snprintf` 包装 | 1 | `ds4.c:15225` MSVC 无 `dprintf` |
| ds4_server.c | `/dev/urandom` + `read` | `BCryptGenRandom(NULL, …, BCRYPT_USE_SYSTEM_PREFERRED_RNG)` | 1 | `ds4_server.c:86` |
| ds4_server.c | `getpid()` | `GetCurrentProcessId()` | 3 | |
| ds4_server.c | `stat(path, &st)` | `_stat64` + UTF-8 包装 | 2 | |
| ds4_server.c | `opendir`/`readdir`/`closedir` | `FindFirstFileW`/`FindNextFileW`/`FindClose` | 2 | 通过 `win_dirent.h` 包装；UTF-16 → UTF-8 文件名转换 |
| ds4_server.c | `signal(SIGINT/SIGTERM, …)` + `sigaction` | **`SetConsoleCtrlHandler`** | 1 | `ds4_server.c:8091-8096`；详见下文 |
| ds4_server.c | `signal(SIGPIPE, SIG_IGN)` | **`#ifdef` 跳过** | 1 | `ds4_server.c:8090`；Windows 上 send 失败靠返回值 + `WSAGetLastError`，无信号 |

**信号处理替换细节**：

```c
#ifdef _WIN32
static volatile LONG g_shutdown_flag = 0;
static BOOL WINAPI ds4_console_ctrl(DWORD ctrl) {
    switch (ctrl) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            InterlockedExchange(&g_shutdown_flag, 1);
            return TRUE;
        default:
            return FALSE;
    }
}
// main 入口：
SetConsoleCtrlHandler(ds4_console_ctrl, TRUE);
// main loop 在 WSAPoll 返回后检查 g_shutdown_flag
#else
struct sigaction sa = { .sa_handler = ds4_server_request_shutdown };
sigaction(SIGINT,  &sa, NULL);
sigaction(SIGTERM, &sa, NULL);
signal(SIGPIPE, SIG_IGN);
#endif
```

### 3.8 控制台与文本 IO — `os_console.h`

```c
#ifdef _WIN32
static inline void os_console_init(void) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    setvbuf(stdout, NULL, _IONBF, 0);    // pipe 下也实时 flush
    setvbuf(stderr, NULL, _IONBF, 0);
}
#else
static inline void os_console_init(void) {}
#endif
```

`ds4_server` 的 `main` 最前面调一次。没有这步，中文/Emoji 在 Windows 终端会乱码，token 流式输出在管道下会被缓冲死。

### 3.9 CLI 策略

Windows v1 **不构建 `ds4.exe`**：

- `linenoise.c/h` 依赖 termios，没有 Windows 等价；
- `ds4_cli.c` 整体不进入 CMake target list（在 `if(NOT WIN32)` 内）；
- 用户走 `ds4_server.exe` 的 HTTP API 做推理。

后续如需保留 one-shot / diagnostics（不带 interactive readline），单独立项重构 `ds4_cli.c`，把 linenoise 调用剥离到独立 source。当前 PR 不在 D1-D8 计划内处理这一项，避免与"砍掉 CLI"自相矛盾。

---

## 四、文件改动量总览

| 文件 | 类型 | 大约行数 | 难度 |
|------|------|---------|------|
| `CMakeLists.txt` | **新增** | ~100 | 低 |
| `src/platform/os_file.h` | **新增** | ~30 | 低 |
| `src/platform/os_file.c` | **新增** | ~80 | 中（DWORD 分块、GetLastError 映射） |
| `src/platform/os_mmap.h` | **新增** | ~25 | 低 |
| `src/platform/os_mmap.c` | **新增** | ~110 | 中（含 UTF-8 路径转换） |
| `src/platform/os_thread.h` | **新增** | ~80 | 中 |
| `src/platform/os_thread.c` | **新增** | ~30 | 低 |
| `src/platform/os_clock.h` | **新增** | ~20 | 低 |
| `src/platform/os_random.h` | **新增** | ~15 | 低 |
| `src/platform/os_path.h` | **新增** | ~25 | 低 |
| `src/platform/os_console.h` | **新增** | ~30 | 低 |
| `src/win32/win_dirent.h` | **新增** | ~90 | 中（UTF-16 → UTF-8） |
| `ds4_gpu.h` | 修改 | ~10 | 低（API 改造） |
| `ds4.c` | 修改 | ~95 | 中 |
| `ds4_cuda.cu` | 修改 | ~90 | 中-高（WDDM 内存模型 + os_file 改造） |
| `ds4_server.c` | 修改 | ~140 | 中（socket_t/WSAGetLastError 改动比初版大） |
| `linenoise.c/h` / `ds4_cli.c` | **不编译** | 0 | — |
| **合计新增** | | **~635** | |
| **合计修改** | | **~335** | |
| **总计** | | **~970** | |

---

## 五、实施步骤（7-8 天）

| 天 | 任务 | 产出 |
|----|------|------|
| **D1** | CMake + MSVC + CUDA 构建骨架；`os_path.h` UTF-8 转换；`os_file.h/c` + `os_pread`；`os_mmap` | `ds4_platform.lib` 可编译 |
| **D2** | `os_thread`（SRWLOCK / CONDITION_VARIABLE / `os_once` / `_Thread_local`）/ `os_clock` / `os_random`；改 `ds4.c` 线程与同步调用、`mmap`→`os_mmap` | `ds4.lib` 可编译 |
| **D3** | 改 `ds4_server.c` Winsock + `socket_t` + `WSAGetLastError`；接 `SetConsoleCtrlHandler`、`os_console_init`、`BCryptGenRandom` | `ds4_server.obj` 可编译 |
| **D4** | `_stat64` / `win_dirent` UTF-16 文件枚举；`ds4.c` 的 `_fstat64` 与 `flock`→`LockFileEx`；`ds4_gpu.h` API 改造为 `os_file_t*` | `ds4_server` IO 路径就绪 |
| **D5** | `ds4_cuda.cu` 平台调用（页大小 / `os_pread` / madvise no-op）；关掉 WDDM 下 `cudaHostRegister` 三处分支，默认走 `cuda_model_copy_chunked` | `ds4_cuda.lib` 可编译 |
| **D6** | 链接全量 `ds4_server.exe`；真机跑通模型加载 → 首 token；排查 cudart/cuBLAS 链接、driver 兼容、`CUDA_ARCHITECTURES` 不匹配 | 首 token 生成 |
| **D7** | 接 `tests/` 到 CTest；跑全量单测；对比 Linux 基线吞吐 + 显存 + working set | 验收指标全部记录 |
| **D8（buffer）** | 处理 D6/D7 未解决项；调 chunk 大小、pinned buffer 上限、driver/toolkit 组合 | 可发布预览版 |

D6 与 D7 历史上是 Windows + CUDA 项目最容易超时的两天，建议预算 D8 作为缓冲。

---

## 六、验收标准

发布预览版前必须全部通过：

1. `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DDS4_CUDA_ARCHITECTURES=89` 配置成功
2. `cmake --build build --config Release` 生成 `ds4_server.exe`，零警告（`/W3` 下）
3. `ds4_server.exe --help` 正常输出，UTF-8 控制台无乱码
4. CUDA 后端能加载 GGUF（含包含非 ASCII 字符的路径），完成 warm / cache 阶段，不触发 Windows-only assert 或 `cudaErrorInvalidValue` 链路
5. 固定 prompt 下生成首 token，输出与 Linux CUDA 或 CPU reference 做 basic sanity compare（前 10 tokens 一致）
6. `/v1/chat/completions` 非流式 + 流式各一条 smoke test 通过
7. Ctrl+C 能在 ≤2 秒内优雅停机，无显存泄漏
8. **指标记录**（不是断言，但必须有数）：
   - 模型加载时间（与 Linux 同模型同硬件对比）
   - 首 token 延迟
   - 显存峰值
   - 进程 working set 峰值
9. CTest 全绿（D7 接入的单测）
10. **长路径冒烟**：用 ≥270 字符的模型路径（自动加 `\\?\` 前缀路径）跑一次模型加载，确认不报 `ERROR_PATH_NOT_FOUND`
11. **chunk 大小基线**：D7 记录 64MiB / 128MiB / 256MiB 三档下的模型加载时间与显存峰值，存档便于后续默认值调优

---

## 七、风险与对策

| 风险 | 概率 | 对策 |
|------|------|------|
| WDDM 下 `cudaHostRegister(MapViewOfFile)` 确定性失败 | **确定** | 默认走 `cuda_model_copy_chunked`，关闭三处兜底分支；详见 §3.4 |
| MSVC 对 C99 VLA 不支持 | 低 | 已 grep 确认 `ds4.c` 未使用 VLA。其它 C99/C11 特性（designated initializer / 复合字面量 / `_Alignof` / `_Thread_local`）在 VS 2019 16.8+ / `/std:c11` 下均支持 |
| C99 designated init 在 NVCC C++ 模式不可靠 | 中 | `.cu` 文件统一用 `OVERLAPPED ol{}` + 显式字段赋值，相关逻辑封装到 `os_pread` |
| NVCC 与 MSVC 版本兼容 | 低 | CUDA 12.x 官方支持 VS 2022 17.x；如出现 `cl.exe` 版本被拒，用 `-allow-unsupported-compiler` 兜底 |
| 双 GPU 设备选择逻辑 | 低 | `cudaSetDevice` / `cudaGetDeviceCount` 跨平台 |
| Unicode 模型路径（中文用户） | 中 | 所有路径入口走 `os_utf8_to_wide` → `CreateFileW`/`_wstat64`，禁用 ANSI 版本 |
| WSAPoll 不上报 connect 失败 | 低 | 当前 server 仅用 POLLOUT + timeout，未受影响。代码注释明示 |
| `send/recv` length 是 `int`，大 buffer 溢出 | 低 | `send_all` 已是循环；review 时校验所有调用点 `n` 不会直接传 `size_t` |
| Pinned host memory 上限低于 Linux | 中 | chunk 默认在 Windows 上调小（256MiB → 64MiB）并保留 env 旋钮 |
| Buffered I/O 在多源内存压力下性能回退 | 中 | 列入 §六 指标记录；若回退显著再评估 `FILE_FLAG_NO_BUFFERING`（需对齐 offset/buffer/size） |
| `_beginthreadex` vs `CreateThread` CRT 状态泄漏 | 低 | 统一走 `_beginthreadex`；`os_thread_join` 内 `CloseHandle` |
| 双构建系统 (Makefile + CMake) 漂移 | 中 | **GitHub Actions matrix**：`ubuntu-latest` 跑 Makefile（CPU + CUDA via container），`windows-latest` 跑 CMake + MSVC + CUDA。任一失败 PR 不可合并。新功能必须同时改两边或在 PR 描述里显式标注 |
| SRWLOCK 在意外递归调用点死锁 | 低 | D2 实施前 grep 所有 `pthread_mutex_lock` 调用栈；worker pool + KV cache 两处重点审查 |
| 长路径 (`\\?\` 前缀) 兼容性 | 低 | `os_utf8_to_wide` 自动加前缀；§六 验收第 10 项冒烟测试 |

---

## 八、未决项 / Follow-up（不在 v1 实施计划内）

- Windows one-shot CLI / diagnostics（重构 `ds4_cli.c` 剥离 linenoise）
- `GitHub Actions` workflow 文件：matrix 跑 Linux Makefile + Windows CMake，触发于所有 PR（与"双构建系统漂移"风险对策对齐）
- `cudaMallocManaged` benchmark vs staged copy
- `cudaHostRegisterReadOnly` + 设备能力查询，对自分配 pinned buffer 是否有收益
- KV cache 文件锁在跨进程多 server 实例下的语义（`LockFileEx` vs `flock` 在网络盘上的差异）
- 显存压力下 chunked copy 的 progressive eviction
- CUDA driver 版本探测（在不支持 CUDA 12.x 的旧 driver 上给出清晰错误）
- `FILE_FLAG_NO_BUFFERING` KV cache 优化（仅在 buffered I/O 性能不达标时）
