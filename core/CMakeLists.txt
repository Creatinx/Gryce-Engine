# core/CMakeLists.txt
# Gryce Engine 模块化核心库 — 拆分为 GryceCore / GryceRenderer / GrycePlatform / GrycePhysics

# ============================================================================
# GryceCore.dll — ECS、Scene、Entity、Component、Math、UUID、Reflection、Assets
# ============================================================================
add_library(GryceCore SHARED)

set_target_properties(GryceCore PROPERTIES
    OUTPUT_NAME GryceCore
    RUNTIME_OUTPUT_NAME GryceCore
    EXPORT_NAME GryceCore
    FOLDER "Core"
)

target_compile_definitions(GryceCore PRIVATE GRYCE_CORE_BUILDING)

target_sources(GryceCore
    PRIVATE
        # C API 实现
        api/core_api.cpp
        api/entity_api.cpp
        api/component_api.cpp
        api/scene_api.cpp
        api/asset_api.cpp
        api/command_buffer.cpp
        api/entity_handle_map.cpp
        api/entity_api.cpp
        api/component_api.cpp
        api/scene_api.cpp
        api/asset_api.cpp
        # 内部模块
        math/math.cpp
        math/camera.cpp
        utils/glog/glog_lib.cpp
        utils/frame_limiter.cpp
        resources/project.cpp
        resources/resource_path.cpp
        resources/gpack_bundle.cpp
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
        audio/audio_engine.cpp
        audio/time_stretcher.cpp
        audio/miniaudio_impl.cpp
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
        ecs/world.cpp
        ecs/component_store.cpp
        ecs/systems/animator_system.cpp
        ecs/systems/fracture_system.cpp
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
        render/render_pipeline.cpp
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
            export.h
            components/component.h
            components/component_factory.h
            components/transform.h
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
            components/audio_source.h
            audio/audio_engine.h
            resources/tileset.h
            components/mesh_renderer.h
            components/skinned_mesh_renderer.h
            components/terrain.h
            resources/project.h
            resources/resource_path.h
            resources/gpack_bundle.h
            physics/physics_point.h
            physics/physics_types.h
            physics/physics_world_2d.h
            physics/physics_world_3d.h
            physics/physics_factory.h
            physics/box2d_world_2d.h
            physics/jolt_physics_world_3d.h
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
            ecs/query.h
            ecs/component_store.h
            ecs/systems/physics_system_3d.h
            ecs/systems/physics_system_2d.h
            ecs/systems/fracture_system.h
            ecs/systems/animator_system.h
            math/math.h
            math/camera.h
            utils/glog/glog_lib.h
            utils/frame_limiter.h
            reflection/reflection.h
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

# 不导出所有符号，避免 libstdc++ 等静态库符号污染
# 导出由各头文件中的 GRYCE_API / __declspec(dllexport) 控制

target_compile_definitions(GryceCore PUBLIC
    $<$<BOOL:${GRYCE_HAS_ASSIMP}>:GRYCE_HAS_ASSIMP>
    $<$<BOOL:${GRYCE_HAS_NLOHMANN_JSON}>:GRYCE_HAS_NLOHMANN_JSON>
)

# Windows: 链接 imm32（IME）和 winmm（高精度定时器）
if(WIN32)
    target_link_libraries(GryceCore PUBLIC imm32 winmm)
endif()

# 链接 nlohmann/json
if(GRYCE_HAS_NLOHMANN_JSON)
    target_link_libraries(GryceCore PUBLIC nlohmann_json::nlohmann_json)
endif()

# 链接 Assimp
if(GRYCE_HAS_ASSIMP)
    target_link_libraries(GryceCore PUBLIC assimp::assimp)
endif()

# ============================================================================
# GryceRenderer.dll — RenderContext、Pipeline、GL/Vulkan Backend、Mesh、Texture
# ============================================================================
add_library(GryceRenderer SHARED)

set_target_properties(GryceRenderer PROPERTIES
    OUTPUT_NAME GryceRenderer
    RUNTIME_OUTPUT_NAME GryceRenderer
    EXPORT_NAME GryceRenderer
    FOLDER "Core"
)

target_compile_definitions(GryceRenderer PRIVATE GRYCE_RENDERER_BUILDING)

target_sources(GryceRenderer
    PRIVATE
        api/render_api.cpp
        render/render.cpp
        render/renderer2d_impl.cpp
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
            render/render_pipeline.h
            render/render_command_buffer.h
            render/render_thread.h
            render/render_context.h
            render/render2d.h
            render/font_atlas.h
            render/renderer2d_impl.h
            render/imgui_backend.h
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
)

target_include_directories(GryceRenderer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/stb>
)

target_link_libraries(GryceRenderer PUBLIC GryceCore gryce::compile_options)

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

# ============================================================================
# GrycePlatform.dll — Window、Input、Cursor
# ============================================================================
add_library(GrycePlatform SHARED)

set_target_properties(GrycePlatform PROPERTIES
    OUTPUT_NAME GrycePlatform
    RUNTIME_OUTPUT_NAME GrycePlatform
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
# GryceAudio.dll — miniaudio（optional）
# ============================================================================
# TODO: 音频模块拆分（Phase 2+）
# add_library(GryceAudio SHARED ...)
