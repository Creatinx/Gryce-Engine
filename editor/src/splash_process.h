#pragma once

#include <memory>
#include <string>

namespace gryce_engine::editor {

// 启动画面子进程。
//
// 以子进程方式运行本可执行文件的 --splash 分支：子进程拥有独立的
// GLFW 窗口、GL 上下文和 ImGui 上下文，与主编辑器完全进程级隔离，
// 从根本上避免多线程共享 ImGui/GLFW 全局状态导致的冲突/卡死。
//
// 生命周期：
//   SplashProcess splash;        // 派生子进程，子进程随即显示加载窗口
//   ... 主线程做初始化重活（着色器编译等，不被抢占）...
//   splash.stop();               // 写 marker -> 等待子进程退出 -> 清理
//   window_->set_visible(true);  // init 完成后显示主窗口
class SplashProcess {
public:
    SplashProcess();
    ~SplashProcess();

    SplashProcess(const SplashProcess&) = delete;
    SplashProcess& operator=(const SplashProcess&) = delete;

    // 通知子进程退出并等待它结束，随后清理临时的 marker 文件。
    void stop();

private:
    std::string marker_path_;
    void* handle_ = nullptr; // 平台子进程句柄（Windows HANDLE / Linux pid）
};

// --splash 子进程入口：解析 marker 路径后显示加载窗口，直到 marker 出现才退出。
// 由 main.cpp 在接收到 --splash 参数时调用。
int run_splash_process(const char* marker_path);

} // namespace gryce_engine::editor