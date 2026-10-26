# ===========================================================================
# editor/ext/CMakeLists.txt — 编辑器功能拓展
#
# 编成 extension.dll 放进 editor/project/，宿主 GryceEditor 启动时按
# <project>/extension.dll 自动加载。
#
# 只链接引擎导入库，只 include engine/api/*.h 的 C ABI。不链 engine/ 的内部
# 目标、不 include 内部 C++ 头：否则扩展会被绑死在一次编译的 C++ ABI 上，
# 引擎一改就得重编，扩展的意义（可独立分发）就没了。
# ===========================================================================

set(GRYCE_EDITOR_EXT_SOURCES
    ext.cpp

    bridge/js_args.cpp
    bridge/js_register.cpp

    session/session.cpp

    command/commands.cpp

    view/viewport.cpp
    view/gizmo.cpp

    asset/asset_index.cpp
    panel/model.cpp
    panel/panel.cpp
    input/shortcuts.cpp

    ui/ui.cpp
    ui/draw.cpp
    ui/intent.cpp
)

add_library(GryceEditorExtension SHARED ${GRYCE_EDITOR_EXT_SOURCES})

target_include_directories(GryceEditorExtension PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}              # "session/session.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/..           # 保持与 editor/ 相对关系
    ${GRYCE_ENGINE_ROOT}/engine              # "api/edit_api.h"
)

# 三个 DLL 都要：编辑命令与场景在 GryceCore，视口与 Gizmo 在 GryceRenderer，
# 快捷键读键走 GrycePlatform。
# 根工程模式下这三个是根 CMakeLists 定义的真实目标；独立模式下是
# editor/CMakeLists.txt 造的 IMPORTED 目标（指向已构建引擎树的导入库）。
target_link_libraries(GryceEditorExtension PRIVATE
    GryceRenderer
    GryceCore
    GrycePlatform
)

set_target_properties(GryceEditorExtension PROPERTIES
    PREFIX ""
    OUTPUT_NAME "GryceEditorExtension"
    # 与其它引擎产物一起落在 build 根，找起来只有一个地方；
    # 子目录默认会落进 build/editor/ext/，和 GryceApp.exe 不在一层。
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# 构建完直接落到项目目录，省掉"编完忘了拷"这一步。
# 用 copy_if_different：文件没变时不动时间戳，宿主不会误判需要重载。
# GRYCE_EDITOR_PROJECT_ROOT 由上级目录算好（绝对路径），两种模式通用。
add_custom_command(TARGET GryceEditorExtension POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GryceEditorExtension>
        ${GRYCE_EDITOR_PROJECT_ROOT}/extension.dll
    COMMENT "editor/project/extension.dll <- GryceEditorExtension"
)
