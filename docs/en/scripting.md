# Scripting (QuickJS)

This document is the complete usage and API reference for **component-level scripting** (`ScriptComponent`, attached to entities): mounting, lifecycle hooks, the base `engine.*` interface, value conventions, sandboxing, hot reload, and troubleshooting.

The other two forms:

| Form | Scope | Document |
| --- | --- | --- |
| `ScriptComponent` | Entity-level, starts and stops with the scene | this document |
| `main.js` entry script | Process-level, one per process; window, scene, main loop | [app-entry.md](app-entry.md) |
| `extension.cpp` native extension | Exposes engine capability that scripts cannot reach yet to the entry script | [extension.md](extension.md) |

All three share one `JSRuntime` and the base bindings (`engine.log` / `time` / `input` / `entity`). What differs is the available API surface: component scripts have no window, scene, or app-control interfaces.

## 1. Components

| Part | Location | Notes |
| --- | --- | --- |
| QuickJS engine | `third_party/quickjs/` | Vendored MIT third-party static library (version 2026-06-04), `GryceQuickJS` target, unmodified |
| Runtime wrapper | `engine/script/js_runtime.{h,cpp}` | Process-level singleton: one shared `JSRuntime`, one `JSContext` per component; memory limit, execution budget, pending-job draining |
| Value conversion | `engine/script/js_value.h` | Single entry point for C++ <-> JS conversion (scalars / math types / `nlohmann::json`) |
| Base bindings | `engine/script/js_bindings.{h,cpp}` | `engine.log` / `time` / `input` / `entity` (component view) and `console.*` |
| Script component | `engine/script/script_component.{h,cpp}` | `ScriptComponent`: loading, compilation, hook dispatch, error isolation, hot reload |

The QuickJS CLI tools (`qjs.c` / `qjsc.c`) and `quickjs-libc.c` (the `std` / `os` modules) are not built, so scripts have no filesystem, process, network, or timer access; they reach the engine only through `engine.*`.

## 2. Quick Start

Attach a `ScriptComponent` to an entity in the scene file:

```json
{
  "type": "Script",
  "enabled": true,
  "script": "res:/scripts/demo_spin.js",
  "props": { "speed": 0.8 }
}
```

`res:/scripts/demo_spin.js`:

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

Command-line verification requires `GryceRenderSmoke` (or `GryceApp`) with `--simulate`, otherwise only `start` runs and `update` is never driven:

```bash
build/GryceRenderSmoke.exe --api opengl --project test/demo_project \
    --scene test/demo_project/scenes/main.gesc --frames 60 --simulate \
    --shot shot.png --no-debug-ui --no-fps
```

## 3. Mounting and Serialized Fields

`ScriptComponent` participates in scene serialization (`serialize` / `deserialize` in `engine/script/script_component.cpp`):

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `type` | string | — | Fixed `"Script"`; the component factory matches on it |
| `enabled` | bool | `true` | Component enable bit; entity-level `enabled` is separate |
| `script` | string | `""` | Script path. Supports `res:/` (relative to project root, Pak archive first) and absolute paths; an empty string means "not configured" and the component stays silent |
| `props` | object | `{}` | Arbitrary JSON object injected as `this.props`; non-objects are treated as empty |

`props` supports all JSON types and is converted recursively: `null` / bool / integer / float / string / array / nested object. Vectors and colours can therefore live in props:

```json
{ "type": "Script", "script": "res:/scripts/orbit.js",
  "props": { "center": [0, 2, 0], "radius": 3.5, "speed": 1.2, "label": "orbit" } }
```

```js
const c = this.props.center;   // [0, 2, 0]
const y = c[1] + Math.sin(engine.time.elapsed() * this.props.speed) * this.props.radius;
```

Reflection registration covers the scalar fields `script_path` and `reload_poll_interval`; `props` is a nested structure and is not reflected — editor data comes from the serialized fields themselves.

## 4. Lifecycle Hooks

Functions exported by the module (`export function`) are the hooks; `this` is the per-instance `self` object (which contains at least `props`). Every hook is optional.

| Hook | Signature | When | Component callback |
| --- | --- | --- | --- |
| `awake` | `awake()` | After the component attaches and compiles successfully. Fires on scene load; also on `set_script_path` and on `reload_now` when not yet started | `on_awake` |
| `start` | `start()` | When the scene starts, after all components initialize; replayed immediately after a successful hot reload if it had started before | `on_start` |
| `_input` | `_input(type, a, b, c)` | Before `update` each frame, one call per input event | inside `on_update` |
| `update` | `update(dt)` | Every frame, `dt` in seconds (float) | `on_update` |
| `destroy` | `destroy()` | Before the component is destroyed (scene unload, entity destruction, process exit) | `on_destroy` |

Module top-level code runs once at compile time and again on hot reload — good for constants and helper functions. Anything that needs engine state belongs in `awake` / `start`.

`_input` arguments (from `InputEvent` in `engine/runtime/engine_context.h`, filled in `api/core_api.cpp`):

| `type` | Constant | `a` | `b` | `c` |
| --- | --- | --- | --- | --- |
| 1 | `INPUT_EVENT_KEY_DOWN` | key code | 0 | 0 |
| 2 | `INPUT_EVENT_KEY_UP` | key code | 0 | 0 |
| 3 | `INPUT_EVENT_MOUSE_MOVE` | x | y | 0 |
| 4 | `INPUT_EVENT_MOUSE_DOWN` | button (0/1/2) | x | y |
| 5 | `INPUT_EVENT_MOUSE_UP` | button (0/1/2) | x | y |

Events are **not consumed**: every script component in the frame sees every event, and the host clears the queue at frame start. Several components reacting to the same event is normal, and there is no way (or need) to mark one handled.

## 5. The Base `engine.*` API (component view)

Installed by `install_engine_bindings` (`engine/script/js_bindings.cpp`); available to both component and entry scripts. The "error" column describes invalid-argument behaviour: a `TypeError` can be caught at the call site and aborts the current hook.

### 5.1 Logging

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.log.info(msg)` | `undefined` | Writes glog info (visible in the editor console) |
| `engine.log.warn(msg)` | `undefined` | warn |
| `engine.log.error(msg)` | `undefined` | error |
| `engine.log.debug(msg)` | `undefined` | debug |
| `console.log(...args)` | `undefined` | Same channel as `engine.log.info`, arguments joined by spaces |
| `console.warn/error/debug(...args)` | `undefined` | Same, at the matching level |

`engine.log.*` takes exactly one argument (zero arguments throws `TypeError`); `console.*` takes any number and converts non-scalars to JSON text, which eases porting web scripts.

### 5.2 Time

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.time.dt()` | `number` | This frame's dt (seconds), identical to the `update(dt)` argument |
| `engine.time.frame()` | `number` | Component frame counter; 1 on the first `update` |
| `engine.time.elapsed()` | `number` | Accumulated seconds (already includes this frame's dt when `update` runs) |

Time comes from `ScriptHost::host_dt/host_frame/host_elapsed`, advanced at the top of each `on_update`; it does not advance while detached or failed.

### 5.3 Input

Input state lives in `gryce_core::g_input_state` (the target of `EngineContext::input`): the host updates it through `ECMD_INPUT_*` commands, and `GCore_BeginFrame` clears the event queue and zeroes the mouse delta at frame start. Without a command bus (for example when a unit test drives the component directly) every query returns zero.

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.input.key_down(code)` | `bool` | Whether the key is currently **held** (stays true; not "pressed this frame") |
| `engine.input.key_up(code)` | `bool` | Whether the key is currently **not held** (note: not "released this frame") |
| `engine.input.mouse_x()` | `number` | Absolute mouse x (integer pixels) |
| `engine.input.mouse_y()` | `number` | Absolute mouse y (integer pixels) |
| `engine.input.mouse_delta()` | `number[2]` | Accumulated movement this frame `[dx, dy]`; floats keep sub-pixel precision |
| `engine.input.mouse_button(i)` | `bool` | Button state for `i` in 0/1/2; out-of-range is always `false` |
| `engine.input.mouse_locked()` | `bool` | Pointer lock state |

**Key codes are GLFW key codes**: printable characters use uppercase ASCII (`A=65`, `D=68`, `W=87`, `S=83`, `E=69`, `Q=81`), space is `32`, and function keys are `256` and up (`ESC=256`, `LEFT_SHIFT=340`, `LEFT_CONTROL=341`). The platform layer only polls GLFW's defined `32..348` range, so anything outside it always reads `false`.

`key_up` means "not held", so edge detection is manual:

```js
let was_down = false;
export function update() {
    const down = engine.input.key_down(32);
    if (was_down && !down) engine.log.info('space released');
    was_down = down;
}
```

### 5.4 Host Entity

In component scripts `engine.entity.*` implicitly targets the **host entity** (the entity carrying the component); no handle is needed. With no host entity (entry-script misuse, or an unattached component) these throw `TypeError`.

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.entity.self()` | `number` | Host entity handle. **Always 1 in v1**; treat it as a placeholder, not a general handle |
| `engine.entity.name()` | `string` | Entity name |
| `engine.entity.set_name(s)` | `undefined` | Rename; goes through the command buffer, effective next frame |
| `engine.entity.position()` | `number[3]` | Local position `[x,y,z]` |
| `engine.entity.set_position(x,y,z)` / `set_position([x,y,z])` | `undefined` | Write local position |
| `engine.entity.rotation()` | `number[4]` | Local rotation quaternion `[x,y,z,w]` |
| `engine.entity.set_rotation(x,y,z,w)` / `set_rotation([x,y,z,w])` | `undefined` | Write local rotation |
| `engine.entity.scale()` | `number[3]` | Local scale |
| `engine.entity.set_scale(x,y,z)` / `set_scale([x,y,z])` | `undefined` | Write local scale |
| `engine.entity.translate(dx,dy,dz)` / `translate([dx,dy,dz])` | `undefined` | Additive translation |
| `engine.entity.set_enabled(bool)` | `undefined` | Entity enable bit |

Position/rotation/scale and the enable bit write straight into the host entity's `transform()` / `enabled` and take effect the same frame. Only `set_name` goes through the command queue, effective next frame. The vector setters accept either components or an array; missing elements are treated as `undefined` (conversion fails, so that component keeps its previous value).

Component scripts do **not** have `engine.entity.find/all/count/spawn/destroy` — that group needs full-scene enumeration and is entry-script-only (see [app-entry.md](app-entry.md) section 4).

## 6. Value Conventions

- Vectors and quaternions are JS arrays: `[x,y,z]`, `[x,y,z,w]`; matrices are 16-element arrays in **column-major memory order** (`m[col*4+row]`, matching GLSL and `Matrix4f::m`).
- Entity handles are ints (`EntityID` is uint64 and never enters JS, to avoid precision loss beyond 2^53).
- `undefined` / `null` fail scalar conversion, which callers treat as "missing argument" (most interfaces throw `TypeError`); `NaN` and `±Infinity` follow ECMAScript `ToInt32` (`NaN → 0`).
- Promises work: pending jobs are drained after all `update` calls each frame (capped at 1024). There is no separate event loop and no `setTimeout` / `setInterval`.
- Strings are UTF-8; `engine.log` output is prefixed with `[script]`.

## 7. Sandbox and Limits

- **No filesystem / process / network / timers**: the `std` / `os` modules are not compiled, `require` does not exist, `setTimeout` does not exist. Scripts reach the engine only through `engine.*`.
- **Memory limit**: 256MB by default (`ScriptRuntime::set_memory_limit`); it must be called before any context exists to take effect. Failed allocations surface to the script as exceptions.
- **Execution budget (opcodes + wall clock)**: each script entry point (module evaluation, hook call, pending job) is timed **independently**, 64M opcodes / 50ms by default (`ScriptRuntime::set_execution_budget`), enforced through `JS_SetInterruptHandler`; an entry over budget is interrupted and marked failed. The semantics are "bounded per entry": an infinite loop in one hook is interrupted within budget and does not affect other entries.
- **Error isolation**: a compile failure or a thrown error in any hook logs the error (with stack) and marks the component failed; subsequent frames skip it. Hot-reload polling keeps running, so fixing the file restores it. An identical message is logged once, then summarized every 100 repeats with `(xN identical)`.
- One `JSContext` per component gives natural global isolation (`let` / globals are not shared across components); the `JSRuntime` is process-shared with manual GC and no background thread, which also holds under Emscripten without pthreads.

## 8. Hot Reload

Every `reload_poll_interval` (0.5s by default; `<=0` disables) the file is checked for changes; on change the source is re-read, the **`JSContext` is rebuilt**, the module re-executed, and exports re-extracted.

Change detection uses **mtime plus file size**: some filesystems (FAT/exFAT, network shares, certain virtual disks) have write-time granularity of 1-2 seconds, so consecutive editor saves land in the same tick and an mtime-only check misses them; edits within one tick usually change the length too, which the size criterion catches. An edit that preserves both mtime and length cannot be detected — use the command surface to force a reload in that case.

Post-reload semantics:

- `props` is a serialized component field and survives context teardown; it is re-injected as `this.props`.
- If the component had started before, `start` is replayed immediately after a successful recompile (`awake` is not).
- If loading had previously failed (missing file), retries follow the poll interval; when the file appears it loads that frame and `update` dispatches normally.
- Module-level variables, closures, and promises inside `update` are all discarded (the context is new). State that must survive a reload belongs in `props`.

Polling happens **before** `update` dispatch, so a saved file shows new behaviour within one poll interval plus one frame.

Command surface (driven by the editor through the command buffer, implemented in `process_command` in `api/core_api.cpp`):

| Command | Payload | Behaviour |
| --- | --- | --- |
| `ECMD_SET_SCRIPT` | `{ GEntityHandle, char script_path[120] }` | Swaps the target entity's script and loads it immediately (props preserved); an empty string unloads |
| `ECMD_RELOAD_SCRIPTS` | none | Force-reloads every enabled script component in the current scene, ignoring mtime/size |

## 9. Troubleshooting

| Symptom | Cause | Action |
| --- | --- | --- |
| Script does not run after scene load; compile error in log | Syntax error / missing file / wrong path prefix | Read the `name: message` and stack in the log; fixing the file restores it via hot reload |
| Same script error repeats in the log | Failed components stop executing; identical messages are only logged on first occurrence and every 100 repeats | Fix the file; hot reload restores it |
| `update` reports interrupted | A single entry exceeded the execution budget (64M opcodes / 50ms) | Optimize the script; raise `ScriptRuntime::set_execution_budget` if needed |
| Scripts see no input | The host does not dispatch `ECMD_INPUT_*` (no `GInput_SyncToCore`), or the CLI run lacks `--simulate` | Fix the host frame order; `engine.input.*` truthfully reports zeros meanwhile |
| `engine.input.key_up` behaves unexpectedly | Its semantics are "not held", not "released this frame" | Track the previous frame's state yourself for edge detection |
| `engine.entity.*` throws `no host entity` | Component-view interface used from the entry script, or the component is not attached | In the entry script use `engine.entity.find(name)` and entity proxies |
| Edits do not take effect | The change landed in the same mtime tick with identical length | Trigger `ECMD_RELOAD_SCRIPTS`, or change the file length |
| Scripts do not run on the web build | `res:/scripts/*.js` not preloaded with the project | Keep the files inside the project directory (MEMFS preloads the whole project) |

## 10. Tests and Licensing

`GryceScriptTests` (`ctest -R script`) covers five suites:

| Suite | Coverage |
| --- | --- |
| `value_convert` | Conversion round-trips and boundaries (NaN / Infinity / undefined / arrays / matrices) |
| `lifecycle` | Hook order, props injection, input event dispatch |
| `error_isolation` | Syntax errors, runtime throws, infinite-loop interrupts leaving other components untouched |
| `hot_reload` | Recompile on file change; loading once a missing file appears |
| `app_entry` | Entry-script hook dispatch, module swap on hot reload, extension `add_function` registration |

Sources are compiled directly into the test binary with no engine DLL link and no GPU dependency, the same shape as the math tests.

QuickJS itself is MIT-licensed (`third_party/quickjs/LICENSE`), vendored and unmodified; the engine body is AGPL-3.0-or-later and third_party libraries keep their original licenses.
