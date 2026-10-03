# ===========================================================================
# cmake/GryceFetchDeps.cmake
#
# 依赖缺失时的 FetchContent 兜底。
#
# 重要：CMake **不会**自动下载依赖。find_package / find_library 只在本机磁盘
# 上搜索已安装的东西；要"自动下载"必须显式用 FetchContent（CMake 内置）、
# ExternalProject、CPM.cmake 或 vcpkg/conan。本模块用 CMake 内置的
# FetchContent：优先用系统包，系统包找不到时才拉源码随工程一起编译，
# 这样"装了环境就能编，没装环境也能编"。
#
# 开关：
#   -DGRYCE_DEP_FETCH=AUTO  默认。系统包优先，缺失项才下载源码构建
#   -DGRYCE_DEP_FETCH=ON    总是用源码构建（忽略系统包）
#   -DGRYCE_DEP_FETCH=OFF   只用系统包（离线/受控环境）
#   -DGRYCE_DEP_GIT_BASE=<前缀>  换镜像，例如 https://gh-proxy.com/https://github.com/
# ===========================================================================
include_guard(GLOBAL)

include(FetchContent)

set(GRYCE_DEP_FETCH "AUTO" CACHE STRING
    "缺失依赖时用 FetchContent 下载源码构建：AUTO / ON / OFF")
set_property(CACHE GRYCE_DEP_FETCH PROPERTY STRINGS AUTO ON OFF)
set(GRYCE_DEP_GIT_BASE "https://github.com/" CACHE STRING
    "依赖源码仓库前缀，网络不通时可换镜像（末尾保留 /）")

# 下载时打印进度：首次配置会 clone 源码，静默会让人以为卡死
set(FETCHCONTENT_QUIET OFF CACHE BOOL "" FORCE)
# 源码已经下载过就跳过 update（git fetch）：否则每次重新配置都要走一遍网络，
# 网络受限时会长时间卡住
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

function(gryce_fetch_enabled out_var)
    if(GRYCE_DEP_FETCH STREQUAL "OFF")
        set(${out_var} FALSE PARENT_SCOPE)
    else()
        set(${out_var} TRUE PARENT_SCOPE)
    endif()
endfunction()

# 统一构造仓库地址：${GRYCE_DEP_GIT_BASE}<org>/<repo>.git
function(gryce_repo_url out_var org repo)
    string(REGEX REPLACE "/+$" "" _base "${GRYCE_DEP_GIT_BASE}")
    set(${out_var} "${_base}/${org}/${repo}.git" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# GLFW：窗口与输入
# ---------------------------------------------------------------------------
function(gryce_fetch_glfw)
    if(TARGET glfw)
        return()
    endif()
    message(STATUS "Fetching GLFW (system package not found)")
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_INSTALL  OFF CACHE BOOL "" FORCE)
    # 3.4 有 GLFW_LIBRARY_TYPE；3.x 早期版本忽略它、由 BUILD_SHARED_LIBS 决定
    set(GLFW_LIBRARY_TYPE   SHARED CACHE STRING "" FORCE)
    gryce_repo_url(_url glfw glfw)
    FetchContent_Declare(glfw
        GIT_REPOSITORY  "${_url}"
        GIT_TAG         3.4
        GIT_SHALLOW     TRUE
        GIT_PROGRESS    TRUE
        GIT_SUBMODULES  "")
    FetchContent_MakeAvailable(glfw)
    if(TARGET glfw AND NOT TARGET glfw3)
        add_library(glfw3 ALIAS glfw)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# GLEW：OpenGL 扩展加载
# ---------------------------------------------------------------------------
function(gryce_fetch_glew)
    if(TARGET GLEW::GLEW OR TARGET glew)
        return()
    endif()
    message(STATUS "Fetching GLEW (system package not found)")
    set(BUILD_UTILS OFF CACHE BOOL "" FORCE)   # glewinfo/visualinfo 需要额外依赖
    # GLEW 2.2 的 build/cmake/CMakeLists.txt 声明 cmake_minimum_required(<3.5)，
    # CMake 4 已移除对 <3.5 的兼容，会直接报错；放低下限让它继续（CMake <3.31 忽略此变量）
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
        "老第三方库声明的 cmake_minimum_required 过低时的兼容下限" FORCE)
    # 注意：不能用 git 仓库。GLEW 的 include/GL/{glew,wglew,glxew}.h 是构建期
    # 生成的，git 仓库里没有（只有官方 release 包里有），直接 clone 会报
    # "Cannot find source file: include/GL/wglew.h"。故改用官方 release 包。
    string(REGEX REPLACE "/+$" "" _base "${GRYCE_DEP_GIT_BASE}")
    FetchContent_Declare(glew
        URL             "${_base}/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.tgz"
        URL_HASH        SHA256=d4fc82893cfb00109578d0a1a2337fb8ca335b3ceccf97b97e5cc7f08e4353e1
        SOURCE_SUBDIR   build/cmake)
    FetchContent_MakeAvailable(glew)
    if(TARGET glew AND NOT TARGET GLEW::GLEW)
        add_library(GLEW::GLEW ALIAS glew)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Box2D：2D 物理
# ---------------------------------------------------------------------------
function(gryce_fetch_box2d)
    if(TARGET box2d::box2d OR TARGET box2d)
        return()
    endif()
    message(STATUS "Fetching Box2D (system package not found)")
    set(BOX2D_SAMPLES      OFF CACHE BOOL "" FORCE)
    set(BOX2D_UNIT_TESTS   OFF CACHE BOOL "" FORCE)
    set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS  ON  CACHE BOOL "" FORCE)
    gryce_repo_url(_url erincatto box2d)
    FetchContent_Declare(box2d
        GIT_REPOSITORY  "${_url}"
        GIT_TAG         v3.1.1
        GIT_SHALLOW     TRUE
        GIT_PROGRESS    TRUE
        GIT_SUBMODULES  "")
    FetchContent_MakeAvailable(box2d)
    if(TARGET box2d AND NOT TARGET box2d::box2d)
        add_library(box2d::box2d ALIAS box2d)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Jolt Physics：3D 物理（源码较大，编译最耗时的一项）
# ---------------------------------------------------------------------------
function(gryce_fetch_jolt)
    if(TARGET Jolt::Jolt OR TARGET Jolt)
        return()
    endif()
    message(STATUS "Fetching Jolt Physics (system package not found)")
    message(STATUS "  Jolt 仓库较大，下载失败多见于网络受限：可换镜像 "
                   "-DGRYCE_DEP_GIT_BASE=<镜像前缀>，或安装系统包 "
                   "mingw-w64-ucrt-x86_64-jolt-physics 后用 -DGRYCE_DEP_FETCH=AUTO")
    set(TARGET_HELLO_WORLD     OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES         OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER          OFF CACHE BOOL "" FORCE)
    set(TARGET_UNIT_TESTS      OFF CACHE BOOL "" FORCE)
    set(TARGET_DOCS            OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)  # 关 LTO，省一半时间
    set(BUILD_SHARED_LIBS      ON  CACHE BOOL "" FORCE)
    gryce_repo_url(_url jrouwe JoltPhysics)
    # JoltPhysics 的 CMake 入口在 Build/ 子目录（仓库根没有 CMakeLists.txt），
    # 不指定 SOURCE_SUBDIR 会拿到一个没有任何 target 的空目录。
    FetchContent_Declare(Jolt
        GIT_REPOSITORY  "${_url}"
        GIT_TAG         v5.3.0
        GIT_SHALLOW     TRUE
        GIT_PROGRESS    TRUE
        GIT_SUBMODULES  ""
        SOURCE_SUBDIR   Build)
    FetchContent_MakeAvailable(Jolt)
    if(TARGET Jolt AND NOT TARGET Jolt::Jolt)
        add_library(Jolt::Jolt ALIAS Jolt)
    endif()
    # Jolt 5.x 面向 C++20；宿主工程用 C++26，锁回 20 避免标准过新导致编译问题
    if(TARGET Jolt)
        set_target_properties(Jolt PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    endif()
endfunction()
