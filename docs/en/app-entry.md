# Application Entry (main.js)

This document is the complete usage reference for the **entry script** form: the fixed `GryceApp.exe` host plus a project `main.js`, covering startup order, hooks, the full entry API, the interaction between the command line and `project.data`, and known limits.

Related documents:

| Topic | Document |
| --- | --- |
| Component-level scripting (`ScriptComponent`) | [scripting.md](scripting.md) |
| Complete native extension ABI and usage | [extension.md](extension.md) |
| Sandbox, execution budget, value conventions (shared) | [scripting.md](scripting.md) sections 6 and 7 |

## 1. Shape

| Role | Location | Varies per project |
| --- | --- | --- |
| Host template | `engine/app/app_main.cpp` (three lines calling `run(argc, argv)`) | No, shared by all projects |
| Host implementation | `engine/app/app_host.cpp` (arguments, init, frame loop, screenshots) | No |
| Entry script | `<project>/main.js`, default `res:/main.js` | Yes; all project logic lives here |
| Native extension | `<project>/extension.cpp` → `extension.dll` | Optional |

A project directory therefore no longer needs its own `main.cpp`: windows, scenes, cameras, and exit conditions are all decided by `main.js` calling `engine.*`. Scaffold a new project with `GryceGC new <dir>` (see [toolchain.md](toolchain.md)).

Division of labour with `ScriptComponent`: components live on entities and start/stop with the scene; the entry script belongs to the process itself, one per process, and owns "the whole application".

## 2. Startup Order

1. Parse the command line (`--project` / `--entry` / `--api` / `--w` / `--h` / `--frames` / `--shot` / `--extension` / `--fps` / `--debug-ui`) and read `<project>/project.data`.
2. `GCore_Init` with `GCore_SetAutoLoadMainScene(false)` (no automatic main-scene load), then push `ECMD_PLAY_MODE` so component `on_update`, animation, and physics are live on the first frame.
3. Create the `ScriptApplication` and register `on_context_created`: install app bindings, then load the extension library if present.
4. Read and evaluate `main.js` (ES module semantics), extract `on_start` / `on_update` / `on_destroy`, then dispatch `on_start()`.
5. Frame loop:

   ```text
   GWindow_PollEvents()          // only when a window exists
   GInput_SyncToCore()           // platform input -> command queue
   GCore_BeginFrame(dt)          // process commands, drive the World
   script.tick(dt)               // hot-reload poll + on_update(dt) + microtasks
   GRender_BeginFrame/RenderWorld/RenderGizmo/EndFrame   // only when a renderer exists
   GCore_EndFrame()
   ```

   Without `GInput_SyncToCore()` the `engine.input.*` readings never see input: it converts platform key/mouse state into `ECMD_INPUT_*` commands, which `GCore_BeginFrame` consumes into `g_input_state`.

6. On exit, dispatch `on_destroy()`, then tear down in the order **scene first (`GCore_Shutdown`), renderer second (`GRender_Shutdown`), window last**. Reversing it runs destructors against a released render context.

`dt` comes from `std::chrono::steady_clock` and is clamped to `[0.0005, 0.1]` seconds so a breakpoint or long stall does not produce a giant frame step.

## 3. Lifecycle Hooks

Functions exported by the module are the hooks; all three are optional:

| Hook | Signature | When |
| --- | --- | --- |
| `on_start()` | `export function on_start()` | Once after the entry loads; replayed after a successful hot reload |
| `on_update(dt)` | `export function on_update(dt)`, `dt` in seconds | Every frame, after `GCore_BeginFrame` and before rendering; `engine.input.*` already reflects this frame |
| `on_destroy()` | `export function on_destroy()` | Once before the process exits |

`this` is an empty object; the entry script has no host entity (so "host entity" interfaces such as `engine.entity.name()` throw `no host entity` here). A thrown error is logged (`name: message` + stack) and stops dispatching until a hot reload fixes the file.

Top-level code runs on every compilation (including hot reload) — good for constants, helpers, and capability probing. Anything that needs engine state belongs in `on_start`.

## 4. Entry API

The entry script is the base bindings (`engine.log` / `time` / `input` / `entity` from the host-entity view, see [scripting.md](scripting.md) section 5) plus the app bindings below. `install_app_bindings` adds window, scene, entity queries, and app control.

### 4.1 `engine.window`

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.window.create(opts)` | `bool` | Creates the window and initialises the renderer. `true` on success; throws `TypeError` when `width`/`height` are not positive; returns `false` and logs an error if window creation or renderer init fails |
| `engine.window.close()` | `undefined` | `GRender_Shutdown` then `GWindow_Destroy`; safe to call repeatedly |
| `engine.window.valid()` | `bool` | Whether the window is valid |
| `engine.window.size()` | `number[2]` | `[w, h]` from the framebuffer (differs from logical size under high DPI) |

`opts` fields:

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `title` | string | `"Gryce"` | Window title |
| `width` | number | `1280` | Client width in pixels, must be > 0 |
| `height` | number | `720` | Client height in pixels, must be > 0 |
| `api` | string | follows `--api` → `project.data.render_api` → `opengl` | `opengl` / `vulkan` / `dx12` / `webgl`; invalid values return `false` |
| `vsync` | bool | `true` | Vertical sync |
| `resizable` | bool | `true` | Parsed but not yet applied in this version (window mode is fixed to windowed) |

Semantics:

- **Idempotent**: when a window already exists it returns `true` immediately without rebuilding, so the new size/backend is ignored. To switch backends, `close()` first, then `create()`.
- Backend priority is `opts.api` > `--api` > `project.data.render_api` > `opengl`.
- Window creation also applies the `project.data` render parameters (HDR, tone mapping, exposure, shadows, ambient, IBL, SSAO/SSR/SSIL, PCSS).
- Skipping `create()` is valid: pure-logic scripts (numeric verification in CI, batch data processing) run entirely headless. Then `engine.app.has_renderer()` is `false` and `engine.app.screenshot()` returns `false`.

### 4.2 `engine.scene`

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.scene.load(path)` | `bool` | **Synchronously** loads a scene, replacing the current one. Returns `false` and logs an error on failure. `path` accepts `res:/` and absolute paths |
| `engine.scene.current()` | `string` | Current scene path; empty when nothing is loaded |

Loading clears the previous scene's entities: previously obtained entity proxies all become invalid (`valid()` returns `false`, other methods throw `stale handle` or fail). Call `find` again afterwards.

### 4.3 `engine.entity` (full enumeration)

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.entity.count()` | `number` | Total entity count, including roots |
| `engine.entity.find(name)` | entity proxy \| `null` | First **exact-name** match; `null` when absent |
| `engine.entity.all()` | array of entity proxies | All entities in handle-table order |
| `engine.entity.spawn(json)` | entity proxy | Creates an entity subtree from `GEntity_ImportJson` format (attached to the scene root, new UUID / EntityID); throws `import failed` |
| `engine.entity.destroy(e)` | `undefined` | Destroys an entity (proxy or raw handle); through the command buffer, processed at the next `GCore_BeginFrame` |

Enumeration uses `EntityHandleMap::all_handles()`, **not** `GEntity_GetCount/GetAt` — the latter counts only entities with a parent, so scene roots (camera, lights, models) are invisible there and it would report 0.

Entity proxy methods (`find` / `all` / `spawn` results):

| Method | Returns | Notes |
| --- | --- | --- |
| `e.id()` | `number` | Entity handle |
| `e.valid()` | `bool` | Whether the handle still points at a live entity |
| `e.name()` | `string` | Name |
| `e.set_name(s)` | `undefined` | Rename (command buffer, effective next frame) |
| `e.position()` | `number[3]` | Local position |
| `e.set_position(x,y,z)` / `([x,y,z])` | `undefined` | Write local position |
| `e.rotation()` | `number[4]` | Local rotation quaternion `[x,y,z,w]` |
| `e.set_rotation(x,y,z,w)` / `([x,y,z,w])` | `undefined` | Write local rotation |
| `e.scale()` | `number[3]` | Local scale |
| `e.set_scale(x,y,z)` / `([x,y,z])` | `undefined` | Write local scale |
| `e.translate(dx,dy,dz)` / `([dx,dy,dz])` | `undefined` | Additive translation |
| `e.enabled()` | `bool` | Entity enable bit |
| `e.set_enabled(bool)` | `undefined` | Write the enable bit |
| `e.destroy()` | `undefined` | Destroy this entity (command buffer) |

Once a handle goes stale, `valid()` returns `false` and property accessors throw `TypeError` (messages contain `stale handle` or `... failed`). Checking `valid()` before use is the cheapest habit for long-lived proxies.

### 4.4 `engine.app`

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.app.quit()` | `undefined` | Requests exit: the current frame finishes, `on_destroy()` is dispatched, and teardown runs normally |
| `engine.app.fps()` | `number` | Renderer FPS; `0` without a renderer |
| `engine.app.has_window()` | `bool` | Window validity (same as `engine.window.valid()`) |
| `engine.app.has_renderer()` | `bool` | Whether the renderer is initialised |
| `engine.app.screenshot(path)` | `bool` | Requests a screenshot. Returns `false` without a renderer or when the request fails |

`engine.app.screenshot` is **asynchronous**: the renderer writes the file on a later frame. The host tracks a four-frame flush counter, so:

- Keep running frames after the call; the file appears within four frames.
- Calling `quit()` right after a screenshot does not lose the image — the exit condition waits for the counter to reach zero.

Paths resolve against the process working directory; absolute paths are advisable.

### 4.5 Input

| Signature | Returns | Notes |
| --- | --- | --- |
| `engine.input.set_mouse_locked(bool)` | `undefined` | Pointer lock. While locked the cursor is not bounded by the window and `engine.input.mouse_delta()` keeps producing movement, which suits FPS cameras |

The remaining `engine.input.*` calls (`key_down` / `key_up` / `mouse_x` / `mouse_y` / `mouse_delta` / `mouse_button` / `mouse_locked`) match the component API; key codes are GLFW key codes, see [scripting.md](scripting.md) section 5.3.

## 5. `project.data` and the Command Line

Before loading the entry, the host reads `<project>/project.data` (JSON, same shape as `GryceRenderSmoke`). Fields actually used:

| Field | Default | Notes |
| --- | --- | --- |
| `name` / `window.title` | `"Gryce"` | Window title (`window.title` wins) |
| `window.width` / `window.height` | `1280` / `720` | Default window size |
| `render_api` | `"opengl"` | Default backend |
| `hdr` / `tone_map_mode` / `exposure` | `true` / `1` / `1.0` | Tone mapping |
| `shadow_enabled` / `shadow_map_size` | `true` / `2048` | Shadows |
| `ambient_r` / `ambient_g` / `ambient_b` | `0.2` / `0.22` / `0.26` | Ambient light |
| `ibl_intensity` / `default_environment` | `1.0` / `false` | Environment map |
| `ssao_enabled` / `ssr_enabled` / `ssil_enabled` | `false` | Screen-space effect switches |
| `ssr_max_steps` / `ssr_max_roughness` / `ssr_thickness` / `ssr_bilateral_filter` / `ssr_env_fallback` / `ssr_resolution_scale` | `64` / `0.6` / `0.1` / `0.5` / `1.0` / `1.0` | SSR parameters |
| `pcss_enabled` / `pcss_light_size` / `pcss_max_radius` | `false` / `0.06` / `12.0` | Soft shadows |

Priority: explicit command-line value > `project.data` > built-in default. A missing or unparsable `project.data` falls back entirely to built-ins with a single `[app] project.data parse failed` line.

## 6. Command Line

| Argument | Notes |
| --- | --- |
| `--project <dir>` | Project root (defaults to the process working directory; resolves `res:/` and is where `project.data` and the default extension are looked up) |
| `--entry <path>` | Entry script (default `res:/main.js`) |
| `--api <name>` | `opengl` / `vulkan` / `dx12`; takes priority over `project.data.render_api` |
| `--w` / `--h <px>` | Window size overrides (**two separate arguments**: `--w 1920 --h 1080`) |
| `--frames <N>` | Exit after N frames; `0` (default) means unlimited |
| `--shot <png>` | Screenshot after the frame count, then exit; implies `--frames 60` and waits for the flush |
| `--extension <path>` | Explicit extension library, overriding the default `<project>/extension.dll` |
| `--no-extension` | Disables the default lookup (an explicit `--extension` still applies) |
| `--fps` | FPS overlay in the top-left (off by default) |
| `--debug-ui` | Renderer ImGui debug panel (off by default; `GryceRenderSmoke`, not the runtime, is the smoke-test surface) |
| `--edit-mode` | Stay in edit mode (no Play, component updates off, undo stack records); overrides `start_in_play` in `project.data` |
| `--help` / `-h` | Print usage and exit |

Unknown arguments print the usage and exit with code 1.

**Start mode**: the default is Play (runtime semantics, component `on_update` runs).
Editor-style hosts set `"start_in_play": false` in `project.data` or pass `--edit-mode`;
in Play mode the engine skips undo snapshots, so an editor that forgets this ends up
with "edits apply but undo is always greyed out" (see [editor-api.md](editor-api.md)
section 2). A `[app] start mode: play|edit` line is printed at startup.

## 7. Native Extensions

Extensions are optional; see [extension.md](extension.md) for the full reference. Key points:

- An extension is an **interface layer**: it wraps engine capability that is implemented but not yet exposed to scripts into `engine.*` interfaces. Game logic still lives in `main.js`.
- The shared library exports `gryce_register_extension`; the virtual table comes from `engine/api/extension_api.h` (pure C, JS value channel only). To call engine capability, include `engine/api/*.h` and link the engine import library.
- Loading happens before `main.js` is evaluated, so top-level code can call extension interfaces.
- Interfaces appear as `engine.<namespace>.<function>`; the value channel is scalar-only, structs travel as JSON strings.
- Desktop builds only; the Web build has no dynamic library loading.
- Probe with `typeof` in `main.js` so the script still runs without the extension.

## 8. Example Walkthrough

Skeleton of `test/demo_project/main.js`, usable as a starting point:

```js
export function on_start() {
    // Optional extension: probe before calling (the script runs without it)
    if (typeof engine.ext === 'object' && typeof engine.ext.info === 'function') {
        engine.log.info('ext: ' + engine.ext.info());
    }

    if (!engine.window.create({ title: 'Gryce Demo', width: 1280, height: 720 })) {
        engine.log.error('window.create failed');
        return;
    }
    if (!engine.scene.load('res:/scenes/main.gesc')) {
        engine.log.error('scene load failed');
        return;
    }

    camera = engine.entity.find('MainCamera');
    if (!camera) {
        engine.log.error('MainCamera not found');
        return;
    }

    // Derive yaw/pitch from the scene camera's orientation so taking over does not snap
    const q = camera.rotation();
    const f = rotate(q, [0, 0, -1]);
    pitch = Math.asin(clamp(f[1], -1, 1)) * 180 / Math.PI;
    yaw = Math.atan2(-f[0], -f[2]) * 180 / Math.PI;
    position = camera.position();

    engine.input.set_mouse_locked(true);
}

export function on_update(dt) {
    if (engine.input.key_down(256 /* ESC */)) {   // GLFW key code
        engine.app.quit();
        return;
    }
    if (engine.input.mouse_locked()) {
        const d = engine.input.mouse_delta();
        yaw -= d[0] * 0.12;
        pitch = clamp(pitch - d[1] * 0.12, -89, 89);
    }
    // ... WASD movement, then write back to the camera
    camera.set_position(position[0], position[1], position[2]);
    camera.set_rotation(view_quat());
}

export function on_destroy() {
    engine.input.set_mouse_locked(false);
}
```

Three practices worth copying:

- **Early return on failure**: if window, scene, or camera lookup fails, `return`; otherwise every frame calls into a null proxy.
- **Derive the orientation**: do not assume the camera's initial rotation; solving yaw/pitch from `rotation()` avoids a visible snap when taking over.
- **Lock check**: only consume `mouse_delta()` while `mouse_locked()` is true, otherwise returning from Alt-Tab flings the view.

## 9. Verification

```bash
# One run per backend (window, scene, camera takeover, screenshot after 60 frames)
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_gl.png   --api opengl
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_vk.png   --api vulkan
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_dx12.png --api dx12

# Headless pure-logic run (cheapest way to verify script numerics in CI)
build/GryceApp.exe --project <dir> --entry res:/logic.js --frames 30
```

`ctest -R script_app_entry` covers entry-script hook dispatch, module swap on hot reload, and extension `add_function` registration without a GPU or window.

## 10. Limits

- **The Web build has no `GryceApp`**: the wasm side still runs `GryceRenderSmoke`'s resident loop with a preloaded payload, and extensions are unavailable.
- One entry script per process; switch scenes with `engine.scene.load` rather than loading a second entry.
- Hot reload uses mtime plus file size; an edit within the same time granularity that preserves length cannot be detected (same policy as `ScriptComponent`).
- `engine.window.create` can only succeed once; to change backends, `close()` first.
- The entry script has no host entity: interfaces like `engine.entity.name()` throw `no host entity`; entity access always goes through proxies from `find` / `all` / `spawn`.
- Namespaces such as `physics` / `audio` / `asset` do not exist on `engine`. Unimplemented interfaces never appear as empty functions, so `typeof` probing is reliable.
