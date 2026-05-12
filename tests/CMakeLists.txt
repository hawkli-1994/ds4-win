if(NOT WIN32)
    add_executable(ds4_test ds4_test.c)
    target_link_libraries(ds4_test PRIVATE
        ds4
        ds4_cuda
        CUDA::cudart
        CUDA::cublas
    )
    add_test(NAME ds4_test COMMAND ds4_test)
endif()
