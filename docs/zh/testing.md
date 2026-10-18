# 测试

本文描述 CTest 门禁的构成、用例清单与 CI 配置。

## 1. 门禁地位

CI 的通过条件只有一条：`ctest` 里的用例全部通过。任一用例失败，`ctest` 返回非零，job 即判定失败（CI 里没有 `continue-on-error`，也不会跳过用例）。配置阶段使用 `--no-tests=error`：万一 CMake 改动导致一条用例都没注册，ctest 会以错误退出，而不是"零用例零失败"地假绿。

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 2. 用例清单

当前共 17 条用例，分两个宿主。

### 2.1 数学与资源规则（`GryceMathTests.exe`，7 条）

被测源码（`engine/math/*.cpp`、`engine/assets/material_rules.cpp`、`engine/assets/mesh_lod.cpp`）直接编译进测试可执行文件，**不链接任何引擎 DLL**，与 GPU、驱动、窗口 / 无头环境完全解耦，CI 上毫秒级出结果。全部为纯计算结果的数值核对，确定性，同一份代码在任何机器上结果一致。

| 用例 | 核对内容 |
| --- | --- |
| `math_vector` | `Vector2/3/4` 的加减乘除、点积叉积、长度与归一化，以及右手坐标系约定（X×Y=Z） |
| `math_matrix` | `Matrix4` 的 translate / scale / rotate / multiply / transpose / inverse，以及 perspective、ortho、look_at、billboard 的投影结果 |
| `math_quaternion` | 轴角构造、共轭与逆、乘法、slerp、`rotate_vector`，以及 `to_matrix` / `from_rotation_matrix` / 欧拉角往返的一致性 |
| `math_camera` | 默认朝向与右手正交基、`look_at` 反解 yaw/pitch、WASD 前后左右移动、pitch 限幅、视图与投影矩阵 |
| `math_ray` | `Ray::at` 求值、slab 法 AABB 求交（命中 / 未命中 / 内部起点）与命中距离 |
| `math_material` | 模型文件材质到 PBR 参数的推导规则（`assets/material_rules.cpp`，见 [model-import.md](model-import.md)） |
| `math_lod` | 自动 LOD 链的结构性保证（`assets/mesh_lod.cpp`，见 [lod.md](lod.md)） |

### 2.2 工具链（`GryceGCTests.exe`，10 条）

| 用例 | 核对内容 |
| --- | --- |
| `grycegc_cli` | 命令行解析与命令分发 |
| `grycegc_crc32` | CRC32 实现 |
| `grycegc_pe` | PE trailer / VERSIONINFO 读写 |
| `grycegc_aes` | AES-256-GCM 加解密 |
| `grycegc_smoke` | 端到端冒烟（临时工程上跑通主要命令） |
| `grycegc_crypto` | 加解密原语与容器格式 |
| `grycegc_pak` | `.gpack` / `.pak` 资源包 create / list / extract / verify |
| `grycegc_meta` | `gryce.meta.json` / `project.data` 元数据读写 |
| `grycegc_package` | 发布目录打包与 `verify` 自洽性 |
| `grycegc_webpack` | 单文件 HTML 打包输出结构 |

## 3. 执行注意

`grycegc_*` 用例并行执行会互相干扰（共享临时目录与进程级状态），本地全量验证请串行跑：

```bash
ctest --test-dir build -j1 --output-on-failure
```

CI 上各用例独立注册、由 runner 调度，不受此限。

## 4. 新增用例

- 数学 / 资源规则套件：在根 `CMakeLists.txt` 的 `GRYCE_MATH_TEST_SUITES` 里加名字，自动注册成一条 CTest。
- 工具链套件：在 `toolchain/CMakeLists.txt` 的 `_grycegc_suites` 里加名字。

## 5. 历史：像素比对用例已删除

早期的 `renderer_smoke_opengl` / `renderer_smoke_vulkan` 两条像素用例及其 Python 工具已删除：它们靠"截图里非背景像素占比"判定，受无头环境 / 软件渲染 / 驱动版本影响很大，既不反映构建正确性也不反映计算正确性。`GryceRenderSmoke.exe` 本身保留为人工运行的演示宿主；Web 端同理不进 CTest，只做人工验证（见 [webgl-backend.md](webgl-backend.md) 第 7 节）。

## 6. CI 配置

`.github/workflows/ci.yml`：任意 push / PR 触发，Windows / MSYS2 UCRT64，Release 配置，安装 GCC / CMake / Ninja / GLFW / GLEW / Box2D / Jolt 系统包（不装 Assimp 与 Vulkan SDK，对应路径自动降级），然后 configure + build + CTest。
