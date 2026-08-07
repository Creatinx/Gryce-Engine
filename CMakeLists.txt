cmake_minimum_required(VERSION 3.28)

# 必须在 project() 之前禁用 C++20 modules 扫描，否则 CMake 4.x 在 C++23 下会自动启用
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# 平台：Windows 下优先使用 MinGW GCC，未找到时让 CMake 自动检测（支持 MSVC fallback）
# 注意：当使用 Visual Studio generator 时，保留 MSVC 选择，不强制切到 MinGW。
if(WIN32 AND NOT CMAKE_C_COMPILER AND NOT CMAKE_CXX_COMPILER)
    # Visual Studio generator 明确使用 MSVC，不要覆盖
    set(_using_vs_generator FALSE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_using_vs_generator TRUE)
    endif()

    if(NOT _using_vs_generator)
        find_program(GRYCE_MINGW_GCC gcc
            PATHS "$ENV{MSYS2_PREFIX}/bin" "C:/msys64/ucrt64/bin" "C:/msys64/mingw64/bin"
        )
        find_program(GRYCE_MINGW_GXX g++
            PATHS "$ENV{MSYS2_PREFIX}/bin" "C:/msys64/ucrt64/bin" "C:/msys64/mingw64/bin"
        )
        if(GRYCE_MINGW_GCC AND GRYCE_MINGW_GXX)
            set(CMAKE_C_COMPILER "${GRYCE_MINGW_GCC}" CACHE FILEPATH "C compiler" FORCE)
            set(CMAKE_CXX_COMPILER "${GRYCE_MINGW_GXX}" CACHE FILEPATH "C++ compiler" FORCE)
            message(STATUS "[Gryce Engine] Auto-selected MinGW GCC: ${GRYCE_MINGW_GCC}")
        else()
            message(WARNING
                "[Gryce Engine] MinGW GCC not found in PATH.  "
                "This project is primarily developed and tested with MSYS2 UCRT64 MinGW-w64.  "
                "Install with: pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja  "
                "Or specify compiler explicitly: -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++"
            )
        endif()
    endif()
endif()

project(GryceEngine
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Gryce Engine — C++ Game Engine"
)

# ---------------------------------------------------------------------------
# C++ 标准
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 统一输出目录（避免 Debug/Release 子目录嵌套）
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

foreach(config_type IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config_type} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin/${config_type})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib/${config_type})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib/${config_type})
endforeach()

# ---------------------------------------------------------------------------
# 选项
# ---------------------------------------------------------------------------
option(GRYCE_BUILD_TESTS          "Build unit tests" ON)
option(GRYCE_ENABLE_SANITIZERS  "Enable ASan/UBSan in Debug (GCC/Clang only)" OFF)
option(GRYCE_ENABLE_LTO           "Enable Link-Time Optimization in Release" ON)
option(GRYCE_STATIC_RUNTIME       "Use static C++ runtime (/MT) on MSVC" OFF)

# ---------------------------------------------------------------------------
# 编译器选项
# ---------------------------------------------------------------------------
include(cmake/compiler_options.cmake)
include(cmake/shaders.cmake)

# ---------------------------------------------------------------------------
# 第三方依赖
#
# 外部依赖（glfw, assimp, box2d, jolt）由 Python 脚本下载到 build/deps/ 下，
# CMake 通过 add_subdirectory 直接集成，不再使用 FetchContent。
#
# 首次配置前，请运行：
#     python tools/deps_manager.py download
# 或
#     python build.py --setup-deps
# ---------------------------------------------------------------------------
# GLEW 等旧依赖声明的 cmake_minimum_required 过低，CMake 4.x 默认拒绝；
# 放宽到 3.5，允许继续配置。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(cmake/deps_resolver.cmake)

# ---------------------------------------------------------------------------
# 子目录
# ---------------------------------------------------------------------------
add_subdirectory(core)

if(GRYCE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_subdirectory(examples)

# editor 已封存至 backup/，WPF Editor 走 C API 调用 GryceCore DLL
# add_subdirectory(editor)

# ---------------------------------------------------------------------------
# WPF Editor 集成到 VS 解决方案（仅 Visual Studio Generator 生效）
# ---------------------------------------------------------------------------
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    include_external_msproject(
        GryceEngine.Editor
        ${CMAKE_SOURCE_DIR}/editor/GryceEngine.Editor.csproj
        TYPE FAE04EC0-301F-11D3-BF4B-00C04F79EFBC
    )
endif()

# ---------------------------------------------------------------------------
# IDE 分组
# ---------------------------------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT gt2dDemo)
