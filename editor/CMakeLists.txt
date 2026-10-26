# ===========================================================================
# editor/CMakeLists.txt — 编辑器：一处编排，两块物理隔离的产物
#
# editor/project/  完整项目（纯 JS + 场景 + 资产），由宿主 GryceEditor.exe 启动。
# editor/ext/      原生功能拓展，编成 extension.dll 放进 project/，宿主自动加载。
#
# 两种构建模式（与 toolchain/ 同构）：
#   1. 独立配置：cmake -S editor -B editor/build -G Ninja
#      编辑器自带一整套 CMake 工程，不经过根 CMakeLists。此时它需要一份
#      **已构建好的引擎树**（导入库 + 头文件），用 GRYCE_ENGINE_BUILD 指定；
#      引擎源码树用 GRYCE_ENGINE_ROOT 指定（默认取 editor/ 的上一级）。
#   2. 被根工程包含：根 CMakeLists 的 GRYCE_BUILD_EDITOR=ON 时 add_subdirectory
#      到这里，此时直接用根工程已经定义好的 GryceCore / GryceRenderer /
#      GrycePlatform / GryceAppHost 目标，不需要任何外部路径。
#
# 为什么要有 editor_main.cpp 这个 exe：编辑器此前只能靠通用宿主带参数启动
# （GryceApp --project editor/project），每次都要记住那串参数，IDE 运行配置里
# 也得手填一遍；漏填时宿主退回当前工作目录解析 res:/，报的是"场景加载失败"
# 这种指向错误位置的消息。烧进项目根后启动就是 build/GryceEditor.exe。
# 宿主逻辑不复制一份，仍走 gryce_engine::app::run —— 编辑器与 demo 一样是
# "宿主 + 项目"，两者的窗口/场景/退出行为必须完全一致。
#
# 与顶层 GryceApp 的关系：GryceApp 是通用模板入口（任意项目都能用），
# GryceEditor 是这一个项目的专用入口。两者并存，不互相替代。
# ===========================================================================

cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# 模式判定：独立配置 vs 被根工程包含。
#
# 与 toolchain/CMakeLists.txt 同一套写法（CMAKE_SOURCE_DIR 与当前目录相同
# 即为独立配置）。放在 project() 之前，因为独立模式要自己起 project()。
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(GRYCE_EDITOR_STANDALONE ON)
else()
    set(GRYCE_EDITOR_STANDALONE OFF)
endif()

if(GRYCE_EDITOR_STANDALONE)
    project(GryceEditor VERSION 0.1.0 LANGUAGES C CXX)

    # 引擎源码树：只需要头文件（engine/api/*.h）与 app_host.cpp。
    set(GRYCE_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
        "Gryce-Engine 源码树根目录（提供 engine/ 与 third_party/ 头文件）")
    # 引擎构建树：提供三个 DLL 的导入库（libGryceCore.dll.a 等）。
    set(GRYCE_ENGINE_BUILD "${GRYCE_ENGINE_ROOT}/build" CACHE PATH
        "已构建的引擎树目录（提供 libGryceCore.dll.a 等导入库）")

    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()
    if(NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g1 -fno-omit-frame-pointer")
    endif()

    # 导入库查找：Windows(MinGW) 是 libGryceCore.dll.a，Linux 是 libGryceCore.so。
    # MSVC 用 .lib，名字同名，由 CMAKE_FIND_LIBRARY_SUFFIXES 默认覆盖。
    find_library(GRYCE_CORE_IMPORT_LIB
        NAMES GryceCore libGryceCore.dll.a
        PATHS "${GRYCE_ENGINE_BUILD}/engine" "${GRYCE_ENGINE_BUILD}"
        NO_DEFAULT_PATH)
    find_library(GRYCE_RENDERER_IMPORT_LIB
        NAMES GryceRenderer libGryceRenderer.dll.a
        PATHS "${GRYCE_ENGINE_BUILD}/engine" "${GRYCE_ENGINE_BUILD}"
        NO_DEFAULT_PATH)
    find_library(GRYCE_PLATFORM_IMPORT_LIB
        NAMES GrycePlatform libGrycePlatform.dll.a
        PATHS "${GRYCE_ENGINE_BUILD}/engine" "${GRYCE_ENGINE_BUILD}"
        NO_DEFAULT_PATH)

    foreach(_missing GRYCE_CORE_IMPORT_LIB GRYCE_RENDERER_IMPORT_LIB GRYCE_PLATFORM_IMPORT_LIB)
        if(NOT ${_missing})
            message(FATAL_ERROR
                "找不到 ${_missing}：独立构建编辑器需要一份**已构建好的引擎树**。\n"
                "先构建引擎：cmake -S <engine-root> -B <engine-root>/build && "
                "cmake --build <engine-root>/build\n"
                "再用 -DGRYCE_ENGINE_BUILD=<engine-root>/build 指定它。")
        endif()
    endforeach()
    unset(_missing)

    # 三个导入库包装成 IMPORTED 目标，让下面的 target_link_libraries 与
    # 根工程模式完全同构。
    add_library(GryceCore SHARED IMPORTED)
    set_target_properties(GryceCore PROPERTIES
        IMPORTED_LOCATION "${GRYCE_ENGINE_BUILD}/engine/GryceCore.dll"
        IMPORTED_IMPLIB "${GRYCE_CORE_IMPORT_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${GRYCE_ENGINE_ROOT}/engine")
    add_library(GryceRenderer SHARED IMPORTED)
    set_target_properties(GryceRenderer PROPERTIES
        IMPORTED_LOCATION "${GRYCE_ENGINE_BUILD}/engine/GryceRenderer.dll"
        IMPORTED_IMPLIB "${GRYCE_RENDERER_IMPORT_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${GRYCE_ENGINE_ROOT}/engine")
    add_library(GrycePlatform SHARED IMPORTED)
    set_target_properties(GrycePlatform PROPERTIES
        IMPORTED_LOCATION "${GRYCE_ENGINE_BUILD}/engine/GrycePlatform.dll"
        IMPORTED_IMPLIB "${GRYCE_PLATFORM_IMPORT_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${GRYCE_ENGINE_ROOT}/engine")

    # 通用宿主（app_host.cpp）：独立模式下直接把源码编进来。
    # 根工程里它是 GryceAppHost 静态库（同一份源码），两者行为一致。
    add_library(GryceAppHost STATIC "${GRYCE_ENGINE_ROOT}/engine/app/app_host.cpp")
    target_include_directories(GryceAppHost PUBLIC
        "${GRYCE_ENGINE_ROOT}/engine"
        "${GRYCE_ENGINE_ROOT}/third_party"
        "${GRYCE_ENGINE_ROOT}/third_party/nlohmann_json"
        "${GRYCE_ENGINE_ROOT}/third_party/quickjs"
    )
    target_compile_definitions(GryceAppHost PUBLIC GRYCE_BUILD_SHARED)
    target_link_libraries(GryceAppHost PUBLIC GryceRenderer GryceCore GrycePlatform)
    if(WIN32)
        target_link_libraries(GryceAppHost PUBLIC opengl32 gdi32)
    endif()
else()
    # 被根工程包含：引擎目标由根 CMakeLists 提供，这里不重复定义，
    # 也不需要任何外部路径参数。
    # 引擎源码树根 = 根工程源码根（根 CMakeLists 里 CMAKE_SOURCE_DIR 就是它）。
    set(GRYCE_ENGINE_ROOT "${CMAKE_SOURCE_DIR}")
    # exe 输出目录：与其它 exe 一起落在 build 根，引擎 DLL 的 POST_BUILD
    # 同步只拷到 ${GRYCE_EXE_OUTPUT_DIR}（= build 根），exe 落在别处就找不到 DLL。
    set(GRYCE_EDITOR_OUTPUT_DIR "${GRYCE_EXE_OUTPUT_DIR}")
endif()

# 独立模式下 exe 落在自己的构建根（没有根工程的 GRYCE_EXE_OUTPUT_DIR 可用）。
if(GRYCE_EDITOR_STANDALONE)
    set(GRYCE_EDITOR_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
endif()

# 项目根烧进二进制。只在配置时求值，改路径要重新 configure。
get_filename_component(GRYCE_EDITOR_PROJECT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/project" ABSOLUTE)

# ---------------------------------------------------------------------------
# 功能拓展：extension.dll。先编它 —— 下面 GryceEditor 的 add_dependencies
# 要求被依赖的目标已定义。
# ---------------------------------------------------------------------------
add_subdirectory(ext)

# ---------------------------------------------------------------------------
# 编辑器宿主：extension.dll 由上面的 ext/ 子目录编出。
# 依赖它而不是只依赖三个引擎 DLL：单独
# `cmake --build build --target GryceEditor` 时若不带上扩展，产物能启动但界面
# 完全没有（扩展缺失时宿主按设计降级成场景浏览器、不报错），这是个很难自查的
# 静默失败。声明式依赖比在文档里写"记得先编扩展"可靠。
# ---------------------------------------------------------------------------
add_executable(GryceEditor editor_main.cpp)
target_link_libraries(GryceEditor PRIVATE GryceAppHost)
target_compile_definitions(GryceEditor PRIVATE
    GRYCE_EDITOR_PROJECT_ROOT="${GRYCE_EDITOR_PROJECT_ROOT}"
)
add_dependencies(GryceEditor GryceEditorExtension)

set_target_properties(GryceEditor PROPERTIES
    # 与其它 exe 一起落在 build 根：引擎 DLL 的 POST_BUILD 同步只拷到
    # ${GRYCE_EXE_OUTPUT_DIR}（= build 根），exe 落在别处就找不到 DLL。
    # 独立模式下没有 GRYCE_EXE_OUTPUT_DIR（那是根工程给的），落在自己的构建根。
    RUNTIME_OUTPUT_DIRECTORY "${GRYCE_EDITOR_OUTPUT_DIR}"
)

if(GRYCE_EDITOR_STANDALONE)
    # 独立构建：把引擎的三个 DLL 与第三方运行库拷到 exe 同目录，
    # 否则脱离 MinGW/MSYS2 终端（或换一台机器）启动就报 DLL 找不到。
    # 用 copy_if_different：文件没变时不动时间戳。
    add_custom_command(TARGET GryceEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GRYCE_ENGINE_BUILD}/engine/GryceCore.dll"
                "$<TARGET_FILE_DIR:GryceEditor>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GRYCE_ENGINE_BUILD}/engine/GryceRenderer.dll"
                "$<TARGET_FILE_DIR:GryceEditor>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GRYCE_ENGINE_BUILD}/engine/GrycePlatform.dll"
                "$<TARGET_FILE_DIR:GryceEditor>"
        COMMENT "editor: syncing engine DLLs next to GryceEditor.exe"
        VERBATIM
    )
endif()

# 第三方运行时（glfw3 / box2d / Jolt / GLEW / MinGW 运行时）的同步由顶层
# GryceSyncRuntimeDlls 负责，它拷到 $<TARGET_FILE_DIR:GryceRenderSmoke>，
# 与本 exe 同目录，所以这里不需要重复一遍。
