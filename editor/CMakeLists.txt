# GryceEngine.Editor — WPF (C#) 编辑器（dotnet 构建）。
#
# 挂载自根 CMake（GRYCE_BUILD_EDITOR=ON 且 Windows 时 add_subdirectory(editor)）。
# 设计约定：
#   - 不加入 ALL：普通 `cmake --build <dir>` 不会编译编辑器；
#   - 显式构建该目标时才编译：`cmake --build <dir> --target GryceEditor`；
#   - 构建 GryceEditor 会先构建依赖（GryceCore / GryceRenderer / GrycePlatform /
#     GrycePhysics），这些目标的 POST_BUILD 会把原生 DLL 自动复制到
#     editor/bin/<Config>/net48/（见 core/CMakeLists.txt 的 gryce_copy_dll_to_editor）。

find_program(GRYCE_DOTNET dotnet)

if(NOT GRYCE_DOTNET)
    # dotnet 缺失时提供一个会失败的目标，而不是静默跳过，方便定位问题。
    add_custom_target(GryceEditor
        COMMAND ${CMAKE_COMMAND} -E echo_error
                "GRYCE_BUILD_EDITOR=ON but dotnet not found in PATH; cannot build editor"
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "GryceEditor: dotnet missing"
        VERBATIM
    )
    message(WARNING "GRYCE_BUILD_EDITOR=ON but dotnet not found; '--target GryceEditor' will fail")
else()
    add_custom_target(GryceEditor
        COMMAND "${GRYCE_DOTNET}" build
                "${CMAKE_CURRENT_SOURCE_DIR}/GryceEngine.Editor.csproj"
                -c "$<CONFIG>" --nologo
        COMMENT "Building WPF Editor (dotnet) ..."
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM
    )
endif()

# 构建编辑器前先构建核心 DLL（含自动复制到编辑器输出目录）。
add_dependencies(GryceEditor GryceCore GryceRenderer GrycePlatform GrycePhysics)
