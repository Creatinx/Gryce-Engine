// ===========================================================================
// engine/runtime/engine_context.cpp
// 引擎全局运行时状态相关的轻量定义 TU。
//
// 只定义 g_input_state（输入状态）：EngineContext 本体含 World / Scene 的
// unique_ptr，其析构需要完整类型，定义留在 api/core_api.cpp；脚本系统
// （engine/script）与单元测试只需要输入状态，链接本 TU 即可，不必拖入
// 整个 C API 层。EngineContext::input 以引用成员指向 g_input_state，
// 既有 g_core_state.input.xxx 访问路径不变（见 engine_context.h 注释）。
//
// 静态初始化顺序：EngineContext 的引用成员在构造时只取 g_input_state 的
// 地址（链接期确定），不读取其值，与跨 TU 初始化顺序无关。
// ===========================================================================

#include "runtime/engine_context.h"

namespace gryce_core {

InputState g_input_state;

} // namespace gryce_core
