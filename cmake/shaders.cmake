# cmake/shaders.cmake
# Gryce Engine Vulkan 着色器 SPIR-V 自动编译
#
# 调用 gryce_compile_vulkan_shaders(<shader_dir>) 会为目录下所有
# vulkan_*.vert / vulkan_*.frag 生成 spirv/<name>.<stage>.spv，
# 并创建一个名为 gryce_shaders_<dir_basename> 的 custom target。
# ---------------------------------------------------------------------------

find_program(GRYCE_GLSLANG_VALIDATOR glslangValidator
    PATHS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
)

if(NOT GRYCE_GLSLANG_VALIDATOR)
    message(WARNING
        "[Gryce Engine] glslangValidator not found. "
        "Vulkan SPIR-V shaders will not be auto-compiled. "
        "Install the Vulkan SDK or add glslangValidator to PATH."
    )
endif()

function(gryce_compile_vulkan_shaders shader_dir)
    if(NOT GRYCE_GLSLANG_VALIDATOR)
        return()
    endif()

    if(NOT IS_DIRECTORY "${shader_dir}")
        message(WARNING "[Gryce Engine] Shader directory does not exist: ${shader_dir}")
        return()
    endif()

    get_filename_component(dir_basename "${shader_dir}" NAME)
    set(spirv_dir "${shader_dir}/spirv")

    file(MAKE_DIRECTORY "${spirv_dir}")

    file(GLOB shader_sources
        "${shader_dir}/vulkan_*.vert"
        "${shader_dir}/vulkan_*.frag"
    )

    set(spirv_outputs)
    foreach(source IN LISTS shader_sources)
        get_filename_component(filename "${source}" NAME)
        set(output "${spirv_dir}/${filename}.spv")
        list(APPEND spirv_outputs "${output}")

        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${GRYCE_GLSLANG_VALIDATOR}" -V "${source}" -o "${output}"
            DEPENDS "${source}"
            COMMENT "Compiling SPIR-V: ${filename}"
            VERBATIM
        )
    endforeach()

    if(spirv_outputs)
        set(target_name "gryce_shaders_${dir_basename}")
        # 避免同名 target（例如 editor/project/shaders 与 examples/3dtest/shaders 不要冲突）
        set(counter 0)
        while(TARGET "${target_name}")
            math(EXPR counter "${counter} + 1")
            set(target_name "gryce_shaders_${dir_basename}_${counter}")
        endwhile()

        add_custom_target("${target_name}" ALL DEPENDS ${spirv_outputs})
        set_target_properties("${target_name}" PROPERTIES FOLDER "Shaders")

        # 把 target 名字返回给调用者，方便 add_dependencies
        set(GRYCE_SHADER_TARGET "${target_name}" PARENT_SCOPE)
    endif()
endfunction()
