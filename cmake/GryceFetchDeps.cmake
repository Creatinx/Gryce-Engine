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
#   -DGRYCE_DEP_FETCH=AUTO   默认。系统包优先，缺失项才下载源码构建
#   -DGRYCE_DEP_FETCH=ON     总是用源码构建（忽略系统包）
#   -DGRYCE_DEP_FETCH=OFF    只用系统包（离线/受控环境）
#   -DGRYCE_DEP_GIT_BASE=<前缀>  手动指定源码地址前缀。留空=自动探测
#                            （先直连 GitHub，不通则自动换镜像）
#
# 为什么用 tarball 而不是 git clone：
#   git 的智能 HTTP 协议在一次 clone 里要发多轮请求，穿过代理/加速器时极易被
#   掐断（Jolt 全量 clone 约 61MB，实测反复 "Connection was reset"；而它的
#   tarball 只有 18MB，且是一次性 HTTPS GET）。tarball 还能用 SHA256 锁定内容，
#   不依赖 git 是否安装，也不需要 --depth 之类的浅克隆技巧。
# ===========================================================================
include_guard(GLOBAL)

include(FetchContent)

set(GRYCE_DEP_FETCH "AUTO" CACHE STRING
    "缺失依赖时用 FetchContent 下载源码构建：AUTO / ON / OFF")
set_property(CACHE GRYCE_DEP_FETCH PROPERTY STRINGS AUTO ON OFF)
set(GRYCE_DEP_GIT_BASE "" CACHE STRING
    "依赖源码地址前缀（末尾保留 /）。留空则自动探测：直连 GitHub 不通时自动换镜像")
set(GRYCE_DEP_DOWNLOAD_RETRIES 3 CACHE STRING
    "单个源码包下载失败后的重试次数")

# 下载时打印进度：首次配置会拉源码，静默会让人以为卡死
set(FETCHCONTENT_QUIET OFF CACHE BOOL "" FORCE)
# 源码已经下载过就跳过 update：否则每次重新配置都要走一遍网络，
# 网络受限时会长时间卡住
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

if(NOT FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
endif()

# ---------------------------------------------------------------------------
# 源码地址前缀解析
#
# GitHub 在某些网络环境（代理 / 加速器）下会直接不可用：CONNECT 被拒 502，
# 或 clone 中途 connection reset。这里用一个极小文件（约 1KB）探测候选前缀，
# 挑第一个真正能连通的，结果缓存到 GRYCE_DEP_SOURCE_BASE_RESOLVED，
# 后续配置不再重复探测。
# ---------------------------------------------------------------------------
set(GRYCE_DEP_SOURCE_CANDIDATES
    "https://github.com/"
    "https://ghfast.top/https://github.com/"
    "https://gh-proxy.com/https://github.com/"
    "https://gh.llkk.cc/https://github.com/")
# 探测目标：体积极小、稳定存在的公开仓库归档
set(_GRYCE_BASE_PROBE "octocat/Hello-World/archive/refs/heads/master.tar.gz")

function(gryce_resolve_source_base out_var)
    # 注意：不能只凭"GRYCE_DEP_GIT_BASE 非空"就跳过探测。该缓存变量的默认值
    # 曾经是 https://github.com/，老构建目录里会残留这个值，而它恰恰是最需要
    # 走镜像兜底的情况。只有当用户设了一个**非默认**前缀时才无条件采用。
    list(GET GRYCE_DEP_SOURCE_CANDIDATES 0 _default_base)

    # 1. 用户显式指定了自定义前缀：无条件采用，不做探测
    if(GRYCE_DEP_GIT_BASE)
        string(REGEX REPLACE "/+$" "" _base "${GRYCE_DEP_GIT_BASE}")
        string(APPEND _base "/")
        if(NOT _base STREQUAL _default_base)
            set(GRYCE_DEP_SOURCE_BASE_RESOLVED "${_base}" CACHE INTERNAL "已解析的依赖源码地址前缀")
            set(${out_var} "${_base}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # 2. 之前探测过：直接用
    if(GRYCE_DEP_SOURCE_BASE_RESOLVED)
        set(${out_var} "${GRYCE_DEP_SOURCE_BASE_RESOLVED}" PARENT_SCOPE)
        return()
    endif()

    # 3. 逐个探测
    set(_probe_file "${FETCHCONTENT_BASE_DIR}/.gryce-base-probe")
    set(_chosen "")
    foreach(_cand IN LISTS GRYCE_DEP_SOURCE_CANDIDATES)
        message(STATUS "Probing dependency source base: ${_cand}")
        file(DOWNLOAD "${_cand}${_GRYCE_BASE_PROBE}" "${_probe_file}"
             STATUS _st TIMEOUT 25)
        list(GET _st 0 _code)
        if(_code EQUAL 0)
            set(_chosen "${_cand}")
            break()
        endif()
    endforeach()
    file(REMOVE "${_probe_file}")

    if(NOT _chosen)
        # 全都不通：仍用直连，让随后真实的下载报出原始错误，信息更完整
        list(GET GRYCE_DEP_SOURCE_CANDIDATES 0 _chosen)
        message(WARNING
            "No dependency source base is reachable (GitHub and all mirrors failed).\n"
            "The download below will most likely fail. If you have a working proxy or\n"
            "mirror, pass it with -DGRYCE_DEP_GIT_BASE=<prefix>, or install the system\n"
            "packages instead (see README).")
    endif()

    set(GRYCE_DEP_SOURCE_BASE_RESOLVED "${_chosen}" CACHE INTERNAL "已解析的依赖源码地址前缀")
    message(STATUS "Dependency source base: ${_chosen}")
    set(${out_var} "${_chosen}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# gryce_fetch_tarball(<名字> <相对路径> <sha256> <输出变量>)
#
# 把源码包下载到构建目录下的持久缓存（${FETCHCONTENT_BASE_DIR}/tarballs），
# 带重试 + SHA256 校验；已存在且校验通过则直接复用。随后 FetchContent 以本地
# 文件作为 URL，ExternalProject 就不再走网络，重配置也不会重复下载。
# ---------------------------------------------------------------------------
function(gryce_fetch_tarball name rel_path hash out_var)
    gryce_resolve_source_base(_base)
    set(_url  "${_base}${rel_path}")
    set(_dir  "${FETCHCONTENT_BASE_DIR}/tarballs")
    set(_dst  "${_dir}/${name}.tar.gz")
    set(_part "${_dst}.part")
    file(MAKE_DIRECTORY "${_dir}")

    # 已下载且校验通过 -> 复用
    if(EXISTS "${_dst}")
        file(SHA256 "${_dst}" _have)
        if(_have STREQUAL hash)
            message(STATUS "Reusing cached ${name} tarball: ${_dst}")
            set(${out_var} "${_dst}" PARENT_SCOPE)
            return()
        endif()
        message(STATUS "Cached ${name} tarball failed its checksum, re-downloading")
        file(REMOVE "${_dst}")
    endif()

    set(_ok FALSE)
    foreach(_attempt RANGE 1 ${GRYCE_DEP_DOWNLOAD_RETRIES})
        message(STATUS "Downloading ${name} (attempt ${_attempt}/${GRYCE_DEP_DOWNLOAD_RETRIES}): ${_url}")
        file(DOWNLOAD "${_url}" "${_part}"
             STATUS _st SHOW_PROGRESS TIMEOUT 900
             EXPECTED_HASH SHA256=${hash})
        list(GET _st 0 _code)
        if(_code EQUAL 0)
            file(RENAME "${_part}" "${_dst}")
            set(_ok TRUE)
            break()
        endif()
        list(GET _st 1 _msg)
        message(STATUS "  download failed: ${_msg}")
        file(REMOVE "${_part}")
    endforeach()

    if(NOT _ok)
        message(FATAL_ERROR
            "Failed to download ${name} from ${_url}\n"
            "Network is the usual cause. Options:\n"
            "  1. Use a reachable mirror:  -DGRYCE_DEP_GIT_BASE=https://ghfast.top/https://github.com/\n"
            "  2. Install the system package instead (see README) and re-run with -DGRYCE_DEP_FETCH=AUTO\n"
            "  3. Drop a matching tarball at ${_dst} manually (SHA256 must be ${hash})")
    endif()

    set(${out_var} "${_dst}" PARENT_SCOPE)
endfunction()

function(gryce_fetch_enabled out_var)
    if(GRYCE_DEP_FETCH STREQUAL "OFF")
        set(${out_var} FALSE PARENT_SCOPE)
    else()
        set(${out_var} TRUE PARENT_SCOPE)
    endif()
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
    gryce_fetch_tarball(glfw "glfw/glfw/archive/refs/tags/3.4.tar.gz"
        "c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01" _pkg)
    FetchContent_Declare(glfw
        URL         "${_pkg}"
        URL_HASH    SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01)
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
    # "Cannot find source file: include/GL/wglew.h"。故用官方 release 包。
    gryce_fetch_tarball(glew "nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.tgz"
        "d4fc82893cfb00109578d0a1a2337fb8ca335b3ceccf97b97e5cc7f08e4353e1" _pkg)
    FetchContent_Declare(glew
        URL         "${_pkg}"
        URL_HASH    SHA256=d4fc82893cfb00109578d0a1a2337fb8ca335b3ceccf97b97e5cc7f08e4353e1
        SOURCE_SUBDIR build/cmake)
    FetchContent_MakeAvailable(glew)
    # GLEW 2.2 的 CMake 用的是**目录级** include_directories()，且 target 上只挂了
    # $<INSTALL_INTERFACE:include>，没有 $<BUILD_INTERFACE>。于是 link glew 不会传递
    # 头文件目录，下游会报 "GL/glew.h: No such file or directory"。显式补上构建期目录。
    if(TARGET glew)
        # 必须用 $<BUILD_INTERFACE:> 包起来：GLEW 的 target 参与 install(EXPORT)，
        # 直接挂绝对路径会被 CMake 的导出校验拒绝（"prefixed in the build directory"）。
        target_include_directories(glew PUBLIC
            "$<BUILD_INTERFACE:${glew_SOURCE_DIR}/include>")
    endif()
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
    gryce_fetch_tarball(box2d "erincatto/box2d/archive/refs/tags/v3.1.1.tar.gz"
        "fb6ef914b50f4312d7d921a600eabc12318bb3c55a0b8c0b90608fa4488ef2e4" _pkg)
    FetchContent_Declare(box2d
        URL         "${_pkg}"
        URL_HASH    SHA256=fb6ef914b50f4312d7d921a600eabc12318bb3c55a0b8c0b90608fa4488ef2e4)
    FetchContent_MakeAvailable(box2d)
    if(TARGET box2d AND NOT TARGET box2d::box2d)
        add_library(box2d::box2d ALIAS box2d)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Jolt Physics：3D 物理（源码最大的一项）
# ---------------------------------------------------------------------------
function(gryce_fetch_jolt)
    if(TARGET Jolt::Jolt OR TARGET Jolt)
        return()
    endif()
    message(STATUS "Fetching Jolt Physics (system package not found)")
    set(TARGET_HELLO_WORLD     OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES         OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER          OFF CACHE BOOL "" FORCE)
    set(TARGET_UNIT_TESTS      OFF CACHE BOOL "" FORCE)
    set(TARGET_DOCS            OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)  # 关 LTO，省一半时间
    set(BUILD_SHARED_LIBS      ON  CACHE BOOL "" FORCE)
    # JoltPhysics 的 CMake 入口在 Build/ 子目录（仓库根没有 CMakeLists.txt），
    # 不指定 SOURCE_SUBDIR 会拿到一个没有任何 target 的空目录。
    gryce_fetch_tarball(Jolt "jrouwe/JoltPhysics/archive/refs/tags/v5.3.0.tar.gz"
        "e7f9621e480646c434150e1fbe3a9410f4ec4b04ffe54791e0678326b741b918" _pkg)
    FetchContent_Declare(Jolt
        URL         "${_pkg}"
        URL_HASH    SHA256=e7f9621e480646c434150e1fbe3a9410f4ec4b04ffe54791e0678326b741b918
        SOURCE_SUBDIR Build)
    FetchContent_MakeAvailable(Jolt)
    if(TARGET Jolt AND NOT TARGET Jolt::Jolt)
        add_library(Jolt::Jolt ALIAS Jolt)
    endif()
    # Jolt 5.x 面向 C++20；宿主工程用 C++26，锁回 20 避免标准过新导致编译问题
    if(TARGET Jolt)
        set_target_properties(Jolt PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    endif()
endfunction()
