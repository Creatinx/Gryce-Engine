cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# toolchain/grycegc — Gryce-Engine 命令行工具链（C++20）
#
# 支持两种构建模式：
#   1) 独立配置：cmake -S toolchain/grycegc -B build-grycegc
#   2) 作为根 CMakeLists 的 add_subdirectory（选项 GRYCE_BUILD_TOOLCHAIN 控制）
#
# 关键设计：自包含、零引擎 DLL 依赖。同 GryceMathTests 模式，
# 把需要的 engine 源码直接编进 grycegc_core 静态库。GRYCE_API 在
# 未定义 GRYCE_BUILD_SHARED 时为空宏，不会引入 dllexport/dllimport 修饰。
# ---------------------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(GryceGC VERSION 1.0.0 LANGUAGES CXX)
    set(GRYCE_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Gryce-Engine source root")
    enable_testing()
    set(BUILD_TESTING ON CACHE BOOL "Build GryceGC tests" FORCE)
    set(GRYCEGC_STANDALONE ON)
else()
    if(NOT DEFINED GRYCE_ENGINE_ROOT)
        set(GRYCE_ENGINE_ROOT "${CMAKE_SOURCE_DIR}")
    endif()
    set(GRYCEGC_STANDALONE OFF)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# grycegc_core：核心静态库（命令实现 + 工具核心 + 共享的引擎资源源码）
# 所有命令代码、core 工具代码、引擎侧资源代码在此统一编译。main.cpp 与
# 测试 exe 分别链接这个库。这样 Windows 上不会因两个 exe 互链而无法生成。
# ---------------------------------------------------------------------------
add_library(grycegc_core STATIC
    src/cli.cpp
    src/core/fsutil.cpp
    src/core/pe_image.cpp
    src/core/dep_resolver.cpp
    src/commands/registry.cpp
    src/commands/cmd_help.cpp
    src/commands/cmd_deps.cpp
    src/commands/cmd_verify.cpp
    src/commands/stubs.cpp
    # 引擎侧：共享密码学（GryceCore 与本工具共用）
    ${GRYCE_ENGINE_ROOT}/engine/utils/glog/glog_lib.cpp
    ${GRYCE_ENGINE_ROOT}/engine/utils/crypto/crc32.cpp
    ${GRYCE_ENGINE_ROOT}/engine/utils/crypto/chacha20.cpp
    ${GRYCE_ENGINE_ROOT}/engine/utils/crypto/random.cpp
    # 引擎侧：资源打包与加密（GPAK v1/v3/v4、AES-256-GCM）
    ${GRYCE_ENGINE_ROOT}/engine/resources/gpack_bundle.cpp
    ${GRYCE_ENGINE_ROOT}/engine/resources/pak_bundle.cpp
    ${GRYCE_ENGINE_ROOT}/engine/resources/aes_gcm.cpp
)

target_include_directories(grycegc_core PUBLIC
    src
    ${GRYCE_ENGINE_ROOT}/engine
    ${GRYCE_ENGINE_ROOT}/third_party/nlohmann_json/include
)
target_compile_definitions(grycegc_core PUBLIC
    GRYCE_GC_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    target_link_libraries(grycegc_core PUBLIC bcrypt)
endif()

# ---------------------------------------------------------------------------
# GryceGC.exe 主程序
# ---------------------------------------------------------------------------
add_executable(GryceGC src/main.cpp)
target_link_libraries(GryceGC PRIVATE grycegc_core)
set_target_properties(GryceGC PROPERTIES OUTPUT_NAME "GryceGC")

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    add_executable(GryceGCTests test/grycegc_tests.cpp)
    target_link_libraries(GryceGCTests PRIVATE grycegc_core)
    set_target_properties(GryceGCTests PROPERTIES OUTPUT_NAME "GryceGCTests")

    set(_grycegc_suites cli crc32 pe aes smoke)
    foreach(s IN LISTS _grycegc_suites)
        add_test(NAME grycegc_${s}
                 COMMAND GryceGCTests --suite ${s}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(grycegc_${s} PROPERTIES
            LABELS "unit;grycegc" TIMEOUT 60)
    endforeach()
endif()
