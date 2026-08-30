# GryceEditor — C++/ImGui Docking 编辑器（替代旧 WPF 编辑器）
#
# 纯 C++ 原生编辑器，链接引擎核心 DLL，使用 ImGui Docking 实现编辑器布局。
# 跨平台（Win/Linux），直接渲染到 OpenGL/Vulkan 上下文。

add_executable(GryceEditor
    src/main.cpp
    src/editor.cpp
    src/editor.h
    src/launcher.cpp
    src/hierarchy_panel.cpp
    src/hierarchy_panel.h
    src/inspector_panel.cpp
    src/inspector_panel.h
    src/file_explorer_panel.cpp
    src/file_explorer_panel.h
    src/scene_manager.cpp
    src/scene_manager.h
    src/command_stack.cpp
    src/command_stack.h
    src/play_mode_manager.cpp
    src/play_mode_manager.h
    src/console_panel.cpp
    src/console_panel.h
    src/blender_theme.h
    src/godot4_theme.h
    ${CMAKE_SOURCE_DIR}/third_party/imguizmo/ImGuizmo.cpp
)

target_link_libraries(GryceEditor PRIVATE
    GryceCore
    GryceRenderer
    GrycePlatform
    GrycePhysics
)

target_include_directories(GryceEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/third_party/imguizmo
)

if(WIN32)
    target_link_libraries(GryceEditor PRIVATE comdlg32 ole32 shell32)
endif()

# 编辑器着色器依赖（共用示例的 SPIR-V）
if(TARGET examples_common)
    add_dependencies(GryceEditor examples_common)
endif()

# MinGW 运行时 DLL 复制
if(WIN32 AND MINGW)
    target_link_options(GryceEditor PRIVATE
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
            add_custom_command(TARGET GryceEditor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_dll} $<TARGET_FILE_DIR:GryceEditor>
                COMMENT "Copying runtime DLL: ${_dll}"
            )
        endif()
    endforeach()
endif()

set_target_properties(GryceEditor PROPERTIES
    FOLDER "Editor"
)
