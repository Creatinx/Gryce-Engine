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

# GryceEditor（C++/ImGui）编译开关
option(GRYCE_BUILD_EDITOR "Build the ImGui-based Editor" ON)

# GryceEngineUtils 嵌入式渲染器框架：可选模块
option(GRYCE_ENABLE_ECS     "Enable ECS module" ON)
option(GRYCE_ENABLE_PHYSICS "Enable Physics module" ON)
option(GRYCE_ENABLE_AUDIO   "Enable Audio module" ON)
option(GRYCE_ENABLE_SCRIPT  "Enable QuickJS scripting" ON)
option(GRYCE_ENABLE_UI      "Enable UI module" ON)

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
add_subdirectory(src)

# GryceSPC: standalone game template (GryceGC packaging entry point)
option(GRYCE_BUILD_GAME_TEMPLATE "Build the standalone game template (GryceSPC)" ON)
add_subdirectory(templates)

# GryceGC: C++ resource packager (GryceSPC packaging tool)
add_subdirectory(tools/grycegc)

if(GRYCE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# examples 目录已清空，所有 demo 已移除

# ===========================================================================
# GryceEngineUtils — 嵌入式渲染器框架（公共包装层）
#
# 底层实现复用 src/ 中的 DLL 模块（GryceCore / GryceRenderer / GrycePlatform /
# GrycePhysics），这里的 "static" target 只编译包装层源码（只包装，不重写），
# 并通过 GRYCE_ENABLE_* 选项控制可选模块（ECS / 物理 / 音频 / 脚本）。
# ===========================================================================
set(GRYCE_UTILS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
set(GRYCE_UTILS_SRC_DIR ${CMAKE_SOURCE_DIR}/src)

# ---- 数学库（必选，header-only re-export）-------------------------------
add_library(gryce_math_static INTERFACE)
target_include_directories(gryce_math_static INTERFACE
    ${GRYCE_UTILS_INCLUDE_DIR}
    ${GRYCE_UTILS_SRC_DIR}
)
target_link_libraries(gryce_math_static INTERFACE GryceCore gryce::compile_options)
set_target_properties(gryce_math_static PROPERTIES FOLDER "GryceEngineUtils")

# ---- 平台库（必选，window/input）----------------------------------------
add_library(gryce_platform_static INTERFACE)
target_include_directories(gryce_platform_static INTERFACE
    ${GRYCE_UTILS_INCLUDE_DIR}
    ${GRYCE_UTILS_SRC_DIR}
)
target_link_libraries(gryce_platform_static INTERFACE GrycePlatform gryce_math_static)
set_target_properties(gryce_platform_static PROPERTIES FOLDER "GryceEngineUtils")

# ---- 核心渲染器库（必选）------------------------------------------------
add_library(gryce_renderer_static STATIC
    ${GRYCE_UTILS_SRC_DIR}/renderer/renderer.cpp
)
target_include_directories(gryce_renderer_static PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
target_link_libraries(gryce_renderer_static PUBLIC
    GryceRenderer GrycePlatform GryceCore gryce_math_static
)
# renderer.cpp 中 UI 集成代码按模块开关编译（关闭时提供空实现）
if(GRYCE_ENABLE_UI)
    target_compile_definitions(gryce_renderer_static PRIVATE GRYCE_ENABLE_UI)
endif()
set_target_properties(gryce_renderer_static PROPERTIES FOLDER "GryceEngineUtils")

# ---- ECS 库（可选）------------------------------------------------------
if(GRYCE_ENABLE_ECS)
    add_library(gryce_ecs_static STATIC
        ${GRYCE_UTILS_SRC_DIR}/ecs/gryce_world.cpp
    )
    target_include_directories(gryce_ecs_static PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
    )
    target_link_libraries(gryce_ecs_static PUBLIC gryce_renderer_static GryceCore)
    set_target_properties(gryce_ecs_static PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- 物理库（可选，独立于 ECS）------------------------------------------
if(GRYCE_ENABLE_PHYSICS)
    add_library(gryce_physics_static STATIC
        ${GRYCE_UTILS_SRC_DIR}/physics/gryce_physics_world.cpp
    )
    target_include_directories(gryce_physics_static PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
    )
    target_link_libraries(gryce_physics_static PUBLIC GrycePhysics gryce_math_static)
    set_target_properties(gryce_physics_static PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- 音频库（可选，独立于 ECS）------------------------------------------
if(GRYCE_ENABLE_AUDIO)
    add_library(gryce_audio_static STATIC
        ${GRYCE_UTILS_SRC_DIR}/audio/gryce_audio_engine.cpp
    )
    target_include_directories(gryce_audio_static PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
    )
    target_link_libraries(gryce_audio_static PUBLIC GryceCore gryce_math_static)
    set_target_properties(gryce_audio_static PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- QuickJS 脚本引擎 -------------------------------------------------
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/quickjs ${CMAKE_BINARY_DIR}/third_party/quickjs)

# ---- 脚本库（QuickJS 运行时 + 绑定，可选）--------------------------------
# 源文件已编译到 GryceCore；本 target 仅传播头文件和链接依赖。
if(GRYCE_ENABLE_SCRIPT)
    add_library(gryce_script_static INTERFACE)
    target_include_directories(gryce_script_static INTERFACE
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(gryce_script_static INTERFACE
        GryceCore
        GryceQuickJS
    )
    set_target_properties(gryce_script_static PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- UI 库（可选，独立于 ECS）--------------------------------------------
if(GRYCE_ENABLE_UI)
    add_library(gryce_ui_static STATIC
        ${GRYCE_UTILS_SRC_DIR}/ui/ui.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/widget.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/stylesheet.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/animation.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/ui_draw.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/label.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/button.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/panel.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/image.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/text_input.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/slider.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/checkbox.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/progress_bar.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/scroll_view.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/divider.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/toolbar.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/menubar.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/combobox.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/radiobutton.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/spinbox.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/factory.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/uif_parser.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/uif_builder.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/style_preset.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/ui_renderer.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/script_vm.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/engine_bridge.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/dsl/lexer.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/dsl/parser.cpp
    )
    target_include_directories(gryce_ui_static PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(gryce_ui_static PUBLIC
        gryce_renderer_static
        GryceQuickJS
    )
    set_target_properties(gryce_ui_static PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- 统一入口 target：按选项聚合所有启用的模块 ---------------------------
add_library(GryceEngineUtils INTERFACE)
target_include_directories(GryceEngineUtils INTERFACE ${GRYCE_UTILS_INCLUDE_DIR})
target_link_libraries(GryceEngineUtils INTERFACE
    gryce_renderer_static
    gryce_platform_static
    gryce_math_static
)
if(GRYCE_ENABLE_ECS)
    target_link_libraries(GryceEngineUtils INTERFACE gryce_ecs_static)
endif()
if(GRYCE_ENABLE_PHYSICS)
    target_link_libraries(GryceEngineUtils INTERFACE gryce_physics_static)
endif()
if(GRYCE_ENABLE_AUDIO)
    target_link_libraries(GryceEngineUtils INTERFACE gryce_audio_static)
endif()
if(GRYCE_ENABLE_SCRIPT)
    target_link_libraries(GryceEngineUtils INTERFACE gryce_script_static)
endif()
if(GRYCE_ENABLE_UI)
    target_link_libraries(GryceEngineUtils INTERFACE gryce_ui_static)
endif()
set_target_properties(GryceEngineUtils PROPERTIES FOLDER "GryceEngineUtils")

# ---------------------------------------------------------------------------
# ImGui Docking Editor（editor/CMakeLists.txt）
# 基于 ImGui Docking 分支的 C++ 原生编辑器（替代旧 WPF 编辑器）。
# 纯 C++ 实现，跨平台，直接渲染到引擎 OpenGL/Vulkan 上下文。
# 编辑器源码目录在重建完成前可能没有 CMakeLists.txt（见最近一次
# "remove C++ ImGui editor" 提交），此时跳过，避免 configure 失败。
# ---------------------------------------------------------------------------
if(WIN32 AND GRYCE_BUILD_EDITOR AND EXISTS "${CMAKE_SOURCE_DIR}/editor/CMakeLists.txt")
    add_subdirectory(editor)
endif()

# ---------------------------------------------------------------------------
# Examples（示例项目）
# ---------------------------------------------------------------------------
option(GRYCE_BUILD_EXAMPLES "Build the example projects" ON)
if(GRYCE_BUILD_EXAMPLES AND EXISTS "${CMAKE_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# IDE 分组
# ---------------------------------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT editor)
