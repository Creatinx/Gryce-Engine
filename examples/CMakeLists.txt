# examples/CMakeLists.txt
# Gryce Engine 示例游戏项目（每个子目录都是一个完整的 GryceGC-A 项目）

set(EXAMPLES_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common)

# ---------------------------------------------------------------------------
# 公共示例框架（减少重复样板代码）
# ---------------------------------------------------------------------------
add_library(examples_common STATIC)
target_sources(examples_common PRIVATE
    ${EXAMPLES_COMMON_DIR}/app_launcher.cpp
    ${EXAMPLES_COMMON_DIR}/ui/debug_panel.cpp
)
target_link_libraries(examples_common PUBLIC gryce_core)
target_include_directories(examples_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EXAMPLES_COMMON_DIR}
)
set_target_properties(examples_common PROPERTIES
    FOLDER "Examples/Common"
)

# ---------------------------------------------------------------------------
# 辅助函数：快速注册一个 demo 可执行文件
# ---------------------------------------------------------------------------
function(add_gryce_demo target_name subdir)
    add_executable(${target_name})
    target_sources(${target_name} PRIVATE
        ${subdir}/main.cpp
    )
    target_link_libraries(${target_name} PRIVATE examples_common)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EXAMPLES_COMMON_DIR}
    )
    set_target_properties(${target_name} PROPERTIES
        FOLDER "Examples"
    )

    _gryce_demo_post_build(${target_name})
endfunction()

# 辅助函数：处理旧 demo 的特殊源文件
function(add_gryce_demo_legacy target_name subdir source_file)
    add_executable(${target_name})
    target_sources(${target_name} PRIVATE
        ${subdir}/${source_file}
    )
    target_link_libraries(${target_name} PRIVATE examples_common)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EXAMPLES_COMMON_DIR}
    )
    set_target_properties(${target_name} PROPERTIES
        FOLDER "Examples"
    )

    _gryce_demo_post_build(${target_name})
endfunction()

function(_gryce_demo_post_build target_name)
    if(WIN32 AND MINGW)
        target_link_options(${target_name} PRIVATE
            -static-libgcc
            -static-libstdc++
        )

        get_filename_component(_compiler_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
        set(_runtime_dlls
            ${_compiler_dir}/glew32.dll
            ${_compiler_dir}/glfw3.dll
            ${_compiler_dir}/libwinpthread-1.dll
        )
        foreach(_dll IN LISTS _runtime_dlls)
            if(EXISTS ${_dll})
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_dll} $<TARGET_FILE_DIR:${target_name}>
                    COMMENT "Copying runtime DLL: ${_dll}"
                )
            endif()
        endforeach()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 现有综合演示（尽可能覆盖所有功能）
# ---------------------------------------------------------------------------
add_gryce_demo_legacy(3dtest 3dtest 3dtest.cpp)
add_gryce_demo_legacy(gt2dDemo gt2dDemo gt2dDemo.cpp)

# ---------------------------------------------------------------------------
# 新增独立功能 demos
# ---------------------------------------------------------------------------
add_gryce_demo(demo_sprite2d   demo_sprite2d)
add_gryce_demo(demo_shapes2d   demo_shapes2d)
add_gryce_demo(demo_lighting2d demo_lighting2d)
add_gryce_demo(demo_tilemap2d  demo_tilemap2d)
add_gryce_demo(demo_particles2d demo_particles2d)
add_gryce_demo(demo_physics2d  demo_physics2d)
add_gryce_demo(demo_character2d demo_character2d)
add_gryce_demo(demo_joints2d   demo_joints2d)

# 3D demos
add_gryce_demo(demo_physics3d       demo_physics3d)
add_gryce_demo(demo_character3d     demo_character3d)
add_gryce_demo(demo_joints3d        demo_joints3d)
add_gryce_demo(demo_fracture        demo_fracture)
add_gryce_demo(demo_lighting3d      demo_lighting3d)
add_gryce_demo(demo_audio3d         demo_audio3d)
add_gryce_demo(demo_scene_serializer demo_scene_serializer)
