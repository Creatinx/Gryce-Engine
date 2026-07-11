# cmake/compiler_options.cmake
# 跨平台编译器选项统一配置

# ---------------------------------------------------------------------------
# MSVC (Visual Studio)
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(
        /W4                    # 最高警告级别
        /permissive-           # 严格标准模式
        /Zc:__cplusplus        # 正确报告 __cplusplus
        /Zc:preprocessor       # 标准预处理器
        /EHsc                  # 仅 C++ 异常（不捕捉 SEH）
        /utf-8                 # 源码与执行字符集 UTF-8
        /MP                    # 多处理器编译
    )

    # 运行时库
    if(GRYCE_STATIC_RUNTIME)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    # Debug
    add_compile_options($<$<CONFIG:Debug>:/Od>)
    add_compile_options($<$<CONFIG:Debug>:/Zi>)
    add_compile_options($<$<CONFIG:Debug>:/RTC1>)

    # Release
    add_compile_options($<$<CONFIG:Release>:/O2>)
    add_compile_options($<$<CONFIG:Release>:/Ob2>)
    add_compile_options($<$<CONFIG:Release>:/DNDEBUG>)
    add_compile_options($<$<CONFIG:Release>:/GF>)  # 字符串池化

    # LTO
    if(GRYCE_ENABLE_LTO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()

# ---------------------------------------------------------------------------
# GCC / Clang
# ---------------------------------------------------------------------------
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wlogical-op)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-Wimplicit-fallthrough)
    endif()

    # Debug
    # 注意：GCC 16 + MinGW 在 -O0 -g3 下会生成导致 ld 崩溃的对象文件
    # (collect2: ld returned 5)。启用 Vulkan 后端后对象进一步膨胀，需要 -O2 -g0。
    add_compile_options($<$<CONFIG:Debug>:-O1>)
    add_compile_options($<$<CONFIG:Debug>:-g1>)
    add_compile_options($<$<CONFIG:Debug>:-fno-omit-frame-pointer>)

    # 使用 LLVM lld 替代 GNU ld，避免处理大型对象文件时崩溃
    find_program(LLD_LINKER ld.lld.exe PATHS ${CMAKE_CXX_COMPILER}/.. ${CMAKE_CXX_COMPILER}/../..)
    if(LLD_LINKER)
        add_link_options(-fuse-ld=lld)
        message(STATUS "Using LLVM lld linker: ${LLD_LINKER}")
    else()
        message(WARNING "ld.lld not found; linking large binaries may fail with GNU ld")
    endif()



    # Release
    add_compile_options($<$<CONFIG:Release>:-O3>)
    add_compile_options($<$<CONFIG:Release>:-DNDEBUG>)

    # Sanitizers
    if(GRYCE_ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()

    # LTO
    # 注意：MinGW + lld + -flto 在链接 console 子系统可执行文件时会错误地寻找 WinMain，
    # 因此默认在 MinGW 上禁用 Release 的 IPO，除非显式开启且能验证链接通过。
    if(GRYCE_ENABLE_LTO AND NOT (WIN32 AND MINGW))
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endif()
