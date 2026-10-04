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
    link_libraries(-O1 -g1)
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

# ===========================================================================
# Vulkan SDK：可选依赖，find_package 读取 VULKAN_SDK 环境变量（任何机器装好
# LunarG SDK 后都会设置）。只取头文件路径，不链接 MSVC 格式的 vulkan-1.lib
# （MinGW 链接该导入库会导致设备级函数指针错乱）。所有 Vulkan 函数改为运行时
# 从 vulkan-1.dll 加载（见 vk_loader.cpp）。
# 必须在 add_subdirectory(engine) 之前探测：engine/CMakeLists.txt 与下面的
# CTest 注册都要读 GRYCE_HAS_VULKAN，而子目录里的变量不会回传到父作用域。
# ===========================================================================
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    set(GRYCE_HAS_VULKAN TRUE)
else()
    set(GRYCE_HAS_VULKAN FALSE)
    message(STATUS "Vulkan SDK not found: Vulkan backend and its smoke test are disabled")
endif()

# ===========================================================================
# 渲染引擎本体：engine/ 下的 GryceCore.dll / GryceRenderer.dll / GrycePlatform.dll
# ===========================================================================
add_subdirectory(engine)

# ===========================================================================
# Vulkan 头文件封装：只暴露 include 路径与 VMA 的"动态加载"开关，不链接
# vulkan-1.lib（见上方说明）。
# ===========================================================================
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
# 启用像素统计（不加 --no-analyze）：renderer_smoke 的判定是"场景里有
# MeshRenderer 却不足 20% 非背景像素"即失败。曾经的假阴性来自 demo_project
# 的 models/ 从未入库（场景 5 个 MeshRenderer、非背景像素 0.04%），
# 而不是阈值本身不合适 —— 模型补齐后 256x256 也有约 60%，余量充足。
# 保留统计才能守住"克隆后场景空白"这类回归。
if(BUILD_TESTING)
    add_test(NAME renderer_smoke_opengl
             COMMAND GryceRenderSmoke --api opengl --frames 8 --w 256 --h 256
                     --shot "${CMAKE_BINARY_DIR}/smoke_opengl.png"
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(renderer_smoke_opengl PROPERTIES
        LABELS "render;smoke" TIMEOUT 120)
    if(GRYCE_HAS_VULKAN)
        add_test(NAME renderer_smoke_vulkan
                 COMMAND GryceRenderSmoke --api vulkan --frames 8 --w 256 --h 256
                         --shot "${CMAKE_BINARY_DIR}/smoke_vulkan.png"
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
# 第三方运行库（glfw3.dll / glew32.dll）也一并同步：它们来自依赖前缀，
# 非 MSYS2 shell 里启动时 PATH 上不一定有，复制到 exe 同目录最稳。
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
    ${_gryce_mingw_rt_cmds}
    DEPENDS ${GRYCE_DLL_TARGETS} GryceRenderSmoke
    COMMENT "Syncing engine, third-party and MinGW runtime DLLs next to GryceRenderSmoke.exe"
    VERBATIM
)
unset(_gryce_mingw_rt_cmds)
