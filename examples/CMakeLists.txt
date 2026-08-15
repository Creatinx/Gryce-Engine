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
target_link_libraries(examples_common PUBLIC GryceCore GryceRenderer GrycePlatform GrycePhysics)
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
# Vulkan 着色器 SPIR-V 自动编译
# ---------------------------------------------------------------------------
set(GRYCE_SHADER_TARGETS)

# 着色器单一来源：examples/common/shaders（76 个 .vert/.frag）。
# 仓库约定：手动复制的文件必须在 CMake 里自动补齐，因此 configure 时
# 统一从 common/shaders 同步到每个 demo 的 shaders 目录，避免三处漂移。
set(GRYCE_SHARED_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common/shaders")
foreach(_demo_dir IN ITEMS 2dDemo 3dtest FPSDemo)
    set(_demo_shaders_dir "${CMAKE_CURRENT_SOURCE_DIR}/${_demo_dir}/shaders")
    if(IS_DIRECTORY "${GRYCE_SHARED_SHADERS_DIR}")
        file(COPY "${GRYCE_SHARED_SHADERS_DIR}/" DESTINATION "${_demo_shaders_dir}")
    endif()
endforeach()

gryce_compile_vulkan_shaders("${CMAKE_CURRENT_SOURCE_DIR}/3dtest/shaders")
if(GRYCE_SHADER_TARGET)
    list(APPEND GRYCE_SHADER_TARGETS "${GRYCE_SHADER_TARGET}")
endif()

gryce_compile_vulkan_shaders("${CMAKE_CURRENT_SOURCE_DIR}/2dDemo/shaders")
if(GRYCE_SHADER_TARGET)
    list(APPEND GRYCE_SHADER_TARGETS "${GRYCE_SHADER_TARGET}")
endif()

gryce_compile_vulkan_shaders("${CMAKE_CURRENT_SOURCE_DIR}/FPSDemo/shaders")
if(GRYCE_SHADER_TARGET)
    list(APPEND GRYCE_SHADER_TARGETS "${GRYCE_SHADER_TARGET}")
endif()

# 把着色器 target 暴露给父目录，editor 可依赖
set(GRYCE_EXAMPLE_SHADER_TARGETS "${GRYCE_SHADER_TARGETS}" PARENT_SCOPE)

# ---------------------------------------------------------------------------
# 现有综合演示（尽可能覆盖所有功能）
# ---------------------------------------------------------------------------
add_gryce_demo_legacy(3dtest 3dtest 3dtest.cpp)
add_gryce_demo_legacy(2dDemo 2dDemo 2dDemo.cpp)
add_gryce_demo_legacy(FPSDemo FPSDemo FPSDemo.cpp)

# 示例可执行文件依赖对应 SPIR-V
if(GRYCE_SHADER_TARGETS)
    add_dependencies(3dtest ${GRYCE_SHADER_TARGETS})
    add_dependencies(2dDemo ${GRYCE_SHADER_TARGETS})
    add_dependencies(FPSDemo ${GRYCE_SHADER_TARGETS})
endif()

