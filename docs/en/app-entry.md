# Application entry (main.js)

This document describes the "project is JavaScript only" runtime shape: the fixed host `GryceApp.exe` plus a project entry `main.js`, together with native API extension through `extension.cpp`. Component-level scripting (`ScriptComponent`) is covered in [scripting.md](scripting.md).

## 1. Layout

| Role | Location | Per project? |
| --- | --- | --- |
| Host template | `engine/app/app_main.cpp` (a few lines calling `run(argc, argv)`) | No, shared |
| Host implementation | `engine/app/app_host.cpp` (arguments, init, frame loop, screenshots) | No |
| Entry script | `<project>/main.js`, default `res:/main.js` | Yes, all project logic |
| Native extension | `<project>/extension.cpp` to `extension.dll` | Optional |

A project directory therefore no longer needs its own `main.cpp`: window creation, scene loading, camera control and exit conditions are all decided in `main.js` through `engine.*` calls.

Division of labour with `ScriptComponent`: a component is attached to an entity and follows the scene lifetime; the entry script belongs to the process itself, one per process, and owns "the whole application".

## 2. Startup sequence

1. Parse `--project` / `--entry` / `--api`, read `project.data` (window size, render defaults).
2. `GCore_Init` with `auto_load_main_scene = false`, then push `ECMD_PLAY_MODE` so component `on_update`, animation and physics are live from the first frame.
3. Load the extension library if present (before `main.js` is evaluated).
4. Evaluate `main.js` and dispatch `on_start()`.
5. Each frame:
   - `GWindow_PollEvents()` + `GInput_SyncToCore()` (without the latter `engine.input.*` never sees input);
   - `GCore_BeginFrame(dt)` (commands, world update);
   - `on_update(dt)`;
   - render one frame if a window exists;
   - `GCore_EndFrame()`.
6. On exit dispatch `on_destroy()`, then tear down in the order **scene, renderer, window**. Reversing it destroys components that still hold renderer handles (see the closing comment in `renderer_smoke.cpp`).

## 3. Lifecycle hooks

Exported functions are hooks; all three are optional.

| Hook | Timing |
| --- | --- |
| `on_start()` | Once after the entry loads; replayed after a successful hot reload |
| `on_update(dt)` | Every frame; `engine.input.*` already reflects the current frame |
| `on_destroy()` | Once before the process exits |

`this` is an empty object: the entry script has no host entity. A thrown error is logged (`name: message` plus stack) and stops dispatch until hot reload replaces the module.

## 4. Script-side API

On top of the component-level `engine.log` / `engine.time` / `engine.input` / host-entity surface, the entry script also gets:

| API | Meaning |
| --- | --- |
| `engine.window.create({title,width,height,api,vsync,resizable})` | Create the window and initialise the renderer; `api` falls back to `project.data`, accepts `opengl` / `vulkan` / `dx12` |
| `engine.window.close()` / `valid()` / `size()` | Destroy / query / `[w,h]` |
| `engine.input.set_mouse_locked(bool)` | Pointer lock |
| `engine.scene.load(path)` / `current()` | Load a scene (synchronous) / current scene path |
| `engine.entity.count()` | Total entity count, roots included |
| `engine.entity.find(name)` | Entity proxy or `null` |
| `engine.entity.all()` | Array of all entity proxies |
| `engine.entity.spawn(json)` / `destroy(e)` | Spawn from JSON (same format as `GEntity_ExportJson`) / destroy |
| `engine.app.quit()` / `fps()` / `has_window()` / `has_renderer()` | Request exit / frame rate / state |
| `engine.app.screenshot(path)` | Request a screenshot (written by the render thread on a later frame; the host waits four frames before exiting) |

Entity proxy methods:

```js
const cam = engine.entity.find('MainCamera');
cam.id();  cam.valid();  cam.name();  cam.set_name('Cam');
cam.position();  cam.set_position(0, 34, 46);
cam.rotation();  cam.set_rotation(qx, qy, qz, qw);
cam.scale();     cam.set_scale(1, 1, 1);
cam.translate(dx, dy, dz);
cam.enabled();   cam.set_enabled(true);
cam.destroy();
```

`engine.entity.count/find/all` enumerate the full handle map (`EntityHandleMap::all_handles`) rather than `GEntity_GetCount/GetAt`, which only reports entities with a parent and therefore hides scene roots.

## 5. Native extensions

An extension is a shared library exporting `gryce_register_extension`, loaded before `main.js` is evaluated. The ABI is a single C header (`engine/api/extension_api.h`) with no QuickJS headers and no engine link dependency, so extensions stay independent of the engine's C++ ABI and compiler flags.

```cpp
#include "api/extension_api.h"

static GryceExtensionApi* g_api = nullptr;

static GJsValue js_add(void*, GJsValue, int argc, const GJsValue* argv) {
    double a = 0.0, b = 0.0;
    if (argc < 2 || !g_api->arg_number(g_api->js_ctx, argv[0], &a) ||
        !g_api->arg_number(g_api->js_ctx, argv[1], &b)) {
        return g_api->throw_error(g_api->js_ctx, "engine.game.add(a,b)");
    }
    return g_api->ret_number(g_api->js_ctx, a + b);
}

GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;
    g_api = api;   // the pointer stays valid for the process lifetime
    api->add_function(api->js_ctx, "game", "add", &js_add, 2);
}
```

Notes:

- The `api` pointer lives for the whole process and may be cached in a static.
- Values cross the boundary as scalars only (`arg_number` / `arg_string` / `arg_bool` and the matching `ret_*`); use JSON strings for structures.
- Registration happens for every newly created context (including `main.js` hot reloads); the extension's own static state is not reset.
- Extensions are desktop-only: the Web build has no dynamic library loading.

Build either standalone or through the in-repo sample target:

```bash
g++ -shared -O2 test/demo_project/extension.cpp -I engine -o extension.dll
cmake --build build --target GryceDemoExtension
```

The host looks for `<project>/extension.dll` (`extension.so` on Linux), overridable with `--extension <path>` and disabled with `--no-extension`. The sample `test/demo_project/extension.cpp` exposes `engine.game.version/add/greet`; `main.js` feature-detects it with `typeof`.

## 6. Command line

| Flag | Meaning |
| --- | --- |
| `--project <dir>` | Project root (defaults to the working directory; `res:/` resolves through it) |
| `--entry <path>` | Entry script (default `res:/main.js`) |
| `--api <name>` | `opengl` / `vulkan` / `dx12`, overriding `project.data` |
| `--w` / `--h <px>` | Window size override |
| `--frames <N>` | Exit after N frames; `0` means unlimited |
| `--shot <png>` | Screenshot after the frame budget, then exit (implies `--frames 60`) |
| `--extension <path>` / `--no-extension` | Select or disable an extension |
| `--fps` | FPS overlay (off by default) |
| `--debug-ui` | Renderer ImGui debug panel (off by default) |

## 7. Verification

```bash
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_gl.png   --api opengl
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_vk.png   --api vulkan
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_dx12.png --api dx12
```

`ctest -R script_app_entry` covers hook dispatch, hot-reload module swapping and extension `add_function` without a GPU or a window.

## 8. Limitations

- The Web build does not ship `GryceApp`: wasm still uses the `GryceRenderSmoke` loop with a preloaded payload, and extensions are unavailable.
- Hot reload uses mtime plus file size; an edit that keeps the same length inside one filesystem timestamp tick is not detected (same rule as `ScriptComponent`).
- One entry script per process; switch scenes with `engine.scene.load` instead of loading a second entry.
- `engine.window.create` succeeds once while a renderer exists; repeated calls return `true` without recreating the window.
