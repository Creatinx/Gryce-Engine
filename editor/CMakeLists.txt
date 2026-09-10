# GryceEditor — C++/ImGui Docking 编辑器
#
# 基于 ImGui Docking 分支的纯 C++ 原生编辑器，直接渲染到引擎 OpenGL/Vulkan 上下文。
# 链接引擎核心 DLL，跨平台（Win/Linux）。源码由 f5fae02 提交恢复并适配当前 src/ 布局。

add_executable(GryceEditor
    src/main.cpp
    src/editor.cpp
    src/editor.h
    src/launcher.cpp
    src/launcher_shared.cpp
    src/splash_process.cpp
    src/splash_process.h
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
    src/editor_theme.h
    src/editor_settings.h
    src/editor_settings.cpp
    src/i18n.h
    src/i18n.cpp
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
    ${CMAKE_SOURCE_DIR}/src          # 引擎公开头（platform/*、render/*、scene/* 等）
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/imguizmo
)

if(WIN32)
    # IFileDialog (comdlg32/shell32) + COM 运行时 (ole32) + 资源管理器打开 (shell32)
    target_link_libraries(GryceEditor PRIVATE comdlg32 shell32 ole32)
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