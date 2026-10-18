# Scripting (QuickJS)

This document describes the engine's JavaScript scripting capability: the `ScriptComponent` model, lifecycle, the `engine.*` API, sandbox constraints, hot reload, and tests. The corresponding plan is [`plans/quickjs-integration-plan.md`](plans/quickjs-integration-plan.md).

## 1. Components

| Part | Location | Notes |
| --- | --- | --- |
| QuickJS engine | `third_party/quickjs/` | Vendored MIT third-party static library (version 2026-06-04), `GryceQuickJS` target, unmodified |
| Runtime wrapper | `engine/script/js_runtime.{h,cpp}` | Process-level singleton: one shared `JSRuntime`, one `JSContext` per component; memory limit, frame interrupt budget, pending-job draining |
| Value conversion | `engine/script/js_value.h` | Single entry point for C++ <-> JS conversion (scalars / math types / nlohmann::json) |
| Native bindings | `engine/script/js_bindings.{h,cpp}` | The `engine.*` namespace (log / time / input / entity) |
| Script component | `engine/script/script_component.{h,cpp}` | `ScriptComponent`: loading, compilation, lifecycle dispatch, error isolation, hot reload |

The QuickJS CLI tools (`qjs.c` / `qjsc.c`) and `quickjs-libc.c` (the `std` / `os` modules) are not built; see section 6 for the sandbox policy.

## 2. Quick Start

The `ScriptCube` entity in `test/demo_project/scenes/main.gesc` carries `res:/scripts/demo_spin.js`:

```json
{
  "type": "Script",
  "enabled": true,
  "script": "res:/scripts/demo_spin.js",
  "props": { "speed": 0.8 }
}
```

```js
export function start() {
    engine.log.info('demo_spin: start, speed=' + this.props.speed);
}

export function update(dt) {
    const half = 0.5 * this.props.speed * dt;
    const s = Math.sin(half);
    const delta = [0.0, s, 0.0, Math.cos(half)]; // incremental Y-axis rotation
    engine.entity.set_rotation(quat_mul(delta, engine.entity.rotation()));
}
```

Script paths use the `res:/` prefix (relative to the project root, Pak archive first), and props are injected as `this.props`.

Command-line verification (`GryceRenderSmoke`) needs `--simulate` to enter Play mode and drive component `on_update`; without it the scene is initialized and rendered frame by frame, so `start` runs but `update` does not:

```bash
GryceRenderSmoke.exe --api opengl --project test/demo_project \
    --scene test/demo_project/scenes/main.gesc --frames 60 --simulate \
    --shot shot.png --no-debug-ui --no-fps
```

## 3. Component Model and Lifecycle

Every entity with a `ScriptComponent` owns an isolated JS global environment (its own `JSContext`, sharing the process-level `JSRuntime`). Module exports are the lifecycle hooks, with `this` bound to the component instance object:

| Hook | When | Component callback |
| --- | --- | --- |
| `awake()` | After the component attaches to the entity (fires during scene load) | `on_awake` |
| `start()` | When the scene starts, after all components initialize | `on_start` |
| `update(dt)` | Every frame, `dt` in seconds | `on_update` |
| `_input(type, a, b, c)` | Before `update` each frame; one call per input event | inside `on_update` |
| `destroy()` | Before the component is destroyed | `on_destroy` |

Event type constants for `_input`: `INPUT_EVENT_KEY_DOWN=1` / `KEY_UP=2` / `MOUSE_MOVE=3` / `MOUSE_DOWN=4` / `MOUSE_UP=5` (defined in `engine/runtime/engine_context.h`). Events are **not consumed** — every script component sees this frame's events; the host clears the queue at frame start.

Props (a serialized `nlohmann::json` object) are injected as `this.props` after compilation. Scene format:

```json
{ "type": "Script", "script": "res:/scripts/x.js", "props": { "speed": 2.5, "label": "demo" } }
```

Reflection registration covers the scalar fields `script_path` and `reload_poll_interval`; props is a nested structure and is not reflected (per the established reflection policy — editor data comes from the serialized fields themselves).

## 4. The `engine.*` API (v1)

| API | Notes |
| --- | --- |
| `engine.log.info/warn/error/debug(msg)` | Writes to glog (visible in the editor console) |
| `console.log/warn/error/debug(...args)` | Same channel (arguments joined by spaces), for porting web scripts directly |
| `engine.time.dt()` | This frame's dt (seconds) |
| `engine.time.frame()` | Component frame counter (first `update` is 1) |
| `engine.time.elapsed()` | Accumulated seconds since component start |
| `engine.input.key_down(code)` / `key_up(code)` | Key state (virtual key code) |
| `engine.input.mouse_x()` / `mouse_y()` | Absolute mouse position |
| `engine.input.mouse_delta()` | This frame's delta `[dx, dy]` (float, sub-pixel) |
| `engine.input.mouse_button(i)` | Mouse button (0/1/2) |
| `engine.input.mouse_locked()` | Pointer lock state |
| `engine.entity.self()` | Host entity handle (always 1 in v1) |
| `engine.entity.name()` / `set_name(s)` | Entity name |
| `engine.entity.position()` / `set_position([x,y,z])` | Local position |
| `engine.entity.rotation()` / `set_rotation([x,y,z,w])` | Quaternion rotation |
| `engine.entity.scale()` / `set_scale([x,y,z])` | Scale |
| `engine.entity.translate([dx,dy,dz])` | Additive translation |
| `engine.entity.set_enabled(bool)` | Entity switch |

Input state comes from `gryce_core::g_input_state` (the target of `EngineContext::input`): the host updates it via `ECMD_INPUT_*` commands and clears the event queue at frame start (`GCore_BeginFrame`). Without a command bus, input reads as zero.

`set_position` / `set_rotation` / `set_scale` / `translate` accept either an array or individual components.

Not in v1: cross-entity queries (`engine.find`) and `engine.physics` — both need a component-to-`Scene` back pointer, which `Entity` does not currently hold; deferred to a follow-up.

## 5. Authoring Conventions

- One component instance per `.js` file; module top-level code runs once at load (and again on hot reload).
- Vectors / quaternions are JS arrays: `[x,y,z]` / `[x,y,z,w]`; matrices are 16-element arrays in **column-major memory order** (`m[col*4+row]`, matching GLSL).
- Entity handles are ints (`EntityID` is uint64 and never enters JS, to avoid precision loss beyond 2^53).
- Promises work: pending jobs are drained after all `update` calls each frame (capped at 1024 per frame). There is no separate event loop and no `setTimeout`.

## 6. Sandbox and Limits

- **No filesystem / process / network access**: the `std` / `os` modules are not compiled and `require` does not exist; scripts reach the engine only through `engine.*` bindings.
- **Memory limit**: 256MB by default (`ScriptRuntime::set_memory_limit`); failed allocations surface to the script as exceptions.
- **Execution budget (opcodes + wall clock)**: each script entry point (module evaluation, hook call, pending job) is timed independently, 64M opcodes / 50ms by default (`ScriptRuntime::set_execution_budget`), enforced via `JS_SetInterruptHandler`; an entry over budget is interrupted and marked failed. The semantics are "bounded per entry": an infinite loop inside one hook is interrupted within budget and does not affect other entries.
- **Error isolation**: a compile failure or a thrown error in any hook logs the error (with stack) and marks the component failed; subsequent frames skip it. Hot-reload polling keeps running, so fixing the file restores it.
- One `JSContext` per component gives natural global isolation; the `JSRuntime` is process-shared with manual GC (no background thread, which also holds under Emscripten without pthreads).

## 7. Hot Reload

Every `reload_poll_interval` (0.5s by default; `<=0` disables) the script file is checked for changes; on change the source is re-read, the `JSContext` rebuilt, and the module re-executed. Change detection uses **mtime plus file size**: some filesystems (FAT/exFAT, network shares, certain virtual disks) have write-time granularity of 1-2 seconds, so consecutive editor saves land in the same tick and an mtime-only check misses them; edits within one tick usually change the length too, which the size criterion catches. Props are a serialized field of the component and survive context teardown; they are re-injected after recompilation. If the component had already started, `start` is re-invoked immediately after a successful reload. When loading fails (missing file), retries follow the poll interval, and the script loads as soon as the file appears and dispatches `update` in the same frame.

Command surface (driven by the editor through the command buffer, implemented in `process_command` in `api/core_api.cpp`):

| Command | Payload | Behaviour |
| --- | --- | --- |
| `ECMD_SET_SCRIPT` | `{ GEntityHandle, char script_path[120] }` | Swaps the target entity's script and loads it immediately (props preserved); an empty string unloads |
| `ECMD_RELOAD_SCRIPTS` | none | Force-reloads every enabled script component in the current scene, ignoring the mtime/size criteria |

## 8. Troubleshooting

| Symptom | Cause | Action |
| --- | --- | --- |
| Script does not run after scene load; compile error in log | Syntax error / missing file | Check the stack in the log; fixing the file restores it via hot reload |
| Same script error repeats in the log | Failed components stop executing; identical messages are not re-logged | Fix the file; hot reload restores it |
| Script update reports interrupted | A single entry exceeded the execution budget (64M opcodes / 50ms by default) | Optimize the script; raise `ScriptRuntime::set_execution_budget` if needed |
| Scripts see no input | Host does not dispatch `ECMD_INPUT_*` commands | Feed input through the command bus; `engine.input.mouse_delta()` etc. read zero meanwhile |
| Scripts do not run on the web build | `res:/scripts/*.js` not preloaded with the project | Ensure the files live inside the project directory (MEMFS preloads the whole project) |

## 9. Tests and Licensing

CTest gains four `script` suites (`GryceScriptTests.exe`, part of the 21-case gate): `value_convert` (conversion round-trips and boundaries), `lifecycle` (hook order / props / input dispatch), `error_isolation` (syntax error / runtime throw / infinite-loop interrupt), and `hot_reload` (file-change recompilation and late-appearing files). Same shape as the math tests: sources compiled directly in, no engine DLL link, GPU-independent.

QuickJS itself is MIT-licensed (`third_party/quickjs/LICENSE`), vendored and unmodified; the engine body is AGPL-3.0-or-later and third_party libraries keep their original licenses.
