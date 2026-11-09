# ---------------------------------------------------------------------------
# GryceDefaultCompiler.cmake — 引擎默认编译器 = MSYS2 UCRT64 g++
#
# 必须在 project() 之前 include()。Windows 桌面构建时：
#   * 未指定编译器 → 默认指向 MSYS2 UCRT64，杜绝 cmake 在 cmd/PowerShell/
#     CLion 下默认挑中 MSVC（缺 rc.exe、与 MinGW 依赖库不兼容，链不通）；
#   * 已指定 MSVC（含旧缓存里记着 cl.exe 的情形，如 CLion 重配）→ 覆盖回
#     MSYS2 并清掉陈旧的编译器探测产物（CMakeFiles），否则 configure
#     必死在 rc.exe 上；
#   * 已显式指定非 MSVC 编译器 → 一律尊重（"除非用户改"）。
#
# 覆盖缓存里的 MSVC 后，构建目录里可能还残留 MSVC ABI 的旧目标文件，
# 若链接报 ABI/编译器不一致，删掉该构建目录重配一次即可。
#
# 已在 project() 之后（语言已启用）或非 Windows/Emscripten/vcpkg 等
# 工具链文件场景下，本文件直接返回，零副作用。
# ---------------------------------------------------------------------------

if(NOT WIN32 OR EMSCRIPTEN OR DEFINED CMAKE_TOOLCHAIN_FILE)
    return()
endif()

set(_gryce_ucrt "D:/msys64/ucrt64/bin")
if(NOT EXISTS "${_gryce_ucrt}/c++.exe")
    message(WARNING
        "Gryce: 未找到 MSYS2 UCRT64 (${_gryce_ucrt}/c++.exe)。\n"
        "       引擎只支持 MSYS2 UCRT64 工具链；请安装 MSYS2 或用\n"
        "       -DCMAKE_C_COMPILER / -DCMAKE_CXX_COMPILER 显式指定。")
    return()
endif()

# 判定污染（命令行或旧缓存），三个来源任一命中就全清重配：
#   1. 编译器是 MSVC（cl.exe / cl）；
#   2. 编译器已是 g++ 但 flags 仍是 MSVC 风格（失败重配时缓存写回了 g++
#      却残留 /DWIN32 等 flags）；
#   3. 编译器规则文件（CMakeFiles/<ver>/CMake*CCompiler.cmake）里残留
#      MSVC 工具链字样——最隐蔽：cache 看着干净，但 CMake 因「编译器没变」
#      跳过重新探测，复用带毒规则（症状：打包静态库用了 lib.exe）。
# 干净的非 MSVC 显式值 → 尊重并退出。
set(_gryce_replace_msvc OFF)
if(DEFINED CMAKE_CXX_COMPILER OR DEFINED CMAKE_C_COMPILER)
    set(_gryce_msvc OFF)
    foreach(_gryce_v "${CMAKE_CXX_COMPILER}" "${CMAKE_C_COMPILER}")
        string(TOLOWER "${_gryce_v}" _gryce_lc)
        if(_gryce_lc MATCHES "cl\\.exe$" OR _gryce_lc STREQUAL "cl")
            set(_gryce_msvc ON)
        endif()
    endforeach()
    if(_gryce_msvc)
        set(_gryce_replace_msvc ON)
    else()
        # flags 污染检测：MSVC 风格选项以 / 开头（/DWIN32 /Zi /RTC1…），
        # GCC 只会把它当输入文件。
        foreach(_gryce_v "${CMAKE_C_FLAGS}" "${CMAKE_CXX_FLAGS}"
                         "${CMAKE_C_FLAGS_DEBUG}" "${CMAKE_CXX_FLAGS_DEBUG}")
            if(_gryce_v MATCHES "(^|[ \t])/[A-Za-z]")
                set(_gryce_replace_msvc ON)
            endif()
        endforeach()
    endif()
endif()
if(NOT _gryce_replace_msvc)
    # 规则文件残留检测：只查 CMake 自己生成的 CMake*CCompiler.cmake，
    # 正常 GNU 配置不可能包含这些字样。
    file(GLOB _gryce_rulefiles "${CMAKE_BINARY_DIR}/CMakeFiles/${CMAKE_VERSION}/CMake*CCompiler.cmake")
    foreach(_gryce_rf ${_gryce_rulefiles})
        if(EXISTS "${_gryce_rf}")
            file(READ "${_gryce_rf}" _gryce_txt)
            if(_gryce_txt MATCHES "cl\\.exe|lib\\.exe|link\\.exe|Microsoft Visual Studio")
                set(_gryce_replace_msvc ON)
                break()
            endif()
        endif()
    endforeach()
    if(NOT _gryce_replace_msvc)
        return()
    endif()
    unset(_gryce_rulefiles)
    unset(_gryce_rf)
    unset(_gryce_txt)
endif()

if(_gryce_replace_msvc)
    message(STATUS
        "Gryce: 检测到 MSVC 编译器/残留 flags/带毒规则文件（引擎不支持），"
        "已清理重配为 MSYS2 UCRT64 工具链。")
    # CMakeFiles/ 里是陈旧的编译器探测结果；CMakeCache.txt 里还残留 MSVC 时代
    # 写入的 CMAKE_C(XX)_FLAGS（/DWIN32 之类，g++ 收到必炸）。两个都删，让
    # 本次 configure 用干净的内存缓存重新初始化（命令行 -D 参数仍在内存里，不丢）。
    # 注意：file(REMOVE CMakeCache.txt) 只删磁盘文件，**内存缓存还在**——
    # CMAKE_AR=lib.exe、LINK_FLAGS=/machine:x64 这类就是从内存里漏过去的
    #（症状：g++ 编译全过，打包静态库却调 lib.exe）。所以 flags 之外，
    # 归档器/链接器/RC/MT 系列变量也必须逐一 unset(CACHE)。
    file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/CMakeFiles")
    file(REMOVE "${CMAKE_CACHEFILE_DIR}/CMakeCache.txt")
    foreach(_gryce_flagvar
            CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_EXE_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
            CMAKE_C_LINK_FLAGS CMAKE_CXX_LINK_FLAGS
            CMAKE_AR CMAKE_RANLIB CMAKE_LINKER CMAKE_MT CMAKE_RC_COMPILER CMAKE_RC_FLAGS
            CMAKE_C_COMPILER_AR CMAKE_C_COMPILER_RANLIB CMAKE_C_COMPILER_LINKER
            CMAKE_CXX_COMPILER_AR CMAKE_CXX_COMPILER_RANLIB CMAKE_CXX_COMPILER_LINKER
            CMAKE_GNUtoMS)
        unset(${_gryce_flagvar} CACHE)
    endforeach()
    unset(_gryce_flagvar)
else()
    message(STATUS "Gryce: 默认使用 MSYS2 UCRT64 工具链（-DCMAKE_CXX_COMPILER 可覆盖）")
endif()

set(CMAKE_C_COMPILER   "${_gryce_ucrt}/cc.exe"  CACHE FILEPATH "MSYS2 UCRT64 cc (engine default)" FORCE)
set(CMAKE_CXX_COMPILER "${_gryce_ucrt}/c++.exe" CACHE FILEPATH "MSYS2 UCRT64 c++ (engine default)" FORCE)
unset(_gryce_ucrt)
unset(_gryce_v)
unset(_gryce_lc)
unset(_gryce_msvc)
unset(_gryce_replace_msvc)
