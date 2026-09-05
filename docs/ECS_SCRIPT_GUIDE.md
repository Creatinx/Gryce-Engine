# ECS 脚本指南（ECS_SCRIPT_GUIDE）

> 实体脚本由 `ScriptComponent` 绑定 `.js` 文件，`ScriptSystem` 用 QuickJS 驱动
> 生命周期（`on_start` / `on_update` / `on_destroy`）。本文档描述 ES Module
> 结构、生命周期、props 双向同步、热重载与错误处理。

## 1. 脚本结构（ES Module）

脚本按 **ES Module** 编写，顶层 `export` 的函数/常量成为模块导出：

```js
// 暴露属性：Inspector 可编辑、随场景保存
export const props = {
    speed: 2.5,        // 数字 -> 浮点 prop（type 0）
    label: "player"    // 字符串 -> 字符串 prop（type 1）
};

export function on_start() {
    // 实体创建 / 组件挂载 / 播放开始时调用一次
}

export function on_update(dt) {
    // 每帧调用，dt 为帧时间（秒）
    if (engine.input.key_down(32)) {            // Space
        engine.scene.load("res:/scenes/level2.gesc");
    }
}

export function on_destroy() {
    // 组件移除 / 场景关闭 / 重载前调用
}
```

可选事件回调：

```js
export function _input(type, a, b, c) {
    // 输入事件分发（type: 事件类型，a/b/c: 事件参数）
}
```

要点：

- 生命周期函数均可选；模块未导出对应函数时静默跳过（不视为错误）。
- 顶层代码在加载时执行一次。
- 每个模块拥有独立作用域（`eval_module` 通过 IIFE + export 转换实现隔离），
  全局变量互不污染。
- 脚本内可直接使用 `engine.*`、`math.*`、`big.*`（见
  [SCRIPT_API_REFERENCE](./SCRIPT_API_REFERENCE.md)）。

## 2. 生命周期

| 回调 | 时机 |
|---|---|
| 顶层代码 | 模块加载时执行一次 |
| `on_start()` | 实体创建 / 组件挂载 / 播放开始时调用一次 |
| `on_update(dt)` | 每帧调用，`dt` 为帧时间（秒） |
| `on_destroy()` | 组件移除 / 场景关闭 / 重载前调用 |
| `_input(type, a, b, c)` | 输入事件分发（每帧按事件队列调用） |

调度顺序：按 `process_priority` 降序（默认 0，值越大 `on_update` 越先执行）。

## 3. props 双向同步

`ScriptComponent.props` 与模块导出的 `props` 对象保持双向同步：

```text
JS 模块导出 props 对象
      │  sync_props_from_env（加载后读取，写入组件）
      ▼
ScriptComponent.props（type 0 = float, type 1 = string）
      ▲  write_prop_to_env（Inspector 修改后写回）
      │
JS 模块的 props 对象（不存在时自动创建）
```

- **JS → C++**：模块加载成功后，`sync_props_from_env` 枚举导出 `props` 对象，
  数字字段写入 `type 0`（float），字符串字段写入 `type 1`（string）。
- **C++ → JS**：`ScriptSystem::set_prop(comp, name, value)` 修改组件 prop 并立即
  写回模块 `props` 对象（下一帧生效）。
- **序列化**：`props` 随场景保存（`.gesc`），`script_path` 同样序列化；
  模块命名空间为运行时状态，不序列化。

## 4. 热重载

保存 `.js` / 触发 `ReloadScripts` 时，`ScriptSystem::reload_all()` 执行：

1. 清空源码缓存。
2. 对每个已加载组件调用 `unload`：先 `on_destroy()`，再释放旧模块命名空间。
3. 下次 `on_update` 遍历时重新 `load`：加载新模块 → `on_start()` → 同步 props →
   **恢复旧 props 值**（热重载前 `load` 会先记住 `old_props`，重载后写回，
   Inspector 里改过的值不丢失）。

UI 与 JS 文件的热重载由 `src/ui/file_watcher.cpp`（轮询监听）+ `UIManager` 提供。

## 5. 错误处理与暂停

| 场景 | 行为 |
|---|---|
| 脚本路径无法解析 / 文件缺失 / 内容为空 | `last_error` 记录，脚本不加载 |
| 模块语法错误 | `last_error` 记录，脚本不加载 |
| `on_start` / `on_update` 抛异常 | `paused_on_error = true`，实体停止驱动直到重载；`last_error` 记录，Inspector 可查 |
| `on_destroy` 抛异常 | 仅记录错误，**不**暂停（避免阻塞卸载） |
| 全局暂停（`g_core_state.paused`） | `pause_mode = false` 的实体跳过 `on_update`；`pause_mode = true` 仍执行 |

`ScriptComponent` 运行时字段：

| 字段 | 说明 |
|---|---|
| `script_path` | 绑定的 `.js` 路径 |
| `props` | 暴露属性（序列化） |
| `script_loaded` / `start_called` | 加载 / 启动状态 |
| `last_error` | 最近一次错误信息（Inspector 可查） |
| `paused_on_error` | 出错暂停标记（直到重载） |
| `process_priority` | `on_update` 优先级（值越大越先） |
| `pause_mode` | 全局暂停时仍执行 `on_update` |

## 6. 发布模式（加密加载）

发布模式 `.js` 以加密字节码形式存放于 `.pak`：

```cpp
// 开发模式
ScriptVM::eval_module(明文源码, path, &ns);

// 发布模式（ResourceLoader 解密 + 字节码执行）
std::vector<uint8_t> bc = ResourceLoader::decrypt_js_bytecode(id);
ScriptVM::eval_bytecode(bc, path, &ns);
```

字节码与 `compile_script`（模块语义）互相兼容，须与打包器使用同一 QuickJS 版本。

## 7. 典型脚本示例

```js
// player.js
export const props = {
    speed: 3.0,
    tag: "player"
};

export function on_start() {
    engine.log.info("player spawned");
}

export function on_update(dt) {
    const x = engine.input.key_down(68) ? 1 : (engine.input.key_down(65) ? -1 : 0); // D/A
    const t = engine.entity.get_transform(engine.self());
    engine.entity.set_transform(engine.self(), {
        position: { x: t.position.x + x * props.speed * dt, y: t.position.y, z: 0 },
        rotation: t.rotation,
        scale: t.scale
    });
}
```

相关文档：[脚本 API 参考](./SCRIPT_API_REFERENCE.md)、[迁移指南](./MIGRATION_GUIDE.md)。
