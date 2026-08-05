#pragma once

#include <functional>
#include <GLFW/glfw3.h>

namespace gryce_engine::editor {

// 初始化 Fluent Design 窗口（DWM Mica + 圆角 + 自定义标题栏）
void FluentWindow_Init(GLFWwindow* window);

// 反初始化
void FluentWindow_Shutdown();

// 获取标题栏高度（DPI 缩放后）
float FluentWindow_GetTitleBarHeight();

// 检查窗口是否最大化
bool FluentWindow_IsMaximized(GLFWwindow* window);

// 设置菜单栏钩子
void FluentWindow_SetMenuBarHook(std::function<void()> hook);

// 渲染标题栏（Logo + 菜单 + 系统按钮）
// 在 imgui.begin_frame() 之后调用，不创建 ImGui 窗口
void FluentWindow_RenderTitleBar(GLFWwindow* window);

} // namespace gryce_engine::editor
