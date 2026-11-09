# 接入其它项目（integration）

本文说明如何把 Gryce 引擎接进一个已有项目：需要哪些产物、目录怎么摆、初始化顺序是什么、CMake 怎么写、以及最容易踩的坑。接口逐函数说明见 [core-api.md](core-api.md)。

## 1. 交付物

构建完成后，`build/` 下只需要带走这些东西：

| 类别 | 文件 | 说明 |
| --- | --- | --- |
| 核心库 | `GryceCore.dll` | 场景、实体、组件、资产、脚本、物理、音频 |
| 渲染库 | `GryceRenderer.dll` | 渲染管线 + 基础调试 UI + 全部后端 |
| 平台库 | `GrycePlatform.dll` | 窗口、输入、光标 |
| 导入库 | `build/lib/libGryceCore.dll.a` 等三份 | **链接期**需要，运行期不需要 |
| 头文件 | `engine/` 下的 `api/*.h` 与 `export.h` | 只依赖 `api/` 即可，其余是引擎内部头 |
| 第三方运行库 | `glfw3.dll`、`glew32.dll`、`libbox2d.dll`、`libJolt.dll`（启用时还有 `libassimp-5.dll`） | 由构建自动同步到 exe 旁 |
| 编译器运行时 | `libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll` | 由构建自动同步 |

`build/` 根目录只放运行期产物（DLL 与 exe），导入库统一在 `build/lib/` —— 打包时"整目录拷贝"不会把 `.dll.a` 带进发布包。

## 2. 运行期目录布局

```text
MyGame/
  MyGame.exe
  GryceCore.dll
  GryceRenderer.dll
  GrycePlatform.dll
  glfw3.dll  glew32.dll  libbox2d.dll  libJolt.dll
  libstdc++-6.dll  libgcc_s_seh-1.dll  libwinpthread-1.dll
  project.data          <- 项目配置（name / window / render_api / main_scene ...）
  main.js               <- 可选：固定宿主 GryceApp 的入口脚本
  extension.dll         <- 可选：原生扩展
  shaders/              <- 引擎默认 shader 的兜底拷贝（见第 6 节）
  scenes/ models/ textures/ fonts/ scripts/
```

`res:/` 一律以 `project.data` 所在目录为根。用绝对路径也能跑，但换机器就断，正式项目统一写 `res:/`。

## 3. 三种接入方式

| 方式 | 适用 | 入口 | 你要写的代码 |
| --- | --- | --- | --- |
| A. 固定宿主 + 脚本 | 玩法逻辑可以用 JS 表达，不想碰 C++ 主循环 | `GryceApp.exe` + `main.js` | 只写脚本与场景（见 [app-entry.md](app-entry.md)） |
| B. 自带主循环 | 已有 C/C++ 应用，要把引擎当一个库用 | 自己的 `main` | 初始化 + 帧循环（本文第 4、5 节） |
| C. 纯逻辑（无窗口） | 服务端、批处理、CI 数值验证 | 自己的 `main` | 只初始化 core，不建窗口、不初始化渲染器 |

三种方式共用同一套 DLL 与头文件；A 只是"别人替你写好了主循环"。

## 4. 方式 B：自带主循环

### 4.1 初始化顺序（不可颠倒）

```text
1. GWindow_Create / GWindow_CreateEx      平台：建窗口（GL 后端需要 GL 上下文；Vulkan/DX12 用 NO_API）
2. GWindow_GetFramebufferSize             取真实像素尺寸（HiDPI 下与逻辑尺寸不同）
3. GCore_Init                             核心：项目根、反射表、World、场景
4. GRender_Init                            渲染：后端选择、管线、资源
```

退出顺序严格相反：

```text
GCore_Shutdown  ->  GRender_Shutdown  ->  GWindow_Destroy
```

先拆 core 再拆渲染器：渲染器在关闭时还会读场景数据（释放 GPU 资源、统计），顺序反了就是访问已释放内存。

### 4.2 帧循环

```c
while (!GWindow_ShouldClose()) {
    GWindow_PollEvents();          // 平台事件 -> 输入状态
    GInput_SyncToCore();           // 输入状态 -> ECMD_INPUT_* 命令（不调则 engine.input.* 永远为空）
    GCore_BeginFrame(dt);          // 消费命令、推进 World（on_update / 动画 / 物理）
    GRender_BeginFrame();
    GRender_RenderWorld();
    GRender_RenderGizmo();         // 可选：编辑器式 Gizmo
    GRender_EndFrame();
    GCore_EndFrame();              // 派发延迟回调、提交撤销分组
}
```

`dt` 用 `steady_clock` 计算并夹到 `[0.0005, 0.1]`，避免断点或长卡顿后出现巨型 dt 把物理打飞。

### 4.3 最小 CMake

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyGame LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 引擎产物目录（拷贝自 build/）
set(GRYCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gryce")

add_executable(MyGame main.cpp)
target_include_directories(MyGame PRIVATE "${GRYCE_ROOT}/include")   # 即 engine/ 目录
target_link_directories(MyGame PRIVATE "${GRYCE_ROOT}/lib")
target_link_libraries(MyGame PRIVATE
    GryceCore GryceRenderer GrycePlatform
    glfw3 glew32 opengl32 gdi32)

# 运行期 DLL 跟着 exe 走（开发期最省事）
add_custom_command(TARGET MyGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GRYCE_ROOT}/bin" "$<TARGET_FILE_DIR:MyGame>")
```

`include` 目录就是引擎源码树里的 `engine/`（复制时改名为 `include/`），代码里写 `#include "api/core_api.h"`。

## 5. 方式 C：纯逻辑（无窗口）

```c
GCoreInitDesc desc = {0};
desc.version = (uint32_t)sizeof(GCoreInitDesc);
desc.project_root = "D:/MyGame";
desc.enable_reflection = true;
GCore_Init(&desc);
GCore_SetAutoLoadMainScene(false);
GScene_Load("res:/scenes/main.gesc");

for (int i = 0; i < 600; ++i) {
    GCore_BeginFrame(1.0f / 60.0f);
    /* 通过 GEdit_* / GScene_Pick* / 组件查询做逻辑验证 */
    GCore_EndFrame();
}
GCore_Shutdown();
```

这条路径不需要 `GryceRenderer.dll`，也不需要显卡 —— CI 里跑逻辑门禁就用它。

## 6. 资源：引擎要读哪些文件

| 资源 | 位置 | 缺失后果 |
| --- | --- | --- |
| 引擎 shader | `<exe>/shaders/`（兜底），或项目自己的 `shaders/` | 渲染器初始化失败（GLSL 编译不到） |
| 项目配置 | `<project>/project.data` | 使用全默认值并告警，`main_scene` 不存在时场景为空 |
| 场景 | `res:/scenes/*.gesc` | 场景载入失败，实体列表为空 |
| 字体 | `res:/fonts/*.ttf` | 文字显示为方块，不崩溃 |
| 模型 / 贴图 | `res:/models`、`res:/textures` | 对应实体不渲染 |

`project.data` 的键与优先级（`--scene` > 入口脚本 > `main_scene`）见 [app-entry.md](app-entry.md) 第 2.1 节；渲染相关键（HDR、色调映射、阴影、SSAO/SSR/SSIL、PCSS、MSAA、TAA）在 `GRender_Init` 时下发一次。

## 7. 编译环境要求

* Windows + MSYS2 UCRT64（`g++`），或 Clang 走同一套 MinGW 运行时；**不支持 MSVC**（D3D12 后端刻意走动态加载，不链 MSVC 导入库）。
* C++26（引擎内部使用；**调用方只需要 C**，C99 及以上即可）。
* 目标 ISA 为 x86-64-v3（AVX2/FMA/BMI/F16C）。要跑更老的机器需自行用 `-DGRYCE_ENABLE_X86_V3=OFF` 重新构建引擎。
* 可选：Vulkan SDK（Vulkan 后端）、Windows SDK 或 mingw vendor 头（D3D12 后端）、Assimp（非 OBJ 模型导入）。

## 8. 常见坑

| 症状 | 原因 | 处理 |
| --- | --- | --- |
| 启动报 `0xc0000135` / 找不到 DLL | exe 旁缺引擎或编译器运行时 DLL | 把 `build/` 根目录的 DLL 全部拷到 exe 同目录 |
| 画面全黑但基础调试 UI 正常 | 场景没载入，或 `main_scene` 指向不存在的文件 | 看日志里的 `load scene failed`；确认 `res:/` 根 |
| `engine.input.*` 永远读不到 | 没调 `GInput_SyncToCore()` | 每帧在 `GCore_BeginFrame` 之前调 |
| 退出一瞬间崩溃 | 先关渲染器再关 core | 顺序改为 core -> renderer -> window |
| 改了引擎代码但运行没变化 | exe 旁是旧 DLL | 重新构建后确认 `build/*.dll` 时间戳，或直接跑 `build/` 里的 exe |
| 撤销把"半个拖拽"算成一步 | 拖拽没有用 `GCore_BeginEdit` / `EndEdit` 包起来 | 拖拽开始/结束各调一次 |
| Vulkan 上 MSAA 不生效 | 窗口创建前未按要求传 `NO_API`，或深度格式不支持 | 用 `GWindow_CreateEx(..., GRYCE_RENDER_API_VULKAN)`；引擎已支持 `Depth32F` MSAA |
| `res:/` 解析到奇怪的位置 | `project.data` 不在 exe 同目录，且未显式指定项目根 | 显式传 `--project`（GryceApp）或在 `GCoreInitDesc.project_root` 给出绝对路径 |

## 9. 接入检查清单

- [ ] 三个 DLL + 第三方 DLL + 编译器运行时都在 exe 同目录
- [ ] 头文件只从 `engine/api/` 引入，没有 include 引擎内部头
- [ ] 初始化顺序 Platform -> Core -> Renderer，退出顺序相反
- [ ] 每帧调了 `GInput_SyncToCore()`（需要输入时）
- [ ] `project.data` 与 `res:/` 资源齐备，`shaders/` 有兜底
- [ ] 拖拽类编辑用 `GCore_BeginEdit` / `EndEdit` 分组
- [ ] 需要跑满帧率做基准时，关闭垂直同步（呈现走 IMMEDIATE 模式）
