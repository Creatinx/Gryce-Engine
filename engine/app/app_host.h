// ===========================================================================
// engine/app/app_host.h
// 通用应用宿主：把"引擎怎么跑"固定下来，项目只提供 main.js。
//
// 与 test/renderer_smoke.cpp 的区别：那个是渲染冒烟台（几十个 A/B 开关、
// 压力场景、截图统计），项目逻辑在 main.cpp 里；这里是产品运行时——窗口、
// 场景、退出都由 main.js 通过 engine.* 调 API 决定，宿主只负责：
//   1. 解析 --project / --api 等启动参数；
//   2. GCore_Init + 进入 Play 模式（组件 on_update 生效）；
//   3. 装载 main.js（及可选的 extension.dll）；
//   4. 每帧：poll 事件 -> 同步输入 -> GCore_BeginFrame -> main.js on_update
//      -> 有窗口/渲染器时提交一帧 -> GCore_EndFrame。
//
// 因此每个项目目录里只有 main.js（+ 资源 + 可选 extension.cpp），不再需要
// 一份自己的 main.cpp —— 模板就是 engine/app/app_main.cpp。
// ===========================================================================

#pragma once

namespace gryce_engine::app {

// 进程入口实现。返回 0 正常退出，1 初始化/装载失败。
int run(int argc, char** argv);

} // namespace gryce_engine::app
