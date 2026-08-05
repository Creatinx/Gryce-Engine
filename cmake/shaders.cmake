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

# 注意：此处只提示；实际在 gryce_compile_vulkan_shaders 内，当确有
# vulkan_*.vert/.frag 需要编译而 glslangValidator 缺失时才会 FATAL_ERROR。
if(NOT GRYCE_GLSLANG_VALIDATOR)
    message(STATUS
        "[Gryce Engine] glslangValidator not found. "
        "Vulkan SPIR-V shaders will not be auto-compiled. "
        "Install the Vulkan SDK or add glslangValidator to PATH."
    )
endif()

function(gryce_compile_vulkan_shaders shader_dir)
    if(NOT IS_DIRECTORY "${shader_dir}")
        message(WARNING "[Gryce Engine] Shader directory does not exist: ${shader_dir}")
        return()
    endif()

    get_filename_component(dir_basename "${shader_dir}" NAME)
    set(spirv_dir "${shader_dir}/spirv")

    file(MAKE_DIRECTORY "${spirv_dir}")

    # CONFIGURE_DEPENDS：.vert/.frag 增删后重新 configure，
    # 避免新增着色器不参与编译导致运行时缺 SPIR-V。
    file(GLOB shader_sources CONFIGURE_DEPENDS
        "${shader_dir}/vulkan_*.vert"
        "${shader_dir}/vulkan_*.frag"
    )

    # 有着色器源码却没有编译器时，产物缺失会导致运行时着色器加载失败，
    # 这里直接报错而非静默跳过，避免把问题留到运行时才暴露。
    if(NOT GRYCE_GLSLANG_VALIDATOR AND shader_sources)
        message(FATAL_ERROR
            "[Gryce Engine] glslangValidator not found, but shaders exist in "
            "${shader_dir}. Install the Vulkan SDK or add glslangValidator to PATH."
        )
    endif()

    if(NOT GRYCE_GLSLANG_VALIDATOR)
        return()
    endif()

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
