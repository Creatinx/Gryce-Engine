cmake_minimum_required(VERSION 3.24)

project(GryceEngineRenderer
        VERSION 0.1.0
        DESCRIPTION "Gryce Engine Renderer: GryceCore + GryceRenderer + GrycePlatform"
        LANGUAGES C CXX)

option(GRYCE_ENABLE_IMGUI "Enable ImGui editor overlay in GryceRenderer" ON)

# ===========================================================================
# WebGL（Emscripten/wasm）构建
#
# 检测到 EMSCRIPTEN 工具链时自动启用（无 emsdk 的机器上 CMake 根本不进
# 这个分支，桌面构建零影响）。Web 目标下：
#   * 三个引擎 SHARED DLL 改静态库，链成单一 wasm；
#   * GLEW/opengl32/gdi32/winmm/MinGW 运行时全部不参与（WebGL2 用
#     GLES3 头 + engine/render/opengl/gl_webgl_shim.h 的 DSA shim）；
#   * GLFW 走 Emscripten 自带端口（-s USE_GLFW=3）；
#   * Box2D/Jolt/Assimp/Amplitude 无法为 wasm 链接 → 物理/音频桩化 +
#     首版 OBJ-only（内置 ObjLoader 零依赖）；
#   * --api webgl 与桌面 OpenGL 共用 GLBackend（docs/zh/webgl-backend.md）。
# ===========================================================================
option(GRYCE_ENABLE_WEBGL "启用 WebGL（Emscripten）构建" OFF)
if(EMSCRIPTEN)
    set(GRYCE_ENABLE_WEBGL ON)
endif()
if(GRYCE_ENABLE_WEBGL AND NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "GRYCE_ENABLE_WEBGL=ON 需要 Emscripten 工具链（emcmake cmake ...）。\n"
        "桌面构建请不要开启该选项。")
endif()

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
# wasm 目标不认 -mavx2 等 x86 标志（EMSCRIPTEN 下 CMAKE_SYSTEM_PROCESSOR
# 也可能是 x86_64，必须显式排除）。
if(GRYCE_ENABLE_X86_V3 AND NOT MSVC AND NOT EMSCRIPTEN
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
#
# Emscripten 例外：GLFW 由 Emscripten 端口提供（-s USE_GLFW=3，头文件在
# 工具链系统 include 里），不 find_package、不 FetchContent。
# ===========================================================================
if(EMSCRIPTEN)
    add_library(glfw INTERFACE)
    target_link_options(glfw INTERFACE "-sUSE_GLFW=3")
    message(STATUS "GLFW: Emscripten port (-s USE_GLFW=3)")
else()

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

endif() # EMSCRIPTEN

# ===========================================================================
# 物理依赖：Box2D（2D 刚体）+ Jolt Physics（3D 刚体）
# 两者都以 CMake 配置包形式安装（box2dConfig.cmake / JoltConfig.cmake），
# 导入目标为 box2d::box2d 与 Jolt::Jolt（均为共享库）。
# 搜索路径来自 CMAKE_PREFIX_PATH（见 GryceDependencyPaths），无需写死前缀。
# 配置包只导出 RELEASE 配置，Debug/RelWithDebInfo 构建经 MAP_IMPORTED_CONFIG
# 回退到 RELEASE 导入库，避免 "configuration not found" 报错。
#
# Emscripten 例外：预编译的 MinGW 库无法为 wasm 链接。物理系统源码已
# #ifdef __EMSCRIPTEN__ 桩化（engine/ecs/systems/physics_system_*.cpp），
# 这里提供空的 INTERFACE 导入目标让链接行不变。
# ===========================================================================
if(EMSCRIPTEN)
    add_library(box2d::box2d INTERFACE IMPORTED)
    add_library(Jolt::Jolt INTERFACE IMPORTED)
    message(STATUS "Box2D/Jolt: Emscripten build (physics stubbed out)")
else()

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

endif() # EMSCRIPTEN

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
#
# Emscripten 例外：预编译 MinGW 库无法为 wasm 链接，且 FetchContent 拉
# assimp 源码编译耗时/体积都不适合首版 → Web 首版 OBJ-only（内置 ObjLoader
# 零依赖），assimp 源码构建列为后续可选项。
# ===========================================================================
if(EMSCRIPTEN)
    message(STATUS "Assimp: Emscripten build (OBJ-only via built-in ObjLoader)")
else()

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

endif() # EMSCRIPTEN

# ===========================================================================
# 音频依赖：Amplitude Audio SDK（SparkyStudios）
# 以预编译静态库形式随仓库分发（third_party/amplitude）：SDK 源码需 GCC16
# 补丁且构建耗时，故引擎侧只消费头文件 + libAmplitude.a 及其两个私有依赖
# （libflatbuffers.a / libsamplerate.a，静态库不会自动带入 PRIVATE 依赖）。
#
# Emscripten 例外：预编译 .a 无法为 wasm 链接。音频系统源码已
# #ifdef __EMSCRIPTEN__ 桩化（静音模式，API 表面不变），这里给一个空的
# INTERFACE 导入目标让链接行不变。
# ===========================================================================
if(EMSCRIPTEN)
    add_library(amplitude INTERFACE IMPORTED)
    message(STATUS "Amplitude: Emscripten build (audio stubbed out)")
else()

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

endif() # EMSCRIPTEN

# ===========================================================================
# GLEW：OpenGL 扩展加载。优先用系统包，缺失时按 GRYCE_DEP_FETCH 拉源码构建。
# 两种情况都统一暴露成 GLEW::GLEW 目标，engine/ 只消费这个目标。
#
# Emscripten 例外：WebGL2 用 GLES3/gl3.h（工具链自带）+ DSA shim
# （engine/render/opengl/gl_webgl_shim.h，经 cmake/web/GL/glew.h 的
# <GL/glew.h> 替身间接包含），不链接任何 GLEW 库。这里给空的 INTERFACE
# 目标让 engine/ 的链接行不变。
# ===========================================================================
if(EMSCRIPTEN)
    add_library(GLEW::GLEW INTERFACE IMPORTED)
    message(STATUS "GLEW: Emscripten build (GLES3 headers + gl_webgl_shim.h)")
else()

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

endif() # EMSCRIPTEN

# GryceRenderSmoke.exe 的运行时输出目录。engine/ 子目录里引擎 DLL 的
# POST_BUILD 拷贝命令需要它（那边无法引用尚未定义的 exe target，
# add_custom_command(TARGET) 也不允许跨目录挂命令，拷贝只能挂在 DLL 自身）。
# 默认不设 RUNTIME_OUTPUT_DIRECTORY，即落在 ${CMAKE_BINARY_DIR} 根下。
set(GRYCE_EXE_OUTPUT_DIR "${CMAKE_BINARY_DIR}")

# ===========================================================================
# QuickJS 脚本引擎：third_party/quickjs 随仓库分发（MIT，GryceQuickJS 静态库）。
# 必须在 engine/ 之前添加：engine/CMakeLists.txt 要把 GryceQuickJS 链进
# GryceCore（沙箱口径见该处的 CMakeLists 头注释）。
# ===========================================================================
add_subdirectory(third_party/quickjs)

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
if(EMSCRIPTEN)
    # Web 构建：默认工程改为 MEMFS 里的 webgl_demo（--preload-file 落到
    # /webgl_demo，相对路径即可被 fopen 解析），避免把 93MB 的桌面
    # demo_project（FBX 占 92MB）塞进 wasm 包。
    target_compile_definitions(GryceRenderSmoke PRIVATE
        GRYCE_DEMO_PROJECT_ROOT="webgl_demo"
    )
    # 输出 GryceRenderSmoke.html 而不是 .js：Emscripten 只有生成 html 输出时
    # 才会应用 --shell-file（否则 warning "--shell-file ignored when not
    # generating html output"，壳文件被静默丢弃，Module.print 捕获不到）。
    set_target_properties(GryceRenderSmoke PROPERTIES SUFFIX ".html")
    # Web 调试开关：ASSERTIONS 让 abort 带原因串，DISABLE_EXCEPTION_CATCHING=0
    # 让 C++ 异常能穿到 JS 层看栈（引擎里 async_loader / 资源加载都有 try/catch，
    # 默认配置下未捕获异常会直接 Aborted() 且无任何信息）。
    # 发布构建可设 GRYCE_WEBGL_DEBUG=OFF 缩小体积。
    option(GRYCE_WEBGL_DEBUG "Web 构建保留 abort 断言与异常信息" ON)
    if(GRYCE_WEBGL_DEBUG)
        target_compile_options(GryceRenderSmoke PRIVATE "-fexceptions")
        target_link_options(GryceRenderSmoke PRIVATE
            "-sASSERTIONS=1"
            "-sDISABLE_EXCEPTION_CATCHING=0"
            "-sSTACK_OVERFLOW_CHECK=1"
        )
    endif()
    target_link_options(GryceRenderSmoke PRIVATE
        "-sUSE_GLFW=3"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAX_WEBGL_VERSION=2"
        "-sFULL_ES3"
        "-sSTACK_SIZE=1MB"
        "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/test/webgl_demo@/webgl_demo"
        # 引擎默认 shader 树。Web 上 exe_base_dir() 落到 current_path()='/'，
        # 而 core_shaders::engine_shaders_dir() 的候选 #1 正是 '<base>/shaders'，
        # 所以预载到 /shaders 即可被兜底命中（MEMFS 里就是普通文件，shader
        # 热重载路径不受影响）。191 个文件 833K。
        "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/engine/render/shaders@/shaders"
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/test/webgl_shell.html"
        # 壳的截图外取要读 MEMFS（Module.FS）。Emscripten 6.x 对未导出的运行时
        # 方法做了"访问即 abort"处理（Aborted('FS' was not exported...)），
        # 不导出会让壳里的 Module.FS 读取把整个运行时杀掉。
        "-sEXPORTED_RUNTIME_METHODS=FS"
    )
    # 资源打包是**链接期**行为：改/增 payload 里的文件若不触发重链，
    # .data 就不会更新，表现为"改了场景却没生效"的灵异 bug。
    # target_sources/HEADER_FILE_ONLY 只把文件挂到编译步，链接规则仍然只依赖
    # .o 与 .a（见 build.ninja 的 LINK 语句），故改用 OBJECT_DEPENDS：把 payload
    # 全部文件挂成 renderer_smoke.cpp 的附加依赖，payload 一变就重编该 .o，
    # 进而必然重链、重新生成 .data。代价仅是一次单文件重编。
    # CONFIGURE_DEPENDS 让 glob 在构建期重新扫描，新增文件无需手动 reconfigure。
    #
    # 想打"一个 html 双击即用"的单文件包（UE5 Web 导出形态）？不要改这里 ——
    # 那是打包器的职责，见 GryceGC webpack（toolchain/src/commands/cmd_webpack.cpp）。
    file(GLOB_RECURSE _gryce_webgl_payload CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test/webgl_demo/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/render/shaders/*"
    )
    set_source_files_properties(test/renderer_smoke.cpp PROPERTIES
        OBJECT_DEPENDS "${_gryce_webgl_payload}"
    )
else()
target_compile_definitions(GryceRenderSmoke PRIVATE
    GRYCE_DEMO_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/test/demo_project"
)
endif()
target_link_libraries(GryceRenderSmoke PRIVATE
    GryceRenderer GryceCore GrycePlatform
)
if(WIN32)
    target_link_libraries(GryceRenderSmoke PRIVATE opengl32 gdi32)
endif()

# ===========================================================================
# 应用运行时：GryceApp.exe
#
# 这是给"项目只写 main.js"的形态准备的宿主：窗口、场景、退出都由脚本调
# engine.* 决定，engine/app/app_main.cpp 只是几行模板 main。项目目录因此
# 不需要自己的 main.cpp；要原生扩展时编一个 extension.dll 放项目根即可
# （ABI 见 engine/api/extension_api.h）。
#
# GryceRenderSmoke 保留为渲染回归/基准台，两者互不替代。
# Web 构建暂不提供 GryceApp（wasm 侧仍是 smoke 的常驻循环 + 预载 payload）。
# ===========================================================================
if(NOT EMSCRIPTEN)
    add_library(GryceAppHost STATIC engine/app/app_host.cpp)
    target_include_directories(GryceAppHost PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
    )
    # PUBLIC：exe 只链 GryceAppHost，也要拿到引擎各 DLL 的导入库与头文件路径
    target_link_libraries(GryceAppHost PUBLIC GryceRenderer GryceCore GrycePlatform)
    if(WIN32)
        target_link_libraries(GryceAppHost PUBLIC opengl32 gdi32)
    endif()

    add_executable(GryceApp engine/app/app_main.cpp)
    target_link_libraries(GryceApp PRIVATE GryceAppHost)

    # 扩展样例：把引擎已实现、脚本层未开放的 C API 包成 JS 接口，因此链接
    # GryceCore 的导入库（只走 engine/api/*.h 的 C ABI，不碰 C++ 内部类型）。
    # ABI 本身不要求链接引擎——只做纯 JS 桥的扩展仍可零依赖构建，但那种写法
    # 等于把业务逻辑搬进 C++，不符合扩展的定位。
    add_library(GryceDemoExtension SHARED test/demo_project/extension.cpp)
    target_include_directories(GryceDemoExtension PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
    )
    target_link_libraries(GryceDemoExtension PRIVATE GryceCore)
    set_target_properties(GryceDemoExtension PROPERTIES
        PREFIX "" OUTPUT_NAME "GryceDemoExtension")
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

# Web 构建没有 DLL 同步概念（静态链成单一 wasm），整块跳过；EMSCRIPTEN 下
# box2d/Jolt/GLEW 还是空 INTERFACE 目标，$<TARGET_FILE:...> 无从解析。
if(NOT EMSCRIPTEN)

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

endif() # EMSCRIPTEN
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
# Web 构建跳过 CTest 与工具链：GryceMathTests 是桌面数值门禁（与 GPU 无关
# 但也没必要为 wasm 编一份），GryceGC 是 Windows 命令行发布工具。
if(NOT EMSCRIPTEN)
include(CTest)

if(BUILD_TESTING)
    # material 套件校验模型文件材质 → PBR 参数的推导规则（见 assets/material_rules.cpp）
    # lod 套件校验自动 LOD 链的结构性保证（见 assets/mesh_lod.cpp）
    set(GRYCE_MATH_TEST_SUITES vector matrix quaternion camera ray material lod)

    add_executable(GryceMathTests
        test/math_tests.cpp
        engine/math/math.cpp
        engine/math/camera.cpp
        engine/assets/material_rules.cpp
        engine/assets/mesh_lod.cpp
        # mesh_lod 的日志走 glog（自包含，无引擎依赖，工具链同样单独链接它）
        engine/utils/glog/glog_lib.cpp
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

    # 脚本系统（QuickJS）套件：值转换 / 生命周期 / 错误隔离 / 热重载 / 应用入口。
    # 与 math 测试同构——被测源码（engine/script + runtime + glog + resources
    # 路径解析）直编进 exe，不链 GryceCore.dll，与 GPU/无头环境无关。
    set(GRYCE_SCRIPT_TEST_SUITES value_convert lifecycle error_isolation hot_reload app_entry)

    add_executable(GryceScriptTests
        test/script_tests.cpp
        engine/script/js_runtime.cpp
        engine/script/js_bindings.cpp
        engine/script/script_component.cpp
        engine/script/script_app.cpp
        engine/script/extension_host.cpp
        engine/runtime/engine_context.cpp
        engine/utils/glog/glog_lib.cpp
        engine/resources/resource_path.cpp
        engine/resources/project.cpp
    )
    target_include_directories(GryceScriptTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json
    )
    target_link_libraries(GryceScriptTests PRIVATE GryceQuickJS)

    foreach(suite IN LISTS GRYCE_SCRIPT_TEST_SUITES)
        add_test(NAME script_${suite}
                 COMMAND GryceScriptTests --suite ${suite}
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(script_${suite} PROPERTIES
            LABELS "unit;script" TIMEOUT 60)
    endforeach()

    # 资产侧车文件（<asset>.meta）与导入设置套件：编辑器靠 guid 维持资产引用，
    # 靠侧车里的缩放/上轴/材质开关复现导入结果。同样只编被测源码，不链 DLL。
    set(GRYCE_ASSET_TEST_SUITES meta import_options)

    add_executable(GryceAssetTests
        test/asset_tests.cpp
        engine/assets/asset_meta.cpp
        engine/assets/import_options.cpp
        engine/utils/glog/glog_lib.cpp
    )
    target_include_directories(GryceAssetTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json
    )

    foreach(suite IN LISTS GRYCE_ASSET_TEST_SUITES)
        add_test(NAME asset_${suite}
                 COMMAND GryceAssetTests --suite ${suite}
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(asset_${suite} PROPERTIES
            LABELS "unit;asset" TIMEOUT 60)
    endforeach()

    # UI 操作队列：UI 操作进队列、按策略受控执行（见 api/ui_op_queue.h）。
    # 与 math/script/asset 同构——被测源码（api/ui_op_queue.cpp + api/ui_api.cpp）
    # 直编进 exe，不链 GryceRenderer.dll，也不定义 GRYCE_HAS_IMGUI：
    # 同时验证"关掉 ImGui 的构建里队列照样能用"。GRYCE_RENDERER_BUILDING 让
    # 导出宏变成 dllexport，静态 exe 内自链接（否则 dllimport 会去找 __imp_ 符号）。
    set(GRYCE_UI_TEST_SUITES queue abi)

    add_executable(GryceUiTests
        test/ui_tests.cpp
        engine/api/ui_op_queue.cpp
        engine/api/ui_api.cpp
    )
    target_include_directories(GryceUiTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
    )
    target_compile_definitions(GryceUiTests PRIVATE GRYCE_RENDERER_BUILDING)

    foreach(suite IN LISTS GRYCE_UI_TEST_SUITES)
        add_test(NAME ui_${suite}
                 COMMAND GryceUiTests --suite ${suite}
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(ui_${suite} PROPERTIES
            LABELS "unit;ui" TIMEOUT 60)
    endforeach()

    # 编辑器前置能力（编辑命令面 / 撤销重做 / 场景存档往返）。
    # 这一个必须链 GryceCore.dll —— 命令消费与序列化都在 core 内部，没有可
    # 剥离的纯计算内核；但这些路径都不碰 GPU，因此 CI 仍不需要显卡。
    set(GRYCE_EDITOR_TEST_SUITES edit undo scene)

    add_executable(GryceEditorTests test/editor_tests.cpp)
    target_include_directories(GryceEditorTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
    )
    target_compile_definitions(GryceEditorTests PRIVATE
        GRYCE_TEST_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/demo_project"
    )
    target_link_libraries(GryceEditorTests PRIVATE GryceCore)

    foreach(suite IN LISTS GRYCE_EDITOR_TEST_SUITES)
        add_test(NAME editor_${suite}
                 COMMAND GryceEditorTests --suite ${suite}
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(editor_${suite} PROPERTIES
            LABELS "unit;editor" TIMEOUT 120)
    endforeach()
endif()

# ===========================================================================
# GryceGC 工具链（toolchain）
#
# 自包含零引擎 DLL 依赖：把需要的 engine 源码（aes_gcm / gpack / pak /
# glog / utils/crypto）直接编进 grycegc_core 静态库，与 GryceMathTests 同构。
# 独立构建：cmake -S toolchain -B toolchain/build -G Ninja
# 日常发布：toolchain/build/GryceGC package --build-dir build
# ===========================================================================
option(GRYCE_BUILD_TOOLCHAIN "构建 GryceGC 命令行工具链" ON)

if(GRYCE_BUILD_TOOLCHAIN AND NOT EMSCRIPTEN)
    add_subdirectory(toolchain)
endif()

endif() # EMSCRIPTEN (CTest)
