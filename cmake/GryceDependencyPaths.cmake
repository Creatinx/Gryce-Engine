# ===========================================================================
# cmake/GryceDependencyPaths.cmake
#
# 可移植依赖定位：仓库内不再出现任何机器专属的绝对路径（如 D:/msys64/ucrt64）。
#
# 本模块按下列顺序推断"依赖前缀"候选，并写入 CMAKE_PREFIX_PATH，之后所有
# find_package / find_path / find_library / find_file 都能自动命中：
#
#   1. -DGRYCE_DEP_PREFIX=<prefix>        显式指定（CI、非标准布局、多工具链并存）
#   2. $ENV{GRYCE_DEP_PREFIX}             同名环境变量
#   3. $ENV{CMAKE_PREFIX_PATH}            CMake 原生环境变量（分号分隔）
#   4. $ENV{MSYS2_ROOT} + $ENV{MSYSTEM}   MSYS2：D:/msys64 + UCRT64 -> D:/msys64/ucrt64
#   5. $ENV{MSYSTEM_PREFIX}/$ENV{MINGW_PREFIX}  MSYS2 自带变量（POSIX 风格 /ucrt64，
#                                        用 cygpath 转成原生路径）
#   6. 编译器路径反推：<root>/<msystem>/bin/g++.exe -> <root>/<msystem>
#      覆盖 MSYS2(ucrt64/mingw64/clang64)、CLion 自带 MinGW、独立 MinGW-w64
#
# 全部候选只是"提示"：找不到时 CMake 仍会回退到系统默认搜索路径（MSYS2 的
# mingw-w64 版 CMake 本身就认识自己的前缀），所以装好依赖即可开箱编译。
# ===========================================================================
include_guard(GLOBAL)

set(_gryce_raw_prefixes "")

# --- 1/2. 显式覆盖 ---------------------------------------------------------
if(GRYCE_DEP_PREFIX)
    list(APPEND _gryce_raw_prefixes "${GRYCE_DEP_PREFIX}")
endif()
if(DEFINED ENV{GRYCE_DEP_PREFIX})
    list(APPEND _gryce_raw_prefixes "$ENV{GRYCE_DEP_PREFIX}")
endif()

# --- 3. CMake 原生环境变量（可能是多个，分号分隔）---------------------------
if(DEFINED ENV{CMAKE_PREFIX_PATH})
    list(APPEND _gryce_raw_prefixes $ENV{CMAKE_PREFIX_PATH})
endif()

# --- 4. MSYS2：MSYS2_ROOT + MSYSTEM ----------------------------------------
if(DEFINED ENV{MSYS2_ROOT} AND DEFINED ENV{MSYSTEM})
    string(TOLOWER _gryce_msystem "$ENV{MSYSTEM}")
    list(APPEND _gryce_raw_prefixes "$ENV{MSYS2_ROOT}/${_gryce_msystem}")
endif()

# --- 5. MSYS2 自带的 MSYSTEM_PREFIX / MINGW_PREFIX（POSIX 风格）------------
foreach(_gryce_var IN ITEMS MSYSTEM_PREFIX MINGW_PREFIX)
    if(DEFINED ENV{${_gryce_var}})
        list(APPEND _gryce_raw_prefixes "$ENV{${_gryce_var}}")
    endif()
endforeach()
unset(_gryce_var)

# --- 6. 编译器路径反推 -----------------------------------------------------
# D:/msys64/ucrt64/bin/g++.exe -> D:/msys64/ucrt64
# D:/JetBrains/CLion/bin/mingw/bin/c++.exe -> D:/JetBrains/CLion/bin/mingw
foreach(_gryce_cc IN ITEMS "${CMAKE_CXX_COMPILER}" "${CMAKE_C_COMPILER}")
    if(_gryce_cc)
        get_filename_component(_gryce_bin "${_gryce_cc}" DIRECTORY)
        get_filename_component(_gryce_pfx "${_gryce_bin}" DIRECTORY)
        if(_gryce_pfx)
            list(APPEND _gryce_raw_prefixes "${_gryce_pfx}")
        endif()
    endif()
endforeach()
unset(_gryce_cc)
unset(_gryce_bin)
unset(_gryce_pfx)

# --- 归一化：POSIX -> 原生路径、去重、丢弃不存在的目录 ---------------------
find_program(GRYCE_CYGPATH NAMES cygpath)

set(GRYCE_DEP_PREFIXES "")
foreach(_gryce_raw IN LISTS _gryce_raw_prefixes)
    if(NOT _gryce_raw)
        continue()
    endif()
    set(_gryce_p "${_gryce_raw}")
    # MSYS2 的 /ucrt64 这类 POSIX 路径只有在有 cygpath 时才能可靠映射到盘符
    if(_gryce_p MATCHES "^/" AND GRYCE_CYGPATH)
        execute_process(COMMAND "${GRYCE_CYGPATH}" -w -a "${_gryce_p}"
                        OUTPUT_VARIABLE _gryce_p
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
    endif()
    if(NOT _gryce_p)
        continue()
    endif()
    cmake_path(SET _gryce_p NORMALIZE "${_gryce_p}")
    if(IS_DIRECTORY "${_gryce_p}" AND NOT _gryce_p IN_LIST GRYCE_DEP_PREFIXES)
        list(APPEND GRYCE_DEP_PREFIXES "${_gryce_p}")
    endif()
endforeach()
unset(_gryce_raw)
unset(_gryce_p)
unset(_gryce_raw_prefixes)

# --- 注入搜索路径 ----------------------------------------------------------
# CMAKE_PREFIX_PATH 会被 find_package(CONFIG) 展开为 <prefix>/lib/cmake/<pkg>，
# 被 find_*(PATH) 展开为 <prefix>/include、<prefix>/lib、<prefix>/bin 等，
# 因此后续所有依赖查找都不需要再写死路径。
if(GRYCE_DEP_PREFIXES)
    list(PREPEND CMAKE_PREFIX_PATH ${GRYCE_DEP_PREFIXES})
    list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
endif()

if(GRYCE_DEP_PREFIXES)
    message(STATUS "Dependency search prefixes: ${GRYCE_DEP_PREFIXES}")
else()
    message(STATUS "Dependency search prefixes: <none detected, using CMake defaults>"
                   " (hint: set -DGRYCE_DEP_PREFIX=<prefix> or the GRYCE_DEP_PREFIX env var)")
endif()

# ===========================================================================
# gryce_dependency_hint(<依赖名>)
# 依赖缺失时给出面向"任意机器"的排查建议，而不是某台机器的路径。
# ===========================================================================
function(gryce_dependency_hint _name)
    set(_msg "${_name} not found.")
    if(GRYCE_DEP_PREFIXES)
        string(REPLACE ";" "\n  - " _list "${GRYCE_DEP_PREFIXES}")
        string(APPEND _msg "\nSearched prefixes:\n  - ${_list}")
    else()
        string(APPEND _msg "\nNo dependency prefix was detected from the toolchain.")
    endif()
    string(APPEND _msg
        "\nHow to fix (pick one):"
        "\n  1. MSYS2 UCRT64: pacman -S --needed mingw-w64-ucrt-x86_64-<package>"
        "\n  2. Point CMake at your toolchain prefix:"
        "\n       cmake -S . -B build -DGRYCE_DEP_PREFIX=<prefix>"
        "\n     or export GRYCE_DEP_PREFIX=<prefix>  (MSYS2: /ucrt64, CLion MinGW: <CLion>/bin/mingw)"
        "\n  3. Or use the standard CMake variable: CMAKE_PREFIX_PATH=<prefix>")
    message(FATAL_ERROR "${_msg}")
endfunction()
