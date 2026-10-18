# ===========================================================================
# engine/CMakeLists.txt
# Gryce 引擎核心镜像根：全量移植参考工程的渲染器及其依赖兄弟模块。
# include 根以 engine/ 为基准（参考以 src/ 为基准，镜像后零改动）。
# 命名空间统一为 gryce_engine。保留参考的 DLL 划分：
#   GryceCore     （抽象层 + 依赖模块 math/utils/resources/assets/scene/
#                   components/ecs/animation/reflection/script + render 抽象层）
#   GryceRenderer （管线 + 后端 renderer_rd/storage_rd/opengl/vulkan/高层管线）
#   GrycePlatform （Window/Input/Cursor）
#   GryceAudio    （静态音频模块，编译进 GryceCore）
# 第三方由根 CMakeLists 提供：glfw、GLEW::GLEW、box2d::box2d、Jolt::Jolt、
# amplitude（以上均为系统包优先、缺失时 FetchContent 拉源码构建）、
# VulkanHeaders、third_party/nlohmann_json、third_party/quickjs(GryceQuickJS target)。
# ===========================================================================

# ---------------------------------------------------------------------------
# 库类型：桌面按 DLL 分模块（GryceCore / GryceRenderer / GrycePlatform），
# Emscripten 下三个都编成静态库，与 smoke main 链成单一 wasm
#（side-module/DLL 在 Emscripten 下没有意义）。
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
    set(_gryce_lib_type STATIC)
else()
    set(_gryce_lib_type SHARED)
endif()

# ---------------------------------------------------------------------------
# GryceCore.dll
# ---------------------------------------------------------------------------
add_library(GryceCore ${_gryce_lib_type})

set_target_properties(GryceCore PROPERTIES
    OUTPUT_NAME GryceCore RUNTIME_OUTPUT_NAME GryceCore PREFIX ""
)

target_compile_definitions(GryceCore PRIVATE GRYCE_CORE_BUILDING)
target_compile_definitions(GryceCore PUBLIC GRYCE_BUILD_SHARED)
target_compile_definitions(GryceCore PUBLIC GRYCE_HAS_NLOHMANN_JSON)

target_sources(GryceCore
    PRIVATE
        # C API
        api/core_api.cpp
        api/entity_api.cpp
        api/component_api.cpp
        api/scene_api.cpp
        api/asset_api.cpp
        api/material_api.cpp
        api/animator_api.cpp
        runtime/command_buffer.cpp
        runtime/entity_handle_map.cpp
        # 内部模块
        math/math.cpp
        math/camera.cpp
        utils/glog/glog_lib.cpp
        utils/frame_limiter.cpp
        utils/crypto/crc32.cpp
        utils/crypto/chacha20.cpp
        utils/crypto/random.cpp
        resources/project.cpp
        resources/resource_path.cpp
        resources/gpack_bundle.cpp
        resources/pak_bundle.cpp
        resources/aes_gcm.cpp
        resources/resource_loader.cpp
        assets/asset_manager.cpp
        assets/async_loader.cpp
        assets/material_rules.cpp
        assets/obj_loader.cpp
        assets/mesh_optimizer.cpp
        assets/mesh_lod.cpp
        assets/stb_image_impl.cpp
        ${CMAKE_SOURCE_DIR}/third_party/stb/stb_image_write_impl.cpp
        assets/compressed_image.cpp
        assets/dds_loader.cpp
        assets/ktx_loader.cpp
        assets/tinyexr_impl.cpp
        ${CMAKE_SOURCE_DIR}/third_party/tinyexr/miniz.c
        animation/skeleton.cpp
        animation/animation_clip.cpp
        animation/pose.cpp
        components/component_factory.cpp
        components/2d/component_2d.cpp
        components/2d/sprite_2d.cpp
        components/2d/tilemap.cpp
        components/2d/parallax_background.cpp
        components/2d/particle_emitter.cpp
        components/2d/skybox_2d.cpp
        components/3d/particle_system_3d.cpp
        components/3d/line_renderer_3d.cpp
        components/3d/trail_renderer.cpp
        components/3d/instanced_mesh_renderer.cpp
        components/3d/billboard.cpp
        components/3d/text_mesh3d.cpp
        components/3d/lod_group.cpp
        components/3d/volumetric_light.cpp
        components/mesh_renderer.cpp
        components/skinned_mesh_renderer.cpp
        components/terrain.cpp
        reflection/builtin_reflections.cpp
        reflection/reflection.cpp
        scene/uuid.cpp
        scene/entity.cpp
        scene/scene.cpp
        scene/prefab.cpp
        scene/scene_serializer.cpp
        scene/multi_material_assembly.cpp
        ecs/world.cpp
        ecs/component_store.cpp
        ecs/systems/animator_system.cpp
        ecs/systems/hierarchy_system.cpp
        # 物理（Box2D 2D / Jolt 3D）
        ecs/systems/physics_system_2d.cpp
        ecs/systems/physics_system_3d.cpp
        # 音频（Amplitude Audio SDK）
        ecs/systems/audio_system.cpp
        # Render 抽象（核心，与 backend 无关）
        render/render_context.cpp
        render/material.cpp
        render/render_command_buffer.cpp
        render/render_thread.cpp
        render/mesh.cpp
        render/shader.cpp
        render/texture.cpp
        render/framebuffer.cpp
        render/render2d.cpp
        render/font_atlas.cpp
        render/ibl_generator.cpp
)

target_include_directories(GryceCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/tinyexr
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann_json
)

# GryceAudio 编译进 Core
if(WIN32)
    target_link_libraries(GryceCore PUBLIC imm32 winmm bcrypt)
endif()

# 物理：Box2D（2D 刚体）+ Jolt Physics（3D 刚体）由 GryceCore 直接使用并对外传递
target_link_libraries(GryceCore PUBLIC box2d::box2d)
target_link_libraries(GryceCore PUBLIC Jolt::Jolt)

# 音频：Amplitude Audio SDK（静态库，含 flatbuffers/samplerate 私有依赖，见根 CMake）
target_link_libraries(GryceCore PUBLIC amplitude)

# 模型导入：Assimp（FBX/glTF/DAE/PLY/STL/3DS/Blend）。目标由根 CMakeLists
# 统一准备（系统包或 FetchContent 源码）；缺失时整个块不生效，
# asset_manager.cpp 走 #else 分支（非 OBJ 格式报错返回 nullptr，OBJ 不受影响）。
if(TARGET assimp::assimp)
    target_sources(GryceCore PRIVATE assets/assimp_importer.cpp)
    target_compile_definitions(GryceCore PUBLIC GRYCE_HAS_ASSIMP)
    target_link_libraries(GryceCore PUBLIC assimp::assimp)
endif()

# ---------------------------------------------------------------------------
# GryceRenderer.dll
# ---------------------------------------------------------------------------
add_library(GryceRenderer ${_gryce_lib_type})

set_target_properties(GryceRenderer PROPERTIES
    OUTPUT_NAME GryceRenderer RUNTIME_OUTPUT_NAME GryceRenderer PREFIX ""
)

target_compile_definitions(GryceRenderer PRIVATE GRYCE_RENDERER_BUILDING)
target_compile_definitions(GryceRenderer PUBLIC GRYCE_BUILD_SHARED)

target_sources(GryceRenderer
    PRIVATE
        api/render_api.cpp
        render/render.cpp
        render/renderer2d_impl.cpp
        render/core_shaders.cpp
        render/shader_source_resolver.cpp
        render/render_forward_basic.cpp
        render/renderer_canvas_render_rd.cpp
        render/compositor_basic.cpp
        render/compositor_forward_clustered.cpp
        render/renderer_rd/cluster_builder_rd.cpp
        render/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp
        render/renderer_rd/forward_clustered/render_forward_clustered.cpp
        render/renderer_rd/forward_clustered/shadow_system_rd.cpp
        render/renderer_rd/shadow/shadow_atlas.cpp
        render/renderer_rd/shadow/vsm_esm_shadow.cpp
        render/renderer_rd/effects/ssr.cpp
        render/renderer_rd/effects/ssil.cpp
        render/renderer_rd/effects/bokeh_dof.cpp
        render/renderer_rd/effects/fsr2.cpp
        render/renderer_rd/effects/subsurface_scattering.cpp
        render/renderer_rd/effects/motion_blur.cpp
        render/renderer_rd/effects/water.cpp
        render/renderer_rd/environment/sdfgi.cpp
        render/renderer_rd/environment/voxel_gi.cpp
        render/renderer_rd/environment/fog.cpp
        render/renderer_rd/environment/reflection_probe.cpp
        render/storage_rd/mesh_storage_impl.cpp
        render/storage_rd/material_storage_impl.cpp
        render/storage_rd/texture_storage_impl.cpp
        render/storage_rd/light_storage_impl.cpp
        render/storage_rd/decal_storage.cpp
        render/compositor_effect.cpp
        render/rendering_server_default.cpp
        render/render_pipeline.cpp
        render/shadow_pass.cpp
        render/postprocess_pass.cpp
        render/renderer_compositor.cpp
        ecs/systems/subviewport_system.cpp
        render/opengl/gl_backend.cpp
        render/opengl/gl_frame_pacing.cpp
        render/opengl/gl_buffer.cpp
        render/opengl/gl_shader.cpp
        render/opengl/gl_texture.cpp
        render/opengl/gl_framebuffer.cpp
        render/opengl/gl_imgui_backend.cpp
        render/opengl/imgui_renderer.cpp
        ecs/systems/render_system_2d.cpp
        ecs/systems/render_system_3d.cpp
)

# ImGui 源（M4 阶段启用）
if(GRYCE_ENABLE_IMGUI)
    target_sources(GryceRenderer
        PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp
            ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_demo.cpp
            ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
            ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
            ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
            ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_glfw.cpp
            ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(GryceRenderer PUBLIC ${CMAKE_SOURCE_DIR}/third_party/imgui)
    # 引擎自带的调试 UI（左上角 FPS/帧时间 + ImGui 调试窗口，见 api/render_api.cpp）
    # 需要知道"ImGui 核心源已编入本 DLL"，否则不能 include <imgui.h>。
    target_compile_definitions(GryceRenderer PRIVATE GRYCE_HAS_IMGUI)
    if(WIN32)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp PROPERTIES COMPILE_FLAGS -w)
    endif()
endif()

# Vulkan 后端
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    set(GRYCE_HAS_VULKAN TRUE)
    target_include_directories(GryceRenderer PUBLIC ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(GryceRenderer PUBLIC GRYCE_HAS_VULKAN)
    target_sources(GryceRenderer
        PRIVATE
            render/vulkan/vk_backend.cpp
            render/vulkan/vk_buffer.cpp
            render/vulkan/vk_compute.cpp
            render/vulkan/vk_device.cpp
            render/vulkan/vk_framebuffer.cpp
            render/vulkan/vk_imgui_backend.cpp
            render/vulkan/vk_instance.cpp
            render/vulkan/vk_mesh.cpp
            render/vulkan/vk_renderer2d.cpp
            render/vulkan/vk_shader.cpp
            render/vulkan/vk_spirv_cache.cpp
            render/vulkan/vk_swapchain.cpp
            render/vulkan/vk_texture.cpp
            render/vulkan/vk_vma.cpp
    )
    if(GRYCE_ENABLE_IMGUI)
        target_sources(GryceRenderer PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_vulkan.cpp
        )
    endif()
    # VMA 采用运行时动态导入（避免静态链接裸 vkXxx 符号）
    target_compile_definitions(GryceRenderer PRIVATE
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    )
    # 链接系统 Vulkan 加载器导入库，解析裸 vkXxx（vulkan-1 动态调度，安全）
    if(TARGET Vulkan::Vulkan)
        target_link_libraries(GryceRenderer PRIVATE Vulkan::Vulkan)
    elseif(Vulkan_LIBRARIES)
        target_link_libraries(GryceRenderer PRIVATE ${Vulkan_LIBRARIES})
    endif()
else()
    set(GRYCE_HAS_VULKAN FALSE)
endif()

# ---------------------------------------------------------------------------
# D3D12 后端（WinNative 方向）
# 探测条件：
#   * Windows + GCC/Clang 编译器
#   * 能找到 d3d12.h（mingw-w64 vendor 头优先，缺时回退 Windows SDK 头）
# 链接策略：链接期不链 d3d12.lib / dxgi.lib —— 后端走 LoadLibrary + GetProcAddress
# （engine/render/d3d12/d3d12_loader.cpp），避免 MinGW 链 MSVC 导入库的坑。
# 缺失时静默关掉，--api dx12 在工厂里 GLOG_ERROR + 返回 nullptr。
#
# 关键教训（2026-10-06）：SDK um/ 头通过 <windows.h> 拖进 objidlbase.h/stralign.h/
# winioctl.h 等，里面大量用 __uuidof / PDEVICE_DSM_RANGE / wcscpy_s 等 MSVC 专属符号，
# MinGW 自带头与之不兼容，会让整个 GryceRenderer 编译失败。所以默认走 mingw-w64
# vendor 头（已经过 M0 探针验证：test/tools/d3d12_m0_clear.cpp）。SDK 头仅作
# private include 给 d3d12_backend.cpp / d3d12_loader.cpp 这两个文件，不污染其他 TU。
# ---------------------------------------------------------------------------
set(GRYCE_HAS_D3D12 FALSE)
# D3D12 后端：18 个源文件全部编译链接通过，`--api dx12` 在 demo_project 上与
# OpenGL 截图逐像素一致（平均 |Δ| = 0.06/255，见 docs/zh/plans/d3d12-backend-plan.md
# 的「实施现状」）。2026-10-13 起默认 ON：
#   * M0–M6 全部落地，已知缺陷（含 SSR 偶发整帧丢失的跨线程竞争）均已修复；
#   * GPU 剔除（compute 视锥 + HiZ 遮挡 + 间接绘制）实测与 CPU 路径视觉等价；
#   * 运行时动态加载 d3d12.dll/dxgi.dll/dxcompiler.dll，老 Windows 缺失时
#     后端自己静默降级，不影响 GL/Vulkan 主线；
#   * 编译期按头文件探测：mingw vendor 头或 Windows SDK 头**都不存在**的
#     机器上依旧静默关闭（与 GRYCE_HAS_VULKAN 同一套写法），配置不失败。
# 需要关闭时显式 -DGRYCE_ENABLE_D3D12=OFF。
option(GRYCE_ENABLE_D3D12 "启用 D3D12 后端" ON)
if(GRYCE_ENABLE_D3D12 AND WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # mingw-w64 vendor 头路径：$MINGW_ROOT/x86_64-w64-mingw32/include/，
    # 不是 $MINGW_ROOT/include/。CLion 自带 mingw 路径示例：
    #   D:/JetBrains/CLion 2026.2.3.1/bin/mingw/x86_64-w64-mingw32/include/d3d12.h
    get_filename_component(_gryce_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_gryce_mingw_root "${_gryce_cxx_dir}/..")
    set(_gryce_d3d12_header_found FALSE)

    # vendor 头候选目录：CLion 自带 mingw 放在 x86_64-w64-mingw32/include/，
    # 而 MSYS2 UCRT64 把 d3d12.h 直接放在 <root>/include/。两处都探。
    set(_gryce_vendor_includes
        "${_gryce_mingw_root}/x86_64-w64-mingw32/include"
        "${_gryce_mingw_root}/include")
    foreach(_gryce_inc IN LISTS _gryce_vendor_includes)
        if(EXISTS "${_gryce_inc}/d3d12.h")
            # mingw vendor 头优先（已 M0 验证）。无需任何额外 include —— GCC 默认就
            # 知道这路径。
            set(_gryce_d3d12_header_found TRUE)
            break()
        endif()
    endforeach()

    if(NOT _gryce_d3d12_header_found)
        # 回退到 Windows SDK 头。SDK um/ 头与 mingw 头不能并存，所以只把 SDK
        # include 加给 d3d12 后端的两个 TU。
        foreach(_gryce_sdk_ver IN ITEMS "10.0.26100.0" "10.0.22621.0" "10.0.19041.0")
            if(EXISTS "C:/Program Files (x86)/Windows Kits/10/Include/${_gryce_sdk_ver}/um/d3d12.h")
                set(_gryce_d3d12_sdk_root
                    "C:/Program Files (x86)/Windows Kits/10/Include/${_gryce_sdk_ver}")
                set(_gryce_d3d12_header_found TRUE)
                break()
            endif()
        endforeach()
    endif()

    if(_gryce_d3d12_header_found)
        set(GRYCE_HAS_D3D12 TRUE)
        target_compile_definitions(GryceRenderer PUBLIC GRYCE_HAS_D3D12)
        if(_gryce_d3d12_sdk_root)
            # 仅给 d3d12 backend 的 .cpp 用 SDK 头，其余 TU 不受影响。
            set_source_files_properties(
                render/d3d12/d3d12_loader.cpp
                render/d3d12/d3d12_backend.cpp
                render/d3d12/d3d12_descriptors.cpp
                render/d3d12/d3d12_upload.cpp
                render/d3d12/d3d12_buffer.cpp
                render/d3d12/d3d12_texture.cpp
                render/d3d12/d3d12_mesh.cpp
                render/d3d12/d3d12_framebuffer.cpp
                render/d3d12/d3d12_vulkanize.cpp
                render/d3d12/d3d12_spirv_cross.cpp
                render/d3d12/d3d12_dxc.cpp
                render/d3d12/d3d12_shader.cpp
                render/d3d12/d3d12_reflect.cpp
                render/d3d12/d3d12_compute.cpp
                PROPERTIES INCLUDE_DIRECTORIES "${_gryce_d3d12_sdk_root}/um;${_gryce_d3d12_sdk_root}/shared")
        endif()
        target_sources(GryceRenderer
            PRIVATE
                render/d3d12/d3d12_loader.cpp
                render/d3d12/d3d12_swapchain.cpp
                render/d3d12/d3d12_descriptors.cpp
                render/d3d12/d3d12_upload.cpp
                render/d3d12/d3d12_buffer.cpp
                render/d3d12/d3d12_texture.cpp
                render/d3d12/d3d12_mesh.cpp
                render/d3d12/d3d12_framebuffer.cpp
                render/d3d12/d3d12_vulkanize.cpp
                render/d3d12/d3d12_spirv_cross.cpp
                render/d3d12/d3d12_dxc.cpp
                render/d3d12/d3d12_shader.cpp
                render/d3d12/d3d12_reflect.cpp
                render/d3d12/d3d12_compute.cpp
                render/d3d12/d3d12_renderer2d.cpp
                render/d3d12/d3d12_backend.cpp
        )
        # ImGui 的 DX12 渲染后端（M5.2）。ImGui 核心源由 GRYCE_ENABLE_IMGUI 段提供，
        # 这里只在启用 ImGui 时补上 imgui_impl_dx12.cpp 与本后端的包装。
        if(GRYCE_ENABLE_IMGUI)
            target_compile_definitions(GryceRenderer PRIVATE GRYCE_D3D12_HAS_IMGUI)
            target_sources(GryceRenderer PRIVATE
                render/d3d12/d3d12_imgui_backend.cpp
                ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_dx12.cpp
            )
            # imgui_impl_dx12.cpp 直接调用 CreateDXGIFactory1 / D3DCompile，会要求
            # 链接 dxgi.lib / d3dcompiler.lib（违背本项目"零 MSVC 导入库"的路线）。
            # 用强制包含的 shim 头把这两个名字宏替换成走 LoadLibrary 的转发函数。
            set_source_files_properties(
                ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_dx12.cpp
                PROPERTIES COMPILE_OPTIONS
                "-include;${CMAKE_CURRENT_SOURCE_DIR}/render/d3d12/d3d12_imgui_compile_shim.h"
            )
        endif()
        if(_gryce_d3d12_sdk_root)
            set(_gryce_d3d12_headers_desc "Windows SDK (${_gryce_d3d12_sdk_root})")
        else()
            set(_gryce_d3d12_headers_desc "mingw-w64 vendor")
        endif()
        message(STATUS "Gryce: D3D12 backend enabled (full; headers: ${_gryce_d3d12_headers_desc})")
    else()
        message(STATUS "Gryce: D3D12 headers not found (mingw vendor or Windows SDK); backend disabled")
    endif()
endif()

# ---------------------------------------------------------------------------
# GLSL → SPIR-V 编译单元（render/glsl_compiler.cpp，shaderc 运行时动态加载）
#
# 该文件自身不依赖任何后端头文件，但 Vulkan 的 vk_spirv_cache 与 D3D12 的
# vulkanize 链路都要调用 compile_glsl_to_spirv。它历史上放在 vulkan/ 下、
# 只在 Vulkan_FOUND 时编入目标，导致无 Vulkan 的机器（CI 的 MSYS2 环境不装
# Vulkan 包）启用 D3D12 时链接期缺符号。故改为"任一路径启用即编译"。
# ---------------------------------------------------------------------------
if(GRYCE_HAS_VULKAN OR GRYCE_HAS_D3D12)
    target_sources(GryceRenderer PRIVATE render/glsl_compiler.cpp)
endif()

target_include_directories(GryceRenderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann_json
)

# Emscripten（WebGL2）：把 <GL/glew.h> 重定向到 DSA shim
#（cmake/web/GL/glew.h → engine/render/opengl/gl_webgl_shim.h）。
# 桌面构建不加这个目录，9 处 #include <GL/glew.h> 零改动。
if(EMSCRIPTEN)
    target_include_directories(GryceRenderer PUBLIC
        ${CMAKE_SOURCE_DIR}/cmake/web
    )
    # ImGui 的 GL 后端以 GLES3 路径编译（imgui_impl_opengl3.h 按此宏
    # 包含 <GLES3/gl3.h> 并使用 ES 300 着色器前缀）。
    target_compile_definitions(GryceRenderer PUBLIC IMGUI_IMPL_OPENGL_ES3)
endif()

target_link_libraries(GryceRenderer PUBLIC GryceCore)
# 渲染器依赖平台窗口/输入（render_api/render.cpp 调用 GWindow_*）
target_link_libraries(GryceRenderer PRIVATE GrycePlatform)
# OpenGL 1.1 固定函数（glClear/glViewport/glGenTextures 等）由 opengl32 直接导出
if(WIN32)
    target_link_libraries(GryceRenderer PRIVATE opengl32 gdi32)
endif()

# OpenGL：glew。由根 CMakeLists 统一准备 GLEW::GLEW（系统包或 FetchContent 源码），
# 这里只消费目标，避免子目录里再写死任何机器专属路径。
target_link_libraries(GryceRenderer PUBLIC GLEW::GLEW)

# GLFW
if(TARGET glfw)
    target_link_libraries(GryceRenderer PUBLIC glfw)
endif()

# ---------------------------------------------------------------------------
# GrycePlatform.dll
# ---------------------------------------------------------------------------
add_library(GrycePlatform ${_gryce_lib_type})

set_target_properties(GrycePlatform PROPERTIES
    OUTPUT_NAME GrycePlatform RUNTIME_OUTPUT_NAME GrycePlatform PREFIX ""
)

target_compile_definitions(GrycePlatform PRIVATE GRYCE_PLATFORM_BUILDING)
target_compile_definitions(GrycePlatform PUBLIC GRYCE_BUILD_SHARED)

target_sources(GrycePlatform
    PRIVATE
        api/platform_api.cpp
        platform/window.cpp
        platform/input.cpp
        platform/cursor.cpp
)

target_include_directories(GrycePlatform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(GrycePlatform PUBLIC GryceCore)
if(TARGET glfw)
    target_link_libraries(GrycePlatform PUBLIC glfw)
endif()
if(WIN32)
    target_link_libraries(GrycePlatform PUBLIC imm32)
endif()

# ---------------------------------------------------------------------------
# 引擎 DLL 重链接后立即同步到 exe 目录（${GRYCE_EXE_OUTPUT_DIR}，由顶层传入）。
#
# 为什么挂 POST_BUILD：日常迭代常用
#   cmake --build . --target GryceRenderer
# 单独构建引擎库，顶层 GryceSyncRuntimeDlls（ALL 目标）不参与，exe 目录
# 会留下旧 DLL，运行时加载过期二进制（"改了代码却看不到变化"的经典陷阱）。
# 命令必须挂在 DLL 自身而不是 exe 上：挂 exe 时改引擎代码 exe 不重链接、
# 不会触发；挂 DLL 则每次重链接必触发。
# add_custom_command(TARGET) 只接受本目录创建的 target，故三个引擎 DLL
# 都在此处理；第三方 DLL 是 IMPORTED 目标无法挂 POST_BUILD，仍由顶层
# GryceSyncRuntimeDlls 负责。copy_if_different 幂等，两处重复执行无副作用。
foreach(_gryce_dll IN ITEMS GryceCore GryceRenderer GrycePlatform)
    add_custom_command(TARGET ${_gryce_dll} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${_gryce_dll}>"
                "${GRYCE_EXE_OUTPUT_DIR}"
        COMMENT "Syncing ${_gryce_dll}.dll next to GryceRenderSmoke.exe"
        VERBATIM
    )
endforeach()
unset(_gryce_dll)
