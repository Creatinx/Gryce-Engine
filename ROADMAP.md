# Gryce Engine 开发路线图

> 按当前进度整理，从原型到可发布版本。

## 当前已完成的基础

- OpenGL / Vulkan 双后端 + RHI 抽象
- 2D Batch 渲染 + TTF 文字
- 3D PBR + Shadow + HDR ToneMapping
- ECS + Scene + `.gesc` JSON 序列化
- 资源路径 `res:/`、AssetManager、OBJ/Assimp 加载
- ImGui 调试面板 + 模型加载面板
- 帧率限制 / GPU Busy Spin / 光标隐藏锁定

---

## P0 渲染器收尾

- [x] Vulkan 关闭时 `VkFramebuffer` 泄漏修复
- [x] OpenGL/Vulkan 干净退出验证
- [x] Release 构建验证一次
- [x] 关闭“启动自动全屏”问题（已默认 Windowed）
- [x] 鼠标/Space/Ctrl 默认映射最终确认（默认标准 FPS）

## P1 组件系统

- [x] `Node2D` / `Node3D` 节点层级（Transform 父子关系）
- [x] `Camera` 组件替代全局 camera
- [x] `Light` 组件（方向光/点光/聚光）
- [x] `StaticBody` / `RigidBody` / `Collider` 组件占位
- [x] `AudioSource` 组件占位
- [x] Component Inspector 在 DebugPanel 里可编辑基础属性
- [x] 场景保存时把组件数据写进 `.gesc`

## P2 物理系统

- [ ] 接入物理库（Jolt / PhysX / Bullet 三选一）
- [ ] 基础碰撞体：Box/Sphere/Capsule/Mesh
- [ ] RigidBody 质量、力、速度
- [ ] 射线检测 `Raycast`
- [ ] 物理步进与渲染帧解耦

## P3 资源管线

- [ ] Assimp 完整导入 glTF/FBX/DAE（材质、骨骼占位）
- [ ] Material 文件格式 `.gmat` + 序列化
- [ ] 纹理压缩/ mipmap 自动生成
- [ ] AssetBrowser ImGui 面板
- [ ] 热重载：模型/材质/着色器改动自动刷新

## P4 Editor

- [ ] 场景层级树（Scene Hierarchy）窗口
- [ ] Inspector 面板：组件属性实时编辑
- [ ] Viewport Gizmo（平移/旋转/缩放）
- [ ] 运行时/编辑态切换（Play/Pause/Stop）
- [ ] 多 viewport / camera preview（可选）

## P5 脚本系统

- [ ] 选择脚本方案：Lua / C# / 自定义 DSL
- [ ] Component 脚本基类 + 生命周期绑定
- [ ] 脚本热重载
- [ ] API 暴露：Input、Transform、Physics、Audio

## P6 音频系统

- [ ] 音频后端：OpenAL / miniaudio / WASAPI
- [ ] 2D/3D 音效播放
- [ ] 音频资源加载（wav/ogg/mp3）
- [ ] 混音、3D 衰减、循环

## P7 高级渲染

- [ ] IBL（环境光照 + 反射探针占位）
- [ ] SSAO / SSR（可选）
- [ ] 后处理栈：Bloom、FXAA、Color Grading
- [ ] GPU Instancing
- [ ] 视锥剔除 + LOD 占位

## P8 构建与发布

- [ ] 资源打包（pak / zip）
- [ ] 独立可执行文件打包脚本
- [ ] Windows 安装包
- [ ] Release/RelWithDebInfo 构建 CI

## P9 测试与文档

- [ ] 单元测试框架接入 + 核心模块测试
- [ ] 渲染测试场景（截图对比）
- [ ] 用户文档 / API 文档
- [ ] 示例项目
