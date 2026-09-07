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
add_library(GryceMathStatic INTERFACE)
target_include_directories(GryceMathStatic INTERFACE
    ${GRYCE_UTILS_INCLUDE_DIR}
    ${GRYCE_UTILS_SRC_DIR}
)
target_link_libraries(GryceMathStatic INTERFACE GryceCore gryce::compile_options)
set_target_properties(GryceMathStatic PROPERTIES FOLDER "GryceEngineUtils")

# ---- 平台库（必选，window/input）----------------------------------------
add_library(GrycePlatformStatic INTERFACE)
target_include_directories(GrycePlatformStatic INTERFACE
    ${GRYCE_UTILS_INCLUDE_DIR}
    ${GRYCE_UTILS_SRC_DIR}
)
target_link_libraries(GrycePlatformStatic INTERFACE GrycePlatform GryceMathStatic)
set_target_properties(GrycePlatformStatic PROPERTIES FOLDER "GryceEngineUtils")

# ---- 核心渲染器库（必选）------------------------------------------------
add_library(GryceRendererStatic STATIC
    ${GRYCE_UTILS_SRC_DIR}/renderer/renderer.cpp
)
target_include_directories(GryceRendererStatic PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
target_link_libraries(GryceRendererStatic PUBLIC
    GryceRenderer GrycePlatform GryceCore GryceMathStatic
)
# renderer.cpp 中 UI 集成代码按模块开关编译（关闭时提供空实现）
if(GRYCE_ENABLE_UI)
    target_compile_definitions(GryceRendererStatic PRIVATE GRYCE_ENABLE_UI)
endif()
set_target_properties(GryceRendererStatic PROPERTIES FOLDER "GryceEngineUtils")

# ---- ECS 库（可选）------------------------------------------------------
if(GRYCE_ENABLE_ECS)
    add_library(GryceEcsStatic STATIC
        ${GRYCE_UTILS_SRC_DIR}/ecs/gryce_world.cpp
    )
    target_include_directories(GryceEcsStatic PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
    )
    target_link_libraries(GryceEcsStatic PUBLIC GryceRendererStatic GryceCore)
    set_target_properties(GryceEcsStatic PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- 物理库（可选，独立于 ECS）------------------------------------------
if(GRYCE_ENABLE_PHYSICS)
    add_library(GrycePhysicsStatic STATIC
        ${GRYCE_UTILS_SRC_DIR}/physics/gryce_physics_world.cpp
    )
    target_include_directories(GrycePhysicsStatic PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
    )
    target_link_libraries(GrycePhysicsStatic PUBLIC GrycePhysics GryceMathStatic)
    set_target_properties(GrycePhysicsStatic PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- 音频库（可选，独立于 ECS）------------------------------------------
if(GRYCE_ENABLE_AUDIO)
    add_library(GryceAudioStatic STATIC
        ${GRYCE_UTILS_SRC_DIR}/audio/gryce_audio_engine.cpp
    )
    target_include_directories(GryceAudioStatic PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
    )
    target_link_libraries(GryceAudioStatic PUBLIC GryceCore GryceMathStatic)
    set_target_properties(GryceAudioStatic PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- QuickJS 脚本引擎 -------------------------------------------------
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/quickjs ${CMAKE_BINARY_DIR}/third_party/quickjs)

# ---- 脚本库（QuickJS 运行时 + 绑定，可选）--------------------------------
# 源文件已编译到 GryceCore；本 target 仅传播头文件和链接依赖。
if(GRYCE_ENABLE_SCRIPT)
    add_library(GryceScriptStatic INTERFACE)
    target_include_directories(GryceScriptStatic INTERFACE
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(GryceScriptStatic INTERFACE
        GryceCore
        GryceQuickJS
    )
    set_target_properties(GryceScriptStatic PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- UI 库（可选，独立于 ECS）--------------------------------------------
if(GRYCE_ENABLE_UI)
    add_library(GryceUiStatic STATIC
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
        ${GRYCE_UTILS_SRC_DIR}/ui/dsl/semantic_analyzer.cpp
        ${GRYCE_UTILS_SRC_DIR}/ui/dsl/ast_optimizer.cpp
    )
    target_include_directories(GryceUiStatic PUBLIC
        ${GRYCE_UTILS_INCLUDE_DIR}
        ${GRYCE_UTILS_SRC_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(GryceUiStatic PUBLIC
        GryceRendererStatic
        GryceQuickJS
    )
    # 双轨解析开关：DSL 为现行默认路径；原 pugixml XML 路径仍保留供回退
    target_compile_definitions(GryceUiStatic PUBLIC GRYCE_UIF_DSL=1)
    set_target_properties(GryceUiStatic PROPERTIES FOLDER "GryceEngineUtils")
endif()

# ---- 统一入口 target：按选项聚合所有启用的模块 ---------------------------
add_library(GryceEngineUtils INTERFACE)
target_include_directories(GryceEngineUtils INTERFACE ${GRYCE_UTILS_INCLUDE_DIR})
target_link_libraries(GryceEngineUtils INTERFACE
    GryceRendererStatic
    GrycePlatformStatic
    GryceMathStatic
)
if(GRYCE_ENABLE_ECS)
    target_link_libraries(GryceEngineUtils INTERFACE GryceEcsStatic)
endif()
if(GRYCE_ENABLE_PHYSICS)
    target_link_libraries(GryceEngineUtils INTERFACE GrycePhysicsStatic)
endif()
if(GRYCE_ENABLE_AUDIO)
    target_link_libraries(GryceEngineUtils INTERFACE GryceAudioStatic)
endif()
if(GRYCE_ENABLE_SCRIPT)
    target_link_libraries(GryceEngineUtils INTERFACE GryceScriptStatic)
endif()
if(GRYCE_ENABLE_UI)
    target_link_libraries(GryceEngineUtils INTERFACE GryceUiStatic)
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

# ---------------------------------------------------------------------------
# gryce_dist — 把 flat 输出整理成自包含的 dist 布局（CMake 直接构建与 build.py 共走）
#   build/<Config>/bin/<Config>/dist/
#       GryceGC/ / GryceGame/ / GryceEditor/ / GryceTests/   每目标自包含（exe + 依赖 DLL）
#       GryceEngineUtils/{lib, include}                       库目录（Core + GryceEngineUtils）
# 挂在 ALL 下，`cmake --build .`（无 --target）即会执行；也可单独 --target gryce_dist。
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(gryce_dist ALL
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/assemble_dist.py"
            --build-dir "${CMAKE_BINARY_DIR}"
            --config "$<CONFIG>"
            --source-dir "${CMAKE_SOURCE_DIR}"
        COMMENT "Assembling GryceEngine dist layout (per-target folders + lib/include)"
        VERBATIM
    )
    set_target_properties(gryce_dist PROPERTIES FOLDER "GryceEngineUtils")
    foreach(_dist_dep GryceGC GryceGame gryce_tests GryceEditor)
        if(TARGET ${_dist_dep})
            add_dependencies(gryce_dist ${_dist_dep})
        endif()
    endforeach()
endif()
