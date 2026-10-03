cmake_minimum_required(VERSION 3.24)

project(GryceEngineRenderer
        VERSION 0.1.0
        DESCRIPTION "Gryce Engine Renderer: GryceCore + GryceRenderer + GrycePlatform"
        LANGUAGES C CXX)

option(GRYCE_ENABLE_IMGUI "Enable ImGui editor overlay in GryceRenderer" ON)

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
    link_libraries(-O1 -g1)
endif()
# 使用 LLVM lld 链接，避免 GNU ld 处理大型对象崩溃
find_program(LLD_LINKER ld.lld)
if(LLD_LINKER)
    add_link_options(-fuse-ld=lld)
endif()

# ===========================================================================
# GLFW：使用系统安装（MSYS2/ucrt64）。共享库语义：引擎 DLL 与可执行文件
# 各自调用 GLFW，动态库由 OS 加载一次，故使用 glfw3.dll 的导入库 libglfw3.dll.a。
# ===========================================================================
find_path(GLFW3_INCLUDE_DIR GLFW/glfw3.h
    PATHS D:/msys64/ucrt64/include)
find_library(GLFW3_LIBRARY NAMES glfw3 glfw
    PATHS D:/msys64/ucrt64/lib)
set(GLFW3_DLL "_glfw_dll_NOTFOUND")
find_file(GLFW3_DLL glfw3.dll
    PATHS D:/msys64/ucrt64/bin)
if(NOT GLFW3_INCLUDE_DIR OR NOT GLFW3_LIBRARY OR NOT GLFW3_DLL)
    message(FATAL_ERROR "GLFW not found: need GLFW/glfw3.h + libglfw3.dll.a + glfw3.dll under MSYS2/ucrt64")
endif()
add_library(glfw SHARED IMPORTED)
set_target_properties(glfw PROPERTIES
    IMPORTED_LOCATION "${GLFW3_DLL}"
    IMPORTED_IMPLIB   "${GLFW3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${GLFW3_INCLUDE_DIR}")

# ===========================================================================
# 物理依赖：Box2D（2D 刚体）+ Jolt Physics（3D 刚体）
# 两者均由 MSYS2/ucrt64 提供 CMake 配置包（box2dConfig / JoltConfig），
# 导入目标为 box2d::box2d 与 Jolt::Jolt（均为共享库）。
# 配置包只导出 RELEASE 配置，Debug/RelWithDebInfo 构建经 MAP_IMPORTED_CONFIG
# 回退到 RELEASE 导入库，避免 "configuration not found" 报错。
# ===========================================================================
find_package(box2d QUIET CONFIG
    PATHS D:/msys64/ucrt64/lib/cmake/box2d D:/msys64/ucrt64/lib/cmake)
find_package(Jolt QUIET CONFIG
    PATHS D:/msys64/ucrt64/lib/cmake/Jolt D:/msys64/ucrt64/lib/cmake)

if(NOT TARGET box2d::box2d)
    message(FATAL_ERROR "Box2D not found: install mingw-w64-ucrt-x86_64-box2d")
endif()
if(NOT TARGET Jolt::Jolt)
    message(FATAL_ERROR "Jolt Physics not found: install mingw-w64-ucrt-x86_64-jolt")
endif()
foreach(_cfg DEBUG RELWITHDEBINFO MINSIZEREL)
    set_target_properties(box2d::box2d PROPERTIES MAP_IMPORTED_CONFIG_${_cfg} RELEASE)
    set_target_properties(Jolt::Jolt PROPERTIES MAP_IMPORTED_CONFIG_${_cfg} RELEASE)
endforeach()
unset(_cfg)
message(STATUS "Box2D: ${box2d_DIR}")
message(STATUS "Jolt Physics: ${Jolt_DIR}")

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
# 渲染引擎本体：engine/ 下的 GryceCore.dll / GryceRenderer.dll / GrycePlatform.dll
# ===========================================================================
add_subdirectory(engine)

# ===========================================================================
# Vulkan SDK：find_package 依赖 VULKAN_SDK 环境变量。只取头文件路径，不链接
# MSVC 格式的 vulkan-1.lib（MinGW/ucrt64 链接该导入库会导致设备级函数指针错乱）。
# 所有 Vulkan 函数改为运行时从 vulkan-1.dll 加载（见 vk_loader.cpp）。
# ===========================================================================
find_package(Vulkan QUIET)

add_library(VulkanHeaders INTERFACE)
target_include_directories(VulkanHeaders INTERFACE
    ${Vulkan_INCLUDE_DIRS}
)
# VMA 默认 VMA_STATIC_VULKAN_FUNCTIONS=1 会静态引用裸 vkXxx 符号，要求链接期
# 绑定 vulkan-1.lib——本项目所有 Vulkan 函数均改造为运行时动态加载，故关静态开动态。
target_compile_definitions(VulkanHeaders INTERFACE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
)

# ===========================================================================
# 测试：接入 CTest（BUILD_TESTING 由 include(CTest) 提供，默认 ON）。
# GryceRenderSmoke 是渲染器全链路冒烟测试：初始化双后端、走帧管线、
# 截图并统计像素，画面非"纯清屏色"即判定渲染链路生效。
# ===========================================================================
include(CTest)

# ===========================================================================
# 渲染器冒烟测试：验证双后端(GryceCore+GryceRenderer+GrycePlatform)初始化、
# 帧管线、截图与清理。运行后自动把引擎 DLL 复制到 exe 同目录。
# ===========================================================================
set(GRYCE_DLL_TARGETS GryceRenderer GryceCore GrycePlatform)

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

# 注册 CTest：每个后端一条冒烟测试（小视口 + 少量帧以便 CI 快速跑完）。
# 用 --no-analyze：renderer_smoke 内置的像素统计阈值针对参考大场景，在
# 小视口下网格占比不足会误判渲染失效（假阴性）。CI 冒烟聚焦"初始化 +
# 走帧管线 + 截图生成 + 不崩溃"，退出码 0 即通过；像素内容统计留给
# 开发者在本地 --frames 大视口下人工对照。
if(BUILD_TESTING)
    add_test(NAME renderer_smoke_opengl
             COMMAND GryceRenderSmoke --api opengl --frames 8 --w 256 --h 256
                     --no-analyze --shot "${CMAKE_BINARY_DIR}/smoke_opengl.png"
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(renderer_smoke_opengl PROPERTIES
        LABELS "render;smoke" TIMEOUT 120)
    if(GRYCE_HAS_VULKAN)
        add_test(NAME renderer_smoke_vulkan
                 COMMAND GryceRenderSmoke --api vulkan --frames 8 --w 256 --h 256
                         --no-analyze --shot "${CMAKE_BINARY_DIR}/smoke_vulkan.png"
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(renderer_smoke_vulkan PROPERTIES
            LABELS "render;smoke" TIMEOUT 120)
    endif()
endif()

# 引擎 DLL 同步到 exe 目录。
# 注意：不能用 add_custom_command(... POST_BUILD) —— 那条命令只在
# GryceRenderSmoke 自身被重新链接时执行；若只改了引擎代码，GryceCore/
# GryceRenderer 会重新链接而 exe 不会，于是 exe 目录里留下旧 DLL，
# 运行时加载的是过期二进制（排查"改了代码却看不到变化"的经典陷阱）。
# 这里用始终执行的 ALL 目标 + copy_if_different，幂等且不会漏拷。
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
    DEPENDS ${GRYCE_DLL_TARGETS} GryceRenderSmoke
    COMMENT "Syncing engine DLLs next to GryceRenderSmoke.exe"
    VERBATIM
)
