# Gryce Engine 文档（中文）

本目录为 Gryce Engine 的项目文档。英文版入口见 [docs/README.md](../README.md) 与 [docs/en/](../en/README.md)。

文档按模块组织，覆盖架构、构建、渲染、资源导入、性能与工具链。历史设计规划（D3D12、WebGL 后端接入计划与 QuickJS 脚本引擎接入计划）归档在 [plans/](plans/)，仅保留中文原文。

| 模块 | 文档 | 内容 |
| --- | --- | --- |
| 总览 | [architecture.md](architecture.md) | DLL 分层、C API 边界、ECS 与反射、场景序列化、资源系统、运行期模型 |
| 构建 | [build.md](build.md) | 依赖发现与来源策略、构建选项、产物清单、DLL 同步、CI |
| 渲染 | [renderer.md](renderer.md) | 四后端、Clustered Forward 管线、阴影、屏幕空间与环境效果、调试工具 |
| 着色器 | [shader-conventions.md](shader-conventions.md) | 源码组织、后端变体、宏定义规则、ESSL300 转换约束 |
| 资源导入 | [model-import.md](model-import.md) | 贴图解析、PBR 材质推导、表面类型、色彩空间 |
| LOD | [lod.md](lod.md) | 误差驱动的自动 LOD 与空间分块 LOD |
| 性能 | [performance.md](performance.md) | 测量方法、耗时构成、已完成的优化 |
| WebGL | [webgl-backend.md](webgl-backend.md) | Web 构建、shim 架构、平台差异与排错 |
| 工具链 | [toolchain.md](toolchain.md) | GryceGC 命令面与发布流程 |
| 脚本 | [scripting.md](scripting.md) | QuickJS 脚本组件：挂载字段、生命周期钩子、基础 engine.* API、值约定、沙箱、热重载 |
| 应用入口 | [app-entry.md](app-entry.md) | `GryceApp` 宿主 + `main.js` 入口：启动顺序、钩子、窗口/场景/实体/应用 API、命令行 |
| 原生扩展 | [extension.md](extension.md) | `extension.cpp` 完整 ABI 参考：加载机制、虚表逐字段语义、值通道、编译分发、排错 |
| 编辑器接口 | [editor-api.md](editor-api.md) | 编辑命令面、撤销/重做、视口输出与 ImGui 桥、双视口、Gizmo、显示模式、资产侧车文件 |
| 测试 | [testing.md](testing.md) | CTest 门禁、用例清单、CI 配置 |
