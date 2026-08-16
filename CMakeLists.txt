cmake_minimum_required(VERSION 3.22)

# 必须在 project() 之前禁用 C++20 modules 扫描，否则 CMake 4.x 在 C++23 下会自动启用。
# 该变量是 CMake 3.28 引入的；旧版 CMake 默认不扫描 modules，无需设置。
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

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

# WPF Editor（C#）编译开关：Windows 上通过 dotnet 构建，与 generator 无关
# （Ninja / Makefiles / Visual Studio 均可）。非 Windows 平台忽略。
option(GRYCE_BUILD_EDITOR        "Build the (C#) Editor" OFF)

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

# GryceSPC: standalone game template (GryceGC packaging entry point)
option(GRYCE_BUILD_GAME_TEMPLATE "Build the standalone game template (GryceSPC)" ON)
add_subdirectory(templates)

# GryceGC: C++ resource packager (GryceSPC packaging tool)
add_subdirectory(tools/grycegc)

if(GRYCE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_subdirectory(examples)

# ---------------------------------------------------------------------------
# WPF Editor（editor/CMakeLists.txt）
#
# 仅当 GRYCE_BUILD_EDITOR=ON 且 Windows 时挂载。编辑器目标不加入 ALL：
# 普通 `cmake --build <dir>` 不会编译它；显式 `cmake --build <dir>
# --target GryceEditor` 时会先编译核心 DLL 依赖（POST_BUILD 自动复制
# 原生 DLL 到 editor/bin/<Config>/net48/），再执行 dotnet 构建。
# ---------------------------------------------------------------------------
if(GRYCE_BUILD_EDITOR)
    if(WIN32)
        add_subdirectory(editor)
    else()
        message(WARNING "GRYCE_BUILD_EDITOR=ON ignored: WPF Editor is Windows-only")
    endif()
endif()

# ---------------------------------------------------------------------------
# IDE 分组
# ---------------------------------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT 2dDemo)
