# core/CMakeLists.txt
# Gryce Engine 核心库（支持静态/动态）

option(GRYCE_BUILD_SHARED "Build GryceCore as a shared library (DLL)" OFF)

if(GRYCE_BUILD_SHARED)
    add_library(gryce_core SHARED)
    target_compile_definitions(gryce_core PRIVATE GRYCE_CORE_BUILDING)
    target_compile_definitions(gryce_core INTERFACE GRYCE_BUILD_SHARED)
else()
    add_library(gryce_core STATIC)
endif()

target_sources(gryce_core
    PRIVATE
        math/math.cpp
        math/camera.cpp
        utils/glog/glog_lib.cpp
        utils/frame_limiter.cpp
        platform/window.cpp
        platform/input.cpp
        platform/cursor.cpp
        resources/project.cpp
        resources/resource_path.cpp
        assets/asset_manager.cpp
        assets/async_loader.cpp
        assets/obj_loader.cpp
        assets/stb_image_impl.cpp
        $<$<BOOL:${GRYCE_HAS_ASSIMP}>:assets/assimp_importer.cpp>
        components/component_factory.cpp
        components/2d/component_2d.cpp
        components/2d/sprite_2d.cpp
        components/2d/tilemap.cpp
        components/2d/parallax_background.cpp
        components/2d/particle_emitter.cpp
        components/audio_source.cpp
        components/mesh_renderer.cpp
        components/physical_material.cpp
        audio/audio_engine.cpp
        audio/miniaudio_impl.cpp
        ecs/systems/physics_system_3d.cpp
        ecs/systems/physics_system_2d.cpp
        ecs/systems/fracture_system.cpp
        physics/physics_factory.cpp
        physics/box2d_world_2d.cpp
        physics/jolt_physics_world_3d.cpp
        scene/uuid.cpp
        scene/entity.cpp
        scene/scene.cpp
        scene/prefab.cpp
        scene/scene_serializer.cpp
        ecs/world.cpp
        ecs/component_store.cpp
        ecs/systems/render_system_2d.cpp
        ecs/systems/render_system_3d.cpp
        render/render.cpp
        render/mesh.cpp
        render/shader.cpp
        render/texture.cpp
        render/material.cpp
        render/framebuffer.cpp
        render/render_pipeline.cpp
        render/render_command_buffer.cpp
        render/render_thread.cpp
        render/render_context.cpp
        render/render2d.cpp
        render/font_atlas.cpp
        render/renderer2d_impl.cpp
        render/opengl/gl_backend.cpp
        render/opengl/gl_frame_pacing.cpp
        render/opengl/gl_buffer.cpp
        render/opengl/gl_shader.cpp
        render/opengl/gl_texture.cpp
        render/opengl/gl_framebuffer.cpp
        render/opengl/gl_imgui_backend.cpp
        render/opengl/imgui_renderer.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
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
            resources/project.h
            resources/resource_path.h
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
            ecs/systems/render_system_2d.h
            ecs/systems/render_system_3d.h
            math/math.h
            math/camera.h
            utils/glog/glog_lib.h
            utils/frame_limiter.h
            platform/window.h
            platform/input.h
            platform/cursor.h
            render/render.h
            render/mesh.h
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

# ImGui 源文件（本地 third_party/imgui 或 FetchContent）
if(GRYCE_HAS_IMGUI)
    target_sources(gryce_core
        PRIVATE
            ${IMGUI_DIR}/imgui.cpp
            ${IMGUI_DIR}/imgui_demo.cpp
            ${IMGUI_DIR}/imgui_draw.cpp
            ${IMGUI_DIR}/imgui_tables.cpp
            ${IMGUI_DIR}/imgui_widgets.cpp
            ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
            ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(gryce_core PUBLIC ${IMGUI_DIR})

    # 抑制第三方 ImGui 源码的编译警告
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

# 抑制 third_party 单文件实现的编译警告
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/miniaudio_impl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/stb_image_impl.cpp
    PROPERTIES COMPILE_FLAGS -w
)

target_include_directories(gryce_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/stb>
        $<INSTALL_INTERFACE:include>
)

# 引擎内部严格警告选项（不传播到第三方依赖）
target_link_libraries(gryce_core PUBLIC gryce::compile_options)

# 将依赖可用性暴露给 C++ 代码
target_compile_definitions(gryce_core PUBLIC
    $<$<BOOL:${GRYCE_HAS_ASSIMP}>:GRYCE_HAS_ASSIMP>
    $<$<BOOL:${GRYCE_HAS_IMGUI}>:GRYCE_HAS_IMGUI>
    $<$<BOOL:${GRYCE_HAS_NLOHMANN_JSON}>:GRYCE_HAS_NLOHMANN_JSON>
)

# ImGui OpenGL3 backend 使用项目已有的 GLEW 加载器，避免自带 loader 冲突
target_compile_definitions(gryce_core PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)

# 链接 Vulkan（可选）
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Vulkan SDK found: ${Vulkan_INCLUDE_DIRS}")
    set(GRYCE_HAS_VULKAN TRUE)
    target_include_directories(gryce_core PUBLIC ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(gryce_core PUBLIC GRYCE_HAS_VULKAN)

    # MinGW 无法直接链接 MSVC 的 vulkan-1.lib，需要从系统 vulkan-1.dll 生成 import lib
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
                    # gendef 默认在 WORKING_DIRECTORY 生成与 DLL 同名的 .def 文件
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
                    target_link_libraries(gryce_core PUBLIC ${GRYCE_VULKAN_IMPLIB})
                    message(STATUS "Vulkan MinGW import library: ${GRYCE_VULKAN_IMPLIB}")
                endif()
            else()
                message(WARNING "Vulkan SDK is MSVC .lib but vulkan-1.dll not found; Vulkan disabled")
                set(GRYCE_HAS_VULKAN FALSE)
            endif()
        else()
            target_link_libraries(gryce_core PUBLIC ${Vulkan_LIBRARY})
        endif()
    else()
        target_link_libraries(gryce_core PUBLIC Vulkan::Vulkan)
    endif()
else()
    message(STATUS "Vulkan not found, Vulkan backend disabled")
    set(GRYCE_HAS_VULKAN FALSE)
endif()

# Vulkan 源文件必须在 GRYCE_HAS_VULKAN 确定后添加
if(GRYCE_HAS_VULKAN)
    target_sources(gryce_core
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

# 链接 GLFW + GLEW（窗口系统 + OpenGL 加载器）
if(GRYCE_HAS_GLFW)
    target_link_libraries(gryce_core PUBLIC glfw)
endif()
find_package(GLEW QUIET)
if(GLEW_FOUND)
    target_link_libraries(gryce_core PUBLIC GLEW::glew)
    message(STATUS "GLEW found")
else()
    message(STATUS "GLEW not found, trying fallback link")
    target_link_libraries(gryce_core PUBLIC glew32)
endif()

# 链接 nlohmann/json（场景序列化）
if(GRYCE_HAS_NLOHMANN_JSON)
    target_link_libraries(gryce_core PUBLIC nlohmann_json::nlohmann_json)
endif()

# 链接 Assimp（模型导入）
if(GRYCE_HAS_ASSIMP)
    target_link_libraries(gryce_core PUBLIC assimp::assimp)
endif()

# 链接 Box2D（2D 物理）
if(GRYCE_HAS_BOX2D)
    target_link_libraries(gryce_core PUBLIC box2d)
    target_compile_definitions(gryce_core PUBLIC GRYCE_HAS_BOX2D)
endif()

# 链接 JoltPhysics（3D 物理主后端）
if(GRYCE_HAS_JOLT)
    target_link_libraries(gryce_core PUBLIC Jolt)
    target_compile_definitions(gryce_core PUBLIC GRYCE_HAS_JOLT)
endif()

# Windows: 链接 imm32（IME 输入法管理）和 winmm（高精度定时器）
if(WIN32)
    target_link_libraries(gryce_core PUBLIC imm32 winmm)
endif()

set_target_properties(gryce_core PROPERTIES
    EXPORT_NAME GryceCore
    FOLDER "Core"
)
