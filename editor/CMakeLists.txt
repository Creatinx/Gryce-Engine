# ===========================================================================
# editor/CMakeLists.txt — 编辑器：一处编排，两块物理隔离的产物
#
# editor/project/  完整项目（纯 JS + 场景 + 资产），由宿主 GryceEditor.exe 启动。
# editor/ext/      原生功能拓展，编成 extension.dll 放进 project/，宿主自动加载。
#
# 为什么要有 editor_main.cpp 这个 exe：编辑器此前只能靠通用宿主带参数启动
# （GryceApp --project editor/project），每次都要记住那串参数，IDE 运行配置里
# 也得手填一遍；漏填时宿主退回当前工作目录解析 res:/，报的是"场景加载失败"
# 这种指向错误位置的消息。烧进项目根后启动就是 build/GryceEditor.exe。
# 宿主逻辑不复制一份，仍走 gryce_engine::app::run —— 编辑器与 demo 一样是
# "宿主 + 项目"，两者的窗口/场景/退出行为必须完全一致。
#
# 与顶层 GryceApp 的关系：GryceApp 是通用模板入口（任意项目都能用），
# GryceEditor 是这一个项目的专用入口。两者并存，不互相替代。
# ===========================================================================

# 项目根烧进二进制。只在配置时求值，改路径要重新 configure。
get_filename_component(GRYCE_EDITOR_PROJECT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/project" ABSOLUTE)

# ---------------------------------------------------------------------------
# 功能拓展：extension.dll。先编它 —— 下面 GryceEditor 的 add_dependencies
# 要求被依赖的目标已定义。
# ---------------------------------------------------------------------------
add_subdirectory(ext)

# ---------------------------------------------------------------------------
# 编辑器宿主：extension.dll 由上面的 ext/ 子目录编出。
# 依赖它而不是只依赖三个引擎 DLL：单独
# `cmake --build build --target GryceEditor` 时若不带上扩展，产物能启动但界面
# 完全没有（扩展缺失时宿主按设计降级成场景浏览器、不报错），这是个很难自查的
# 静默失败。声明式依赖比在文档里写"记得先编扩展"可靠。
# ---------------------------------------------------------------------------
add_executable(GryceEditor editor_main.cpp)
target_link_libraries(GryceEditor PRIVATE GryceAppHost)
target_compile_definitions(GryceEditor PRIVATE
    GRYCE_EDITOR_PROJECT_ROOT="${GRYCE_EDITOR_PROJECT_ROOT}"
)
add_dependencies(GryceEditor GryceEditorExtension)

set_target_properties(GryceEditor PROPERTIES
    # 与其它 exe 一起落在 build 根：引擎 DLL 的 POST_BUILD 同步只拷到
    # ${GRYCE_EXE_OUTPUT_DIR}（= build 根），exe 落在别处就找不到 DLL。
    RUNTIME_OUTPUT_DIRECTORY "${GRYCE_EXE_OUTPUT_DIR}"
)

# 第三方运行时（glfw3 / box2d / Jolt / GLEW / MinGW 运行时）的同步由顶层
# GryceSyncRuntimeDlls 负责，它拷到 $<TARGET_FILE_DIR:GryceRenderSmoke>，
# 与本 exe 同目录，所以这里不需要重复一遍。