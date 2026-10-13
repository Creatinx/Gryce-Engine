cmake_minimum_required(VERSION 3.24)

project(GryceEngineRenderer
        VERSION 0.1.0
        DESCRIPTION "Gryce Engine Renderer: GryceCore + GryceRenderer + GrycePlatform"
        LANGUAGES C CXX)

option(GRYCE_ENABLE_IMGUI "Enable ImGui editor overlay in GryceRenderer" ON)

# ===========================================================================
# 依赖定位（可移植，不含任何机器专属绝对路径）
# cmake/GryceDependencyPaths.cmake 按 GRYCE_DEP_PREFIX / MSYS2_ROOT+MSYSTEM /
# MSYSTEM_PREFIX / 编译器路径 逐级推断候选前缀，写入 CMAKE_PREFIX_PATH。
# 之后的 find_* 全部不再写死路径：把 MSYS2 装在任意盘符、用 CLion 自带
# MinGW、或用 vcpkg/自定义前缀，都能直接配置。
# ===========================================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GryceDependencyPaths)
# 依赖缺失时的 FetchContent 兜底（CMake 不会自动下载依赖，必须显式声明）
include(GryceFetchDeps)

# ===========================================================================
# 全局编译选项
# ===========================================================================
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
if(NOT CMAKE_CONFIGURATION_TYPES)
    # Debug/-O0 下 GCC16+MinGW 会生成导致 symbol/link 异常的对象，参考工程用 -O1 -g1
    set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g1 -fno-omit-frame-pointer")
    # 注意：link_libraries() 是目录级作用域的，写在这里会同样加到 Release 的链接行。
    # 链接期 -O1 在启用 LTO（GRYCE_ENABLE_LTO）时会反过来覆盖 LTO 的代码生成级别，
    # 把 Release 的 -O3 悄悄拉回 -O1，故此处必须限定 Debug。
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        link_libraries(-O1 -g1)
    endif()
endif()

# ===========================================================================
# 目标 ISA：x86-64-v3（AVX2 / FMA / BMI1 / F16C / LZCNT / POPCNT）
#
# 这组标志此前并非由本工程声明，而是依赖（Jolt，其 CMake 选项 USE_AVX2/USE_FMADD
# 为 ON）在配置阶段把 -mavx2 ... 追加进 CMAKE_CXX_FLAGS 后"泄漏"到引擎目标的：
# 源码树里搜不到，却出现在 285/287 个编译单元的命令行上。后果是**不可复现** ——
# 一旦依赖改走 find_package(CONFIG)、更换依赖前缀或不再由源码构建，重新 configure
# 就会静默丢失，表现为"什么都没改，帧时间却变差"。这里显式固化，Debug/Release
# 一致；需要跑在老于 Haswell 的机器上时用 -DGRYCE_ENABLE_X86_V3=OFF 关闭。
# ===========================================================================
option(GRYCE_ENABLE_X86_V3 "启用 x86-64-v3 目标 ISA（AVX2/FMA/BMI/F16C/LZCNT/POPCNT）" ON)
if(GRYCE_ENABLE_X86_V3 AND NOT MSVC
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|X64)$")
    add_compile_options(-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse)
endif()

# ===========================================================================
# LTO：引擎按 DLL 分模块（GryceCore / GryceRenderer / GrycePlatform），跨 DLL
# 不内联，但单个 DLL 内部跨 TU 内联与去虚拟化仍然有效，对 math/render 这类
# 大量小函数（Vector3f/Matrix4f 运算符、包围盒变换、剔除）收益明显。
# Debug 下关闭：LTO 会显著拖慢增量编译，且 Debug 本就不看性能。
# ===========================================================================
option(GRYCE_ENABLE_LTO "对非 Debug 构建启用过程间优化（LTO）" OFF)
if(GRYCE_ENABLE_LTO AND NOT MSVC AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# 使用 LLVM lld 链接，避免 GNU ld 处理大型对象崩溃
find_program(LLD_LINKER ld.lld)
if(LLD_LINKER)
    add_link_options(-fuse-ld=lld)
endif()

# ===========================================================================
# GLFW：优先用环境里已安装的 GLFW（MSYS2 / vcpkg / CLion MinGW / 自定义前缀），
# 找不到时按 GRYCE_DEP_FETCH 策略用 FetchContent 拉源码构建（默认 AUTO）。
# 共享库语义：引擎 DLL 与可执行文件各自调用 GLFW，动态库由 OS 加载一次，
# 故优先挑 DLL 的导入库（libglfw3.dll.a、libglfw3dll.a），而不是静态 libglfw3.a。
# ===========================================================================
if(NOT GRYCE_DEP_FETCH STREQUAL "ON")
    find_path(GLFW3_INCLUDE_DIR GLFW/glfw3.h)
    set(_gryce_saved_lib_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
        # .dll.a 是 MinGW 的导入库，排在 .a 前面可避免误链静态库
        set(CMAKE_FIND_LIBRARY_SUFFIXES .dll.a .lib .a)
    endif()
    find_library(GLFW3_LIBRARY NAMES glfw3dll glfw3 glfw)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_gryce_saved_lib_suffixes})
    unset(_gryce_saved_lib_suffixes)
    # 运行期 DLL：MSYS2 在 <prefix>/bin，部分 MinGW 发行版放在 <prefix>/lib
    # 注意：不能先用 set() 预置哨兵值——find_file 见到同名普通变量会直接跳过搜索，
    # 结果变量永远停在哨兵上且能通过 if(NOT ...) 判空（历史 bug）。要重置请用 unset(CACHE)。
    unset(GLFW3_DLL CACHE)
    find_file(GLFW3_DLL glfw3.dll PATH_SUFFIXES bin lib)
endif()

if(GLFW3_INCLUDE_DIR AND GLFW3_LIBRARY AND GLFW3_DLL)
    add_library(glfw SHARED IMPORTED)
    set_target_properties(glfw PROPERTIES
        IMPORTED_LOCATION "${GLFW3_DLL}"
        IMPORTED_IMPLIB   "${GLFW3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GLFW3_INCLUDE_DIR}")
    message(STATUS "GLFW: system package at ${GLFW3_LIBRARY}")
else()
    gryce_fetch_enabled(_gryce_may_fetch)
    if(NOT _gryce_may_fetch)
        gryce_dependency_hint("GLFW (need GLFW/glfw3.h + glfw3 import library + glfw3.dll)")
    endif()
    gryce_fetch_glfw()
    if(NOT TARGET glfw)
        message(FATAL_ERROR "GLFW unavailable: neither a system package nor FetchContent succeeded")
    endif()
endif()

# ===========================================================================
# 物理依赖：Box2D（2D 刚体）+ Jolt Physics（3D 刚体）
# 两者都以 CMake 配置包形式安装（box2dConfig.cmake / JoltConfig.cmake），
# 导入目标为 box2d::box2d 与 Jolt::Jolt（均为共享库）。
# 搜索路径来自 CMAKE_PREFIX_PATH（见 GryceDependencyPaths），无需写死前缀。
# 配置包只导出 RELEASE 配置，Debug/RelWithDebInfo 构建经 MAP_IMPORTED_CONFIG
# 回退到 RELEASE 导入库，避免 "configuration not found" 报错。
# ===========================================================================
if(NOT GRYCE_DEP_FETCH STREQUAL "ON")
    find_package(box2d QUIET CONFIG)
    find_package(Jolt QUIET CONFIG)
endif()

gryce_fetch_enabled(_gryce_may_fetch)
if(NOT TARGET box2d::box2d)
    if(NOT _gryce_may_fetch)
        gryce_dependency_hint("Box2D (MSYS2: mingw-w64-ucrt-x86_64-box2d)")
    endif()
    gryce_fetch_box2d()
    if(NOT TARGET box2d::box2d)
        message(FATAL_ERROR "Box2D unavailable: neither a system package nor FetchContent succeeded")
    endif()
endif()
if(NOT TARGET Jolt::Jolt)
    if(NOT _gryce_may_fetch)
        gryce_dependency_hint("Jolt Physics (MSYS2: mingw-w64-ucrt-x86_64-jolt-physics)")
    endif()
    gryce_fetch_jolt()
    if(NOT TARGET Jolt::Jolt)
        message(FATAL_ERROR "Jolt Physics unavailable: neither a system package nor FetchContent succeeded")
    endif()
endif()

# 配置包只导出 RELEASE 配置：Debug/RelWithDebInfo 构建需要把导入库回退到
# RELEASE，否则报 "configuration not found"。该属性只对 IMPORTED 目标有效，
# FetchContent 构建出来的普通目标不能设（会报错），故先判断。
foreach(_t box2d::box2d Jolt::Jolt)
    get_target_property(_is_imported ${_t} IMPORTED)
    if(_is_imported)
        foreach(_cfg DEBUG RELWITHDEBINFO MINSIZEREL)
            set_target_properties(${_t} PROPERTIES MAP_IMPORTED_CONFIG_${_cfg} RELEASE)
        endforeach()
    endif()
endforeach()
unset(_t)
unset(_is_imported)
unset(_cfg)
if(box2d_DIR)
    message(STATUS "Box2D: ${box2d_DIR}")
endif()
if(Jolt_DIR)
    message(STATUS "Jolt Physics: ${Jolt_DIR}")
endif()

# ===========================================================================
# 模型导入：Assimp（FBX / glTF / DAE / PLY / STL / 3DS / Blend，OBJ 走引擎
# 内置 ObjLoader）。与 GLFW/GLEW 同策略：优先系统包（MSYS2
# mingw-w64-ucrt-x86_64-assimp / vcpkg 的 assimpConfig.cmake），缺失时按
# GRYCE_DEP_FETCH 拉源码静态编译。
# 与上面几项的区别：Assimp 是**可选**依赖——缺了只是非 OBJ 格式导入不可用
# （engine/assets/asset_manager.cpp 里由 GRYCE_HAS_ASSIMP 条件编译，OBJ 不受
# 影响），所以这里不 FATAL_ERROR，只告警降级。
# ===========================================================================
if(NOT GRYCE_DEP_FETCH STREQUAL "ON")
    find_package(assimp QUIET CONFIG)
endif()

gryce_fetch_enabled(_gryce_may_fetch)
if(NOT TARGET assimp::assimp)
    if(NOT _gryce_may_fetch)
        gryce_dependency_hint("Assimp (MSYS2: mingw-w64-ucrt-x86_64-assimp; optional, OBJ import works without it)")
    endif()
    gryce_fetch_assimp()
endif()

# 系统共享库（libassimp-5.dll）需要随第三方运行库同步到 exe 目录；
# FetchContent 源码构建出来的是静态库（TARGET_FILE 是 .a），不复制。
set(_gryce_assimp_sync_cmd "")
if(TARGET assimp::assimp)
    get_target_property(_gryce_assimp_imported assimp::assimp IMPORTED)
    if(_gryce_assimp_imported)
        set(_gryce_assimp_sync_cmd
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:assimp::assimp>" "$<TARGET_FILE_DIR:GryceRenderSmoke>")
    endif()
    unset(_gryce_assimp_imported)
    message(STATUS "Assimp: available (FBX/glTF/DAE/PLY/STL import enabled)")
else()
    message(WARNING
        "Assimp unavailable: non-OBJ model formats (FBX/glTF/DAE/PLY/STL) will not import.\n"
        "OBJ import via the built-in ObjLoader is unaffected.")
endif()

# ===========================================================================
# 音频依赖：Amplitude Audio SDK（SparkyStudios）
# 以预编译静态库形式随仓库分发（third_party/amplitude）：SDK 源码需 GCC16
# 补丁且构建耗时，故引擎侧只消费头文件 + libAmplitude.a 及其两个私有依赖
# （libflatbuffers.a / libsamplerate.a，静态库不会自动带入 PRIVATE 依赖）。
# ===========================================================================
set(AMPLITUDE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/amplitude)
if(NOT EXISTS ${AMPLITUDE_ROOT}/lib/libAmplitude.a)
    message(FATAL_ERROR "Amplitude SDK not vendored under third_party/amplitude")
endif()

add_library(amplitude STATIC IMPORTED)
set_target_properties(amplitude PROPERTIES
    IMPORTED_LOCATION "${AMPLITUDE_ROOT}/lib/libAmplitude.a"
    INTERFACE_INCLUDE_DIRECTORIES "${AMPLITUDE_ROOT}/include"
)
add_library(flatbuffers STATIC IMPORTED)
set_target_properties(flatbuffers PROPERTIES
    IMPORTED_LOCATION "${AMPLITUDE_ROOT}/lib/libflatbuffers.a"
)
add_library(samplerate STATIC IMPORTED)
set_target_properties(samplerate PROPERTIES
    IMPORTED_LOCATION "${AMPLITUDE_ROOT}/lib/libsamplerate.a"
)

find_package(Threads REQUIRED)
target_link_libraries(amplitude INTERFACE
    flatbuffers samplerate Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    # Amplitude 内置 miniaudio 后端在 Windows 上的系统依赖（WASAPI/DSound/MME）。
    target_link_libraries(amplitude INTERFACE ole32 uuid winmm user32 advapi32)
endif()

# ===========================================================================
# GLEW：OpenGL 扩展加载。优先用系统包，缺失时按 GRYCE_DEP_FETCH 拉源码构建。
# 两种情况都统一暴露成 GLEW::GLEW 目标，engine/ 只消费这个目标。
# ===========================================================================
if(NOT GRYCE_DEP_FETCH STREQUAL "ON")
    find_path(GLEW_INCLUDE_DIR GL/glew.h)
    find_library(GLEW_LIBRARY NAMES glew32 GLEW glew)
    unset(GLEW_DLL CACHE)
    find_file(GLEW_DLL glew32.dll PATH_SUFFIXES bin lib)
endif()

if(GLEW_INCLUDE_DIR AND GLEW_LIBRARY)
    if(GLEW_DLL)
        add_library(GLEW::GLEW SHARED IMPORTED)
        set_target_properties(GLEW::GLEW PROPERTIES
            IMPORTED_LOCATION "${GLEW_DLL}"
            IMPORTED_IMPLIB   "${GLEW_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GLEW_INCLUDE_DIR}")
    else()
        # 只有导入库、拿不到 DLL：用 UNKNOWN 类型，链接照常，只是不同步运行库
        add_library(GLEW::GLEW UNKNOWN IMPORTED)
        set_target_properties(GLEW::GLEW PROPERTIES
            IMPORTED_LOCATION "${GLEW_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GLEW_INCLUDE_DIR}")
    endif()
    message(STATUS "GLEW: system package at ${GLEW_LIBRARY}")
else()
    gryce_fetch_enabled(_gryce_may_fetch)
    if(NOT _gryce_may_fetch)
        gryce_dependency_hint("GLEW (MSYS2: mingw-w64-ucrt-x86_64-glew)")
    endif()
    gryce_fetch_glew()
    if(NOT TARGET GLEW::GLEW)
        message(FATAL_ERROR "GLEW unavailable: neither a system package nor FetchContent succeeded")
    endif()
endif()

# GryceRenderSmoke.exe 的运行时输出目录。engine/ 子目录里引擎 DLL 的
# POST_BUILD 拷贝命令需要它（那边无法引用尚未定义的 exe target，
# add_custom_command(TARGET) 也不允许跨目录挂命令，拷贝只能挂在 DLL 自身）。
# 默认不设 RUNTIME_OUTPUT_DIRECTORY，即落在 ${CMAKE_BINARY_DIR} 根下。
set(GRYCE_EXE_OUTPUT_DIR "${CMAKE_BINARY_DIR}")

# ===========================================================================
# 渲染引擎本体：engine/ 下的 GryceCore.dll / GryceRenderer.dll / GrycePlatform.dll
# （Vulkan SDK 由 engine/CMakeLists.txt 自行探测并设置 GRYCE_HAS_VULKAN）
# ===========================================================================
add_subdirectory(engine)
# ===========================================================================
# 演示宿主：GryceRenderSmoke.exe（人工运行，非 CTest 用例）
#
# 它加载 test/demo_project 演示场景，是打包产物的入口（见 toolchain/ 与 dist/）。
# 历史上它还 "身兼第二职"：以 renderer_smoke_opengl / renderer_smoke_vulkan 两条
# CTest 用例注册，靠 "截图里非背景像素占比不足阈值就判失败" 来守渲染回归。
# 那两条像素用例已按需求删除 —— 它们依赖 GPU / 驱动 / 无头环境，CI 上不稳定，
# 也和 "计算结果对不对" 无关。CI 门禁改由下面的 GryceMathTests 承担。
# ===========================================================================
add_executable(GryceRenderSmoke test/renderer_smoke.cpp)
target_include_directories(GryceRenderSmoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)
target_compile_definitions(GryceRenderSmoke PRIVATE
    GRYCE_DEMO_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/test/demo_project"
)
target_link_libraries(GryceRenderSmoke PRIVATE
    GryceRenderer GryceCore GrycePlatform
)
if(WIN32)
    target_link_libraries(GryceRenderSmoke PRIVATE opengl32 gdi32)
endif()

# 引擎 DLL 同步到 exe 目录。
# 注意：这个 ALL 目标不能替代 POST_BUILD 拷贝 —— 它只在（全量）构建时执行，
# 日常 `cmake --build . --target GryceRenderer` 单独构建引擎库时不会触发，
# exe 目录里会留下旧 DLL，运行时加载过期二进制（"改了代码却看不到变化"
# 的经典陷阱）。所以引擎 DLL 自身也挂了 POST_BUILD 拷贝，见下方 foreach。
# 这里负责第三方运行库（glfw3.dll / glew32.dll / box2d / Jolt）与 MinGW
# 运行时：它们来自依赖前缀或编译器 bin 目录，非 MSYS2 shell 里启动时 PATH
# 上不一定有，复制到 exe 同目录最稳；且多为 IMPORTED 目标，无法挂 POST_BUILD。
# 用 $<TARGET_FILE:...> 而不是绝对路径：系统包场景解析为 IMPORTED_LOCATION，
# FetchContent 场景解析为本地构建产物，两种来源一套代码。
#
# MinGW 运行时 DLL（libstdc++-6 / libgcc_s_* / libwinpthread-1）不在任何依赖
# 前缀里，只由编译器自己的 bin 目录提供，而该目录通常仅通过 PATH 可见。
# 不复制就会导致脱离 MinGW/MSYS2 shell 时无法启动：shell 里报
#   "libstdc++-6.dll: cannot open shared object file"（退出码 127），
# ctest 里报 0xc0000135（DLL not found，且因 Windows 弹模态框而卡到超时）。
# 这里用编译器路径反推 bin 目录，兼容 CLion 自带 MinGW 与 MSYS2 任意前缀；
# libgcc_s 同时试 seh/dw2 两个名字，覆盖 x64 与 32 位线程模型。
set(GRYCE_DLL_TARGETS GryceRenderer GryceCore GrycePlatform)
set(_gryce_mingw_rt_cmds "")
if(MINGW)
    get_filename_component(_gryce_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_gryce_rt IN ITEMS
            libstdc++-6.dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libwinpthread-1.dll)
        unset(_gryce_rt_path CACHE)
        find_file(_gryce_rt_path "${_gryce_rt}"
            PATHS "${_gryce_cxx_dir}" "${_gryce_cxx_dir}/../bin" NO_DEFAULT_PATH)
        if(_gryce_rt_path)
            list(APPEND _gryce_mingw_rt_cmds
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_gryce_rt_path}" "$<TARGET_FILE_DIR:GryceRenderSmoke>")
        endif()
    endforeach()
    unset(_gryce_rt_path CACHE)
endif()

add_custom_target(GryceSyncRuntimeDlls ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:GryceRenderer>" "$<TARGET_FILE_DIR:GryceRenderSmoke>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:GryceCore>" "$<TARGET_FILE_DIR:GryceRenderSmoke>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:GrycePlatform>" "$<TARGET_FILE_DIR:GryceRenderSmoke>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:box2d::box2d>" "$<TARGET_FILE_DIR:GryceRenderSmoke>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Jolt::Jolt>" "$<TARGET_FILE_DIR:GryceRenderSmoke>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:glfw>" "$<TARGET_FILE_DIR:GryceRenderSmoke>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:GLEW::GLEW>" "$<TARGET_FILE_DIR:GryceRenderSmoke>"
    ${_gryce_assimp_sync_cmd}
    ${_gryce_mingw_rt_cmds}
    DEPENDS ${GRYCE_DLL_TARGETS} GryceRenderSmoke
    COMMENT "Syncing engine, third-party and MinGW runtime DLLs next to GryceRenderSmoke.exe"
    VERBATIM
)
unset(_gryce_mingw_rt_cmds)
unset(_gryce_assimp_sync_cmd)

# ===========================================================================
# 测试：CTest 只跑"纯计算正确性核对"，CI 要求全绿才算通过。
#
# 测试可执行文件不链接任何引擎 DLL：被测源码（engine/math/*.cpp）直接编译进
# GryceMathTests.exe。好处是与 GPU / 驱动 / 无头环境完全解耦：CI 上毫秒级出
# 结果，也不会因为缺 glfw3.dll / MinGW 运行时 DLL 而卡到超时（历史上这类
# DLL 缺失在 ctest 里表现为 0xc0000135 + 模态弹窗挂起到 TIMEOUT）。
#
# 每个 suite 注册成一条独立 CTest：ctest 任一条失败即返回非零、整体判定为
# 不通过（CI 里没有任何 continue-on-error / 跳过逻辑）。
# 新增 suite 时同步往 GRYCE_MATH_TEST_SUITES 里加名字即可。
# ===========================================================================
include(CTest)

if(BUILD_TESTING)
    set(GRYCE_MATH_TEST_SUITES vector matrix quaternion camera ray)

    add_executable(GryceMathTests
        test/math_tests.cpp
        engine/math/math.cpp
        engine/math/camera.cpp
    )
    target_include_directories(GryceMathTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
    )

    foreach(suite IN LISTS GRYCE_MATH_TEST_SUITES)
        add_test(NAME math_${suite}
                 COMMAND GryceMathTests --suite ${suite}
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(math_${suite} PROPERTIES
            LABELS "unit;math" TIMEOUT 60)
    endforeach()
endif()
