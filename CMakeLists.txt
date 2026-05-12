cmake_minimum_required(VERSION 3.24)
project(ds4 LANGUAGES C CXX CUDA)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DS4_CUDA_ARCHITECTURES "80;86;89;90" CACHE STRING
    "CUDA architectures (semicolon-separated SM list, or 'native')")

find_package(CUDAToolkit REQUIRED)

add_library(ds4_platform STATIC
    src/platform/os_file.c
    src/platform/os_mmap.c
    src/platform/os_thread.c
)
target_include_directories(ds4_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win32
)
if(WIN32)
    target_compile_definitions(ds4_platform PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0601
    )
elseif(NOT APPLE)
    target_compile_definitions(ds4_platform PUBLIC _GNU_SOURCE)
endif()

add_library(ds4_cuda STATIC ds4_cuda.cu)
target_compile_features(ds4_cuda PRIVATE cxx_std_17)
target_link_libraries(ds4_cuda PUBLIC ds4_platform)
set_target_properties(ds4_cuda PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "${DS4_CUDA_ARCHITECTURES}"
)

add_library(ds4 STATIC ds4.c rax.c)
target_link_libraries(ds4 PUBLIC ds4_platform)
if(WIN32)
    target_link_libraries(ds4 PUBLIC bcrypt)
endif()

add_executable(ds4_server ds4_server.c)
target_link_libraries(ds4_server PRIVATE
    ds4
    ds4_cuda
    CUDA::cudart
    CUDA::cublas
)
if(WIN32)
    target_link_libraries(ds4_server PRIVATE ws2_32 bcrypt)
endif()

if(NOT WIN32)
    add_executable(ds4_cli ds4_cli.c linenoise.c)
    target_link_libraries(ds4_cli PRIVATE
        ds4
        ds4_cuda
        CUDA::cudart
        CUDA::cublas
    )
endif()

enable_testing()
add_subdirectory(tests)
