# Gryce Engine — 资源路径与项目布局规范

## 1. `res:/` 虚拟资源根

`res:/` 是 Gryce Engine 的虚拟资源根路径。它**不是**指向引擎仓库根目录，而是指向"基于本引擎制作的游戏项目"的根目录。

### 解析规则

```
res:/scenes/main.gesc
    ↓ 解析为
<game_project_root>/scenes/main.gesc
```

- 在编辑器/运行器启动时，通过 `--project-root <path>` 或工作目录确定游戏项目根。
- 若未显式指定，默认以当前工作目录作为游戏项目根（向后兼容旧测试）。
- 引擎内部代码（核心、渲染、资源管理）只应处理 `res:/` 路径；不应硬编码引擎仓库路径。

### 与引擎仓库的关系

引擎仓库本身也可以包含一个默认示例项目，但那是示例，不是 `res:/` 的语义目标：

```
Gryce-Engine/                  ← 引擎仓库根（不是 res:/）
├── core/                      ← 引擎核心源码
├── editor/                    ← 编辑器
├── examples/                  ← 示例游戏项目集合
│   ├── 3dtest/                ← 一个完整的游戏项目
│   │   ├── scenes/main.gesc   ← res:/scenes/main.gesc
│   │   ├── models/
│   │   └── project.gryce      ← 项目配置文件
│   └── gt2dDemo/              ← 另一个完整的游戏项目
│       ├── scenes/main.gesc
│       ├── tilesets/
│       └── project.gryce
├── res/                       ← 引擎内置公共资源（可选）
└── docs/
```

运行示例时：

```bash
./gryce.exe --project-root examples/3dtest
```

## 2. GryceGC-A 项目标准

GryceGC（Gryce Game Compiler）是将来用于打包游戏资源的编译器/管线工具。本阶段不实现 GryceGC，但示例项目必须预先符合 **GryceGC-A** 布局标准，以便未来平滑迁移。

### 2.1 目录结构（GryceGC-A）

```
<game_project_root>/
├── project.gryce              # 项目元数据（名称、版本、入口场景）
├── scenes/                    # 场景文件（.gesc，JSON）
├── scripts/                   # 脚本资源（预留，未来 GryceGC 编译）
├── assets/
│   ├── models/                # 模型（.obj, .fbx, .gltf, .dae, .ply, .stl 等）
│   ├── textures/              # 贴图
│   ├── materials/             # 材质定义（未来 GryceGC 编译）
│   ├── audio/                 # 音效/音乐
│   ├── fonts/                 # 字体
│   ├── shaders/               # 着色器源（未来 GryzeGC 编译为 SPIR-V/DXBC/Metal IR）
│   └── tilesets/              # 2D 瓦片集
├── physics/                   # 物理材质、碰撞体预设（预留）
└── cache/                     # GryceGC 输出缓存（gitignore）
```

### 2.2 `project.gryce` 示例

```json
{
  "name": "3dtest",
  "version": "0.1.0",
  "engine_version": ">=0.1.0",
  "entry_scene": "res:/scenes/main.gesc",
  "physics": {
    "backend_3d": "jolt",
    "backend_2d": "box2d"
  },
  "window": {
    "width": 1280,
    "height": 720,
    "title": "Gryce Engine - 3D Test"
  }
}
```

### 2.3 设计原则

1. **自包含**：每个示例项目是一个完整目录，可在任意位置部署。
2. **只引用 `res:/`**：场景文件、脚本、材质内部路径全部使用 `res:/`。
3. **引擎与项目分离**：引擎二进制不依赖项目具体位置。
4. **向后兼容**：未提供 `--project-root` 时，默认以工作目录作为项目根。

## 3. 当前示例迁移计划

- 将 `tests/3dtest.cpp` 对应的游戏资产和入口迁移到 `examples/3dtest/`。
- 将 `tests/gt2dDemo.cpp` 对应的游戏资产和入口迁移到 `examples/gt2dDemo/`。
- `tests/` 目录保留纯单元测试（`gryce_tests`）。

## 4. 物理后端配置

GryceGC-A 允许在 `project.gryce` 中指定物理后端：

| 维度 | 后端选项 | 状态 |
|------|---------|------|
| 2D   | `box2d` | 默认，待集成 |
| 3D   | `jolt`  | 主后端，待集成 |
| 3D   | `physx` | NVIDIA PhysX，预留，后续实现 |

引擎内部将提供统一的 `IPhysicsWorld2D` / `IPhysicsWorld3D` 接口，底层由 Box2D / Jolt / PhysX 实现。
