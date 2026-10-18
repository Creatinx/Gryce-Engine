# Gryce Engine 文档（中文）

本目录为 Gryce Engine 的项目文档。英文版入口见 [docs/README.md](../README.md) 与 [docs/en/](../en/README.md)。

文档按模块组织，覆盖架构、构建、渲染、资源导入、性能与工具链。历史设计规划（D3D12 与 WebGL 后端接入计划）归档在 [plans/](plans/)，仅保留中文原文。

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
| 测试 | [testing.md](testing.md) | CTest 门禁、用例清单、CI 配置 |
