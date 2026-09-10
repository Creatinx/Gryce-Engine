# src/CMakeLists.txt
# Gryce Engine 模块化核心库 — 拆分为 GryceCore / GryceRenderer / GrycePlatform / GrycePhysics

# ============================================================================
# GryceCore.dll — ECS、Scene、Entity、Component、Math、UUID、Reflection、Assets
# ============================================================================
# 有意识地保留在 Core 中的领域（本阶段不拆分，避免为拆而拆引入风险/循环）：
#   - 脚本（script/ + ecs/systems/script_system）：被 Core 内 ECS 系统强依赖，
#     且 src/ui 存在并行的脚本运行时，拆分收益低、风险高。
#   - 渲染抽象层（render/rendering_device.h、storage_rd/*、render 管线）：GryceRenderer
#     后端已依赖 Core，把抽象移出便会形成 DLL 循环。
#   - 资源加密格式（gpack/pak/aes_gcm）：与 resource_loader / asset_manager 交织，拆开收益低。
# 音频引擎已独立为 GryceAudio 模块（见下方定义）。
add_library(GryceCore SHARED)

set_target_properties(GryceCore PROPERTIES
    OUTPUT_NAME GryceCore
    RUNTIME_OUTPUT_NAME GryceCore
    PREFIX ""
    EXPORT_NAME GryceCore
    FOLDER "Core"
)

target_compile_definitions(GryceCore PRIVATE GRYCE_CORE_BUILDING)

target_sources(GryceCore
    PRIVATE
        # C API 实现（core_api 包含 g_core_state/api_mutex 等核心符号，必须留在 GryceCore）
        api/core_api.cpp
        api/entity_api.cpp
        api/component_api.cpp
        api/scene_api.cpp
        api/asset_api.cpp
        api/material_api.cpp
        api/animator_api.cpp
        api/script_api.cpp
        runtime/command_buffer.cpp
        runtime/entity_handle_map.cpp
        # 内部模块
        math/math.cpp
        math/camera.cpp
        utils/glog/glog_lib.cpp
        utils/frame_limiter.cpp
        resources/project.cpp
        resources/resource_path.cpp
        resources/gpack_bundle.cpp
        resources/pak_bundle.cpp
        resources/aes_gcm.cpp
        resources/resource_loader.cpp
        assets/asset_manager.cpp
        assets/async_loader.cpp
        assets/obj_loader.cpp
        assets/stb_image_impl.cpp
        ${CMAKE_SOURCE_DIR}/third_party/stb/stb_image_write_impl.cpp
        assets/compressed_image.cpp
        assets/dds_loader.cpp
        assets/ktx_loader.cpp
        assets/tinyexr_impl.cpp
        ${CMAKE_SOURCE_DIR}/third_party/tinyexr/miniz.c
        $<$<BOOL:${GRYCE_HAS_ASSIMP}>:assets/assimp_importer.cpp>
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
        components/audio_source.cpp
        components/mesh_renderer.cpp
        components/skinned_mesh_renderer.cpp
        components/terrain.cpp
        reflection/builtin_reflections.cpp
        reflection/reflection.cpp
        components/physical_material.cpp
        scene/uuid.cpp
        scene/entity.cpp
        scene/scene.cpp
        scene/prefab.cpp
        scene/scene_serializer.cpp
        script/big_number.cpp
        script/runtime/script_vm.cpp
        script/runtime/script_context.cpp
        script/bindings/engine_bridge.cpp
        script/bindings/math_bridge.cpp
        script/bindings/big_bridge.cpp
        ecs/systems/script_system.cpp
        ecs/world.cpp
        ecs/component_store.cpp
        ecs/systems/animator_system.cpp
        ecs/systems/fracture_system.cpp
        # subviewport_system 移至 GryceRenderer（依赖 RenderPipeline）
        ecs/systems/hierarchy_system.cpp
        # Render 核心（与 backend 无关）
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
        # 渲染管线实现在 GryceRenderer（render_pipeline.cpp 依赖 RD 新特性）
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            GryceCore/types.h
            GryceCore/core_api.h
            GryceCore/entity_api.h
            GryceCore/component_api.h
            GryceCore/scene_api.h
            GryceCore/asset_api.h
            GryceCore/material_api.h
            GryceCore/animator_api.h
            export.h
            components/component.h
            components/component_factory.h
            components/transform.h
            components/hierarchy_components.h
            components/physics_body.h
            components/node2d.h
            components/node3d.h
            components/camera.h
            components/light.h
            components/static_body.h
            components/rigid_body.h
            components/box_collider.h
            components/sphere_collider.h
            components/plane_collider.h
            components/audio_source.h
            components/audio_listener.h
            components/physical_material.h
            components/static_body_2d.h
            components/rigid_body_2d.h
            components/box_collider_2d.h
            components/circle_collider_2d.h
            components/destructible_body.h
            components/fragment_body.h
            components/2d/component_2d.h
            components/2d/basic_rect.h
            components/2d/shape.h
            components/2d/label.h
            components/2d/light_2d.h
            components/2d/sprite_2d.h
            components/2d/tilemap.h
            components/2d/camera_2d.h
            components/2d/parallax_background.h
            components/2d/particle_emitter.h
            components/3d/visual_components.h
            components/3d/colliders.h
            components/3d/navigation_components.h
            components/3d/system_components.h
            components/2d/anim_components.h
            components/2d/visual_components.h
            components/2d/physics_components.h
            components/2d/navigation_components.h
            components/2d/misc_components.h
            components/common/system_components.h
            components/audio_source.h
            resources/tileset.h
            components/mesh_renderer.h
            components/skinned_mesh_renderer.h
            components/terrain.h
            resources/project.h
            resources/resource_path.h
            resources/gpack_bundle.h
            resources/pak_bundle.h
            resources/aes_gcm.h
            resources/resource_loader.h
            assets/mesh_data.h
            assets/asset.h
            assets/asset_handle.h
            assets/asset_manager.h
            assets/async_loader.h
            assets/obj_loader.h
            assets/texture_data.h
            assets/skinned_mesh_data.h
            animation/skeleton.h
            animation/animation_clip.h
            animation/pose.h
            $<$<BOOL:${GRYCE_HAS_ASSIMP}>:assets/assimp_importer.h>
            scene/uuid.h
            scene/entity.h
            scene/scene.h
            scene/prefab.h
            scene/scene_serializer.h
            ecs/types.h
            ecs/system.h
            ecs/world.h
            scene/query.h
            ecs/component_store.h
            ecs/systems/fracture_system.h
            ecs/systems/animator_system.h
            ecs/systems/hierarchy_system.h
            math/math.h
            math/camera.h
            utils/glog/glog_lib.h
            utils/frame_limiter.h
            reflection/reflection.h
            # 渲染抽象/管线实现在 GryceCore（render_pipeline.cpp 等），头文件随实现归位
            render/render_pipeline.h
            # Godot 式渲染器抽象层
            render/rendering_device.h
            render/renderer_compositor.h
            render/renderer_scene_render.h
            render/renderer_fog.h
            render/renderer_gi.h
            render/storage_rd/light_storage.h
            render/storage_rd/material_storage.h
            render/storage_rd/mesh_storage.h
            render/storage_rd/texture_storage.h
            render/storage_rd/particles_storage.h
            render/storage_rd/utilities.h
)

target_include_directories(GryceCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/stb>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/tinyexr>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(GryceCore PUBLIC gryce::compile_options)

# GryceSRT 脚本运行时：链接 QuickJS 静态库
target_link_libraries(GryceCore PRIVATE GryceQuickJS)

# 不导出所有符号，避免 libstdc++ 等静态库符号污染
# 导出由各头文件中的 GRYCE_API / __declspec(dllexport) 控制

target_compile_definitions(GryceCore PUBLIC
    $<$<BOOL:${GRYCE_HAS_ASSIMP}>:GRYCE_HAS_ASSIMP>
    $<$<BOOL:${GRYCE_HAS_NLOHMANN_JSON}>:GRYCE_HAS_NLOHMANN_JSON>
)

# Windows: 链接 imm32（IME）、winmm（高精度定时器）和 bcrypt（CNG 随机数）
if(WIN32)
    target_link_libraries(GryceCore PUBLIC imm32 winmm bcrypt)
endif()

# 链接 nlohmann/json
if(GRYCE_HAS_NLOHMANN_JSON)
    target_link_libraries(GryceCore PUBLIC nlohmann_json::nlohmann_json)
endif()

# 链接 Assimp
if(GRYCE_HAS_ASSIMP)
    target_link_libraries(GryceCore PUBLIC assimp::assimp)
endif()

# 链接音频引擎模块（GryceAudio 见下方定义）。
# GryceCore 单向依赖 GryceAudio：音频引擎只依赖 header-only 的 math，
# 不反向引用 Core 符号，故无 DLL 循环。
target_link_libraries(GryceCore PRIVATE GryceAudio)

# ============================================================================
# GryceRenderer.dll — RenderContext、Pipeline、GL/Vulkan Backend、Mesh、Texture
# ============================================================================
add_library(GryceRenderer SHARED)

set_target_properties(GryceRenderer PROPERTIES
    OUTPUT_NAME GryceRenderer
    RUNTIME_OUTPUT_NAME GryceRenderer
    PREFIX ""
    EXPORT_NAME GryceRenderer
    FOLDER "Core"
)

target_compile_definitions(GryceRenderer PRIVATE GRYCE_RENDERER_BUILDING)

target_sources(GryceRenderer
    PRIVATE
        api/render_api.cpp
        render/render.cpp
        render/renderer2d_impl.cpp
        # Core 默认 shader：目录定位 + 两级解析（项目覆盖 → core 兜底）
        render/core_shaders.cpp
        render/shader_source_resolver.cpp
        # Godot 式渲染器实现
        render/render_forward_basic.cpp
        render/renderer_canvas_render_rd.cpp
        render/compositor_basic.cpp
        render/compositor_forward_clustered.cpp
        # Forward Clustered 渲染器
        render/renderer_rd/cluster_builder_rd.cpp
        render/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp
        render/renderer_rd/forward_clustered/render_forward_clustered.cpp
        render/renderer_rd/forward_clustered/shadow_system_rd.cpp
        # Shadow Atlas
        render/renderer_rd/shadow/shadow_atlas.cpp
        render/renderer_rd/shadow/vsm_esm_shadow.cpp
        # 后处理效果
        render/renderer_rd/effects/ssr.cpp
        render/renderer_rd/effects/ssil.cpp
        render/renderer_rd/effects/bokeh_dof.cpp
        render/renderer_rd/effects/fsr2.cpp
        render/renderer_rd/effects/subsurface_scattering.cpp
        render/renderer_rd/effects/motion_blur.cpp
        render/renderer_rd/effects/water.cpp
        # GI 系统
        render/renderer_rd/environment/sdfgi.cpp
        render/renderer_rd/environment/voxel_gi.cpp
        render/renderer_rd/environment/fog.cpp
        render/renderer_rd/environment/reflection_probe.cpp
        # Storage 系统实现
        render/storage_rd/mesh_storage_impl.cpp
        render/storage_rd/material_storage_impl.cpp
        render/storage_rd/texture_storage_impl.cpp
        render/storage_rd/light_storage_impl.cpp
        render/storage_rd/decal_storage.cpp
        # RenderingServer + Compositor Effect
        render/compositor_effect.cpp
        render/rendering_server_default.cpp
        # 渲染管线（依赖 RD 新特性，位于 GryceRenderer）
        render/render_pipeline.cpp
        render/shadow_pass.cpp
        render/postprocess_pass.cpp
        render/renderer_compositor.cpp
        # SubViewportSystem（依赖 RenderPipeline 等 RD 类型）
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
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            GryceRenderer/render_api.h
            GryceRenderer/viewport_api.h
            render/render.h
            render/mesh.h
            render/skinned_vertex.h
            render/shader.h
            render/texture.h
            render/material.h
            render/framebuffer.h
            render/render_command_buffer.h
            render/render_thread.h
            render/render_context.h
            render/render2d.h
            render/font_atlas.h
            render/renderer2d_impl.h
            render/imgui_backend.h
            # Core 默认 shader 头文件
            render/core_shaders.h
            render/shader_source_resolver.h
            # Godot 式渲染器实现头文件
            render/renderer_canvas_render.h
            render/render_forward_basic.h
            render/renderer_canvas_render_rd.h
            render/compositor_basic.h
            render/compositor_forward_clustered.h
            # Forward Clustered 头文件
            render/renderer_rd/cluster_builder_rd.h
            render/renderer_rd/forward_clustered/scene_shader_forward_clustered.h
            render/renderer_rd/forward_clustered/render_forward_clustered.h
            render/renderer_rd/forward_clustered/shadow_system_rd.h
            # 后处理效果头文件
            render/renderer_rd/effects/ssr.h
            render/renderer_rd/effects/ssil.h
            render/renderer_rd/effects/bokeh_dof.h
            render/renderer_rd/effects/fsr2.h
            # GI 系统头文件
            render/renderer_rd/environment/sdfgi.h
            render/renderer_rd/environment/voxel_gi.h
            render/renderer_rd/environment/fog.h
            render/renderer_rd/environment/reflection_probe.h
            # Storage 系统头文件
            render/storage_rd/decal_storage.h
            render/storage_rd/mesh_storage.h
            render/storage_rd/material_storage.h
            render/storage_rd/texture_storage.h
            render/storage_rd/light_storage.h
            render/storage_rd/particles_storage.h
            render/storage_rd/utilities.h
            render/storage_rd/mesh_storage_impl.h
            render/storage_rd/material_storage_impl.h
            render/storage_rd/texture_storage_impl.h
            render/storage_rd/light_storage_impl.h
            # RenderingServer + Compositor Effect 头文件
            render/compositor_effect.h
            render/rendering_server.h
            render/rendering_server_default.h
            render/opengl/gl_backend.h
            render/opengl/gl_buffer.h
            render/opengl/gl_shader.h
            render/opengl/gl_texture.h
            render/opengl/gl_framebuffer.h
            render/opengl/gl_utils.h
            render/opengl/gl_imgui_backend.h
            render/opengl/imgui_renderer.h
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_backend.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_instance.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_device.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_swapchain.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_buffer.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_mesh.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_shader.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_texture.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_framebuffer.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_imgui_backend.h>
            $<$<BOOL:${GRYCE_HAS_VULKAN}>:render/vulkan/vk_renderer2d.h>
            # 渲染系统头（供编辑器与上层消费）
            ecs/systems/render_system_2d.h
            ecs/systems/render_system_3d.h
)

target_include_directories(GryceRenderer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/stb>
)

target_link_libraries(GryceRenderer PUBLIC GryceCore GrycePlatform gryce::compile_options)

# ImGui 源文件
if(GRYCE_HAS_IMGUI)
    target_sources(GryceRenderer
        PRIVATE
            ${IMGUI_DIR}/imgui.cpp
            ${IMGUI_DIR}/imgui_demo.cpp
            ${IMGUI_DIR}/imgui_draw.cpp
            ${IMGUI_DIR}/imgui_tables.cpp
            ${IMGUI_DIR}/imgui_widgets.cpp
            ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
            ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(GryceRenderer PUBLIC ${IMGUI_DIR})

    # ImGui 符号导出/导入：编译 GryceRenderer 时导出，消费时导入
    if(WIN32)
        target_compile_definitions(GryceRenderer PRIVATE IMGUI_API=__declspec\(dllexport\))
        target_compile_definitions(GryceRenderer INTERFACE IMGUI_API=__declspec\(dllimport\))
    endif()

    set(_imgui_sources
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    if(GRYCE_HAS_VULKAN)
        list(APPEND _imgui_sources ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
    endif()
    set_source_files_properties(${_imgui_sources} PROPERTIES COMPILE_FLAGS -w)
endif()

# 抑制第三方单文件实现的编译警告
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/stb_image_impl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/tinyexr_impl.cpp
    ${CMAKE_SOURCE_DIR}/third_party/tinyexr/miniz.c
    PROPERTIES COMPILE_FLAGS -w
)

# 新增组件后反射注册表符号较多，MSVC 需要 /bigobj（GCC/Clang 忽略该 genex）
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/reflection/builtin_reflections.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/components/component_factory.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>"
)

# ImGui OpenGL3 backend 使用项目已有的 GLEW 加载器
target_compile_definitions(GryceRenderer PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)

# 链接 Vulkan
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Vulkan SDK found: ${Vulkan_INCLUDE_DIRS}")
    set(GRYCE_HAS_VULKAN TRUE)
    target_include_directories(GryceRenderer PUBLIC ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(GryceRenderer PUBLIC GRYCE_HAS_VULKAN)

    if(MINGW AND EXISTS "${Vulkan_LIBRARY}")
        get_filename_component(_vulkan_lib_ext "${Vulkan_LIBRARY}" EXT)
        if(_vulkan_lib_ext STREQUAL ".lib")
            find_file(GRYCE_VULKAN_DLL vulkan-1.dll PATHS C:/Windows/System32)
            if(GRYCE_VULKAN_DLL AND EXISTS "${GRYCE_VULKAN_DLL}")
                set(GRYCE_VULKAN_IMPLIB "${CMAKE_BINARY_DIR}/libvulkan-1.dll.a")
                set(GRYCE_VULKAN_DEF "${CMAKE_BINARY_DIR}/vulkan-1.def")
                if(NOT EXISTS ${GRYCE_VULKAN_IMPLIB})
                    message(STATUS "Generating MinGW import library for ${GRYCE_VULKAN_DLL}")
                    find_program(GENDEF_EXECUTABLE gendef REQUIRED)
                    find_program(DLLTOOL_EXECUTABLE dlltool REQUIRED)
                    get_filename_component(_vulkan_dll_name "${GRYCE_VULKAN_DLL}" NAME_WE)
                    execute_process(
                        COMMAND ${GENDEF_EXECUTABLE} "${GRYCE_VULKAN_DLL}"
                        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                        RESULT_VARIABLE _gendef_result
                    )
                    if(_gendef_result EQUAL 0 AND EXISTS "${CMAKE_BINARY_DIR}/${_vulkan_dll_name}.def")
                        file(RENAME "${CMAKE_BINARY_DIR}/${_vulkan_dll_name}.def" ${GRYCE_VULKAN_DEF})
                        execute_process(
                            COMMAND ${DLLTOOL_EXECUTABLE} -d ${GRYCE_VULKAN_DEF} -D vulkan-1.dll -l ${GRYCE_VULKAN_IMPLIB}
                            RESULT_VARIABLE _dlltool_result
                        )
                    endif()
                    if(NOT EXISTS ${GRYCE_VULKAN_IMPLIB})
                        message(WARNING "Failed to generate Vulkan import library; Vulkan disabled")
                        set(GRYCE_HAS_VULKAN FALSE)
                    endif()
                endif()
                if(GRYCE_HAS_VULKAN)
                    target_link_libraries(GryceRenderer PUBLIC ${GRYCE_VULKAN_IMPLIB})
                    message(STATUS "Vulkan MinGW import library: ${GRYCE_VULKAN_IMPLIB}")
                endif()
            else()
                message(WARNING "Vulkan SDK is MSVC .lib but vulkan-1.dll not found; Vulkan disabled")
                set(GRYCE_HAS_VULKAN FALSE)
            endif()
        else()
            target_link_libraries(GryceRenderer PUBLIC ${Vulkan_LIBRARY})
        endif()
    else()
        target_link_libraries(GryceRenderer PUBLIC Vulkan::Vulkan)
    endif()
else()
    message(STATUS "Vulkan not found, Vulkan backend disabled")
    set(GRYCE_HAS_VULKAN FALSE)
endif()

# Vulkan 源文件
if(GRYCE_HAS_VULKAN)
    target_sources(GryceRenderer
        PRIVATE
            render/vulkan/vk_backend.cpp
            render/vulkan/vk_instance.cpp
            render/vulkan/vk_device.cpp
            render/vulkan/vk_swapchain.cpp
            render/vulkan/vk_buffer.cpp
            render/vulkan/vk_mesh.cpp
            render/vulkan/vk_shader.cpp
            render/vulkan/vk_texture.cpp
            render/vulkan/vk_framebuffer.cpp
            render/vulkan/vk_imgui_backend.cpp
            render/vulkan/vk_renderer2d.cpp
            render/vulkan/vk_glsl_compiler.cpp
            render/vulkan/vk_vma.cpp
            $<$<BOOL:${GRYCE_HAS_IMGUI}>:${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp>
    )
endif()

# 链接 GLFW + GLEW
if(GRYCE_HAS_GLFW)
    target_link_libraries(GryceRenderer PUBLIC glfw)
endif()
if(GRYCE_HAS_GLEW)
    if(TARGET GLEW::glew)
        target_link_libraries(GryceRenderer PUBLIC GLEW::glew)
    elseif(TARGET glew_s)
        target_link_libraries(GryceRenderer PUBLIC glew_s)
        target_compile_definitions(GryceRenderer PUBLIC GLEW_STATIC)
    elseif(TARGET glew)
        target_link_libraries(GryceRenderer PUBLIC glew)
    else()
        message(FATAL_ERROR "GLEW found but no known target present")
    endif()
else()
    message(FATAL_ERROR "GLEW is required. Run: python tools/deps_manager.py download")
endif()

# OpenGL-only games must not require the Vulkan loader on the target machine:
# delay-load vulkan-1.dll so the import is only resolved when the Vulkan
# backend is actually used. (MSVC only; MinGW packages keep the hard import.)
if(MSVC AND GRYCE_HAS_VULKAN)
    target_link_options(GryceRenderer PRIVATE /DELAYLOAD:vulkan-1.dll)
    target_link_libraries(GryceRenderer PRIVATE delayimp)
endif()

# ============================================================================
# GrycePlatform.dll — Window、Input、Cursor
# ============================================================================
add_library(GrycePlatform SHARED)

set_target_properties(GrycePlatform PROPERTIES
    OUTPUT_NAME GrycePlatform
    RUNTIME_OUTPUT_NAME GrycePlatform
    PREFIX ""
    EXPORT_NAME GrycePlatform
    FOLDER "Core"
)

target_compile_definitions(GrycePlatform PRIVATE GRYCE_PLATFORM_BUILDING)

target_sources(GrycePlatform
    PRIVATE
        api/platform_api.cpp
        platform/window.cpp
        platform/input.cpp
        platform/cursor.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            GrycePlatform/window_api.h
            GrycePlatform/input_api.h
            platform/window.h
            platform/input.h
            platform/cursor.h
)

target_include_directories(GrycePlatform
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
)

target_link_libraries(GrycePlatform PUBLIC GryceCore gryce::compile_options)

if(GRYCE_HAS_GLFW)
    target_link_libraries(GrycePlatform PUBLIC glfw)
endif()

# ============================================================================
# GrycePhysics.dll — Jolt 3D / Box2D 2D
# ============================================================================
add_library(GrycePhysics SHARED)

set_target_properties(GrycePhysics PROPERTIES
    OUTPUT_NAME GrycePhysics
    RUNTIME_OUTPUT_NAME GrycePhysics
    PREFIX ""
    EXPORT_NAME GrycePhysics
    FOLDER "Core"
)

target_compile_definitions(GrycePhysics PRIVATE GRYCE_PHYSICS_BUILDING)

target_sources(GrycePhysics
    PRIVATE
        api/physics_api.cpp
        physics/physics_factory.cpp
        physics/box2d_world_2d.cpp
        physics/jolt_physics_world_3d.cpp
        ecs/systems/physics_system_3d.cpp
        ecs/systems/physics_system_2d.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            GrycePhysics/physics_api.h
            physics/physics_point.h
            physics/physics_types.h
            physics/physics_world_2d.h
            physics/physics_world_3d.h
            physics/physics_factory.h
            physics/box2d_world_2d.h
            physics/jolt_physics_world_3d.h
            # 物理系统头随实现（physics_system_*.cpp）归位到 GrycePhysics
            ecs/systems/physics_system_3d.h
            ecs/systems/physics_system_2d.h
)

target_include_directories(GrycePhysics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
)

target_link_libraries(GrycePhysics PUBLIC GryceCore gryce::compile_options)

if(GRYCE_HAS_BOX2D)
    target_link_libraries(GrycePhysics PUBLIC box2d)
    target_compile_definitions(GrycePhysics PUBLIC GRYCE_HAS_BOX2D)
endif()

if(GRYCE_HAS_JOLT)
    target_link_libraries(GrycePhysics PUBLIC Jolt)
    target_compile_definitions(GrycePhysics PUBLIC GRYCE_HAS_JOLT)
endif()

# ============================================================================
# GryceAudio — miniaudio 音频引擎模块（独立 target）
# ============================================================================
# 说明：音频引擎（audio_engine / miniaudio / stb_vorbis / time_stretcher）从
# GryceCore 的扁平源码列表中独立成自己的编译单元，作为静态模块被 GryceCore 依赖。
#
# 为何是静态模块而非独立 DLL？
# 1) render_pipeline.cpp 与 audio_source 组件（均编译在 GryceCore 内）直接调用
#    AudioEngine；若拆成独立 DLL，Core → GryceAudio 与 GryceAudio → Core(math)
#    会形成互导 DLL 循环。
# 2) 彻底解耦需把 audio_source 组件与源码间的多次调用改成服务门面（C API +
#    运行期注册或延迟加载），属于后续优先事项。
# 采用静态模块：音频源码独立成 target、头归 GryceAudio，最终链接进 Core.dll。
# 音频引擎只依赖 header-only 的 math，不反向引用 Core，链路单向、无循环。
add_library(GryceAudio STATIC
    audio/audio_engine.cpp
    audio/time_stretcher.cpp
    audio/miniaudio_impl.cpp
    audio/stb_vorbis_impl.cpp
)

# GryceAudio 编译进 GryceCore.dll，故必须带 GRYCE_CORE_BUILDING 使其 GRYCE_API
# 符号 dllexport 导出，否则 GryceRenderer（render_pipeline.cpp）跨 DLL 调用
# AudioEngine::instance()/set_listener_position() 会链接失败。
target_compile_definitions(GryceAudio PRIVATE GRYCE_CORE_BUILDING)

set_target_properties(GryceAudio PROPERTIES
    FOLDER "Core"
)

target_sources(GryceAudio PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES
        audio/audio_engine.h
)

target_include_directories(GryceAudio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
)

target_link_libraries(GryceAudio PUBLIC gryce::compile_options)



