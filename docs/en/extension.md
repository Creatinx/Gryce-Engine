# Native Extensions (extension.cpp)

This document is the complete usage and ABI reference for native extensions: how to add `engine.*` interfaces to `main.js` from C/C++, the exact semantics of every ABI field, value passing rules, build and distribution, and known pitfalls.

Extensions target the **entry script** ([app-entry.md](app-entry.md)). Component-level scripts ([scripting.md](scripting.md)) do not load extensions: they are instantiated inside arbitrary processes where no matching shared library is guaranteed to exist.

## 1. Scope

| Need | Right place |
| --- | --- |
| Level logic, camera control, UI flow, parameter tweaking | `main.js` |
| Project-private formats, platform APIs, third-party SDKs | extension |
| Performance-sensitive loops (per-point math, batch geometry) | extension |
| Engine internals not yet exposed | add an engine API first; extension v1 only reaches JS |
| Changing the render pipeline, asset import, or ECS behaviour | change the engine, not the extension |

The extension boundary is the **JS bridge**: it registers functions, reads scalar arguments, returns scalars, throws, and logs. It does not hold engine objects and cannot obtain scene or renderer handles.

## 2. Loading

| Step | Behaviour |
| --- | --- |
| Lookup | `--extension <path>` wins; otherwise `<project>/extension.dll` (Linux: `extension.so`) is loaded when present |
| Disable | `--no-extension` disables the **default lookup**; an explicit `--extension` still applies |
| Timing | Inside the `on_context_created` callback, ordered "install app bindings → load extension → evaluate `main.js`", so `main.js` top-level code can call extension interfaces |
| Failure | Logs `[app] extension load failed: ...` and keeps running; `main.js` starts normally. Probe optional capabilities with `typeof` on the script side |
| Unload | Never. The library lives as long as the process |
| Re-registration | A `main.js` hot reload rebuilds the `JSContext`, so the loading sequence replays: `dlopen` / `LoadLibrary` hits the already-loaded module (refcount stays non-zero), but a fresh `GryceExtensionApi` is created and the registration entry is called again |

Re-registration is the easiest thing to get wrong: **the registration entry may run multiple times and each call receives a new `api` object**. The host keeps every `api` object alive until the process exits, so old pointers never dangle; however, an old pointer's `js_ctx` refers to a destroyed context, and registering through it installs functions into a dead context. Refresh the cache on every registration:

```cpp
GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;
    g_api = api;   // overwrite every time: new context, new api
    api->add_function(api->js_ctx, "game", "version", &js_version, 0);
}
```

The extension's own statics (counters, handles, caches) are **not** reset by re-registration; they are process-level, consistent with a singleton.

## 3. Minimal Working Example

`extension.cpp` (the repository sample is `test/demo_project/extension.cpp`):

```cpp
#include "api/extension_api.h"

#include <string>

namespace {

GryceExtensionApi* g_api = nullptr;   // refreshed on registration, valid for the process

GJsValue js_add(void*, GJsValue, int argc, const GJsValue* argv) {
    double a = 0.0;
    double b = 0.0;
    if (argc < 2 || !g_api->arg_number(g_api->js_ctx, argv[0], &a) ||
        !g_api->arg_number(g_api->js_ctx, argv[1], &b)) {
        return g_api->throw_error(g_api->js_ctx, "engine.game.add(a, b) expects two numbers");
    }
    return g_api->ret_number(g_api->js_ctx, a + b);
}

GJsValue js_greet(void*, GJsValue, int argc, const GJsValue* argv) {
    char name[64] = {};
    if (argc < 1 || g_api->arg_string(g_api->js_ctx, argv[0], name, sizeof(name)) < 0) {
        return g_api->throw_error(g_api->js_ctx, "engine.game.greet(name) expects a string");
    }
    return g_api->ret_string(g_api->js_ctx, (std::string("hello, ") + name).c_str());
}

} // namespace

GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;
    g_api = api;
    api->add_function(api->js_ctx, "game", "add", &js_add, 2);
    api->add_function(api->js_ctx, "game", "greet", &js_greet, 1);
}
```

On the `main.js` side, probe optional capabilities so the same script runs without the extension:

```js
export function on_start() {
    if (typeof engine.game === 'object' && typeof engine.game.add === 'function') {
        engine.log.info('1+2=' + engine.game.add(1, 2));
    } else {
        engine.log.warn('extension not loaded, running without it');
    }
}
```

Build and run:

```bash
# Header path only, no engine link
g++ -shared -O2 test/demo_project/extension.cpp -I engine -o test/demo_project/extension.dll

# Or use the repository sample target (output goes to the build directory)
cmake --build build --target GryceDemoExtension

# The host looks for <project>/extension.dll by default; an explicit path also works
build/GryceApp.exe --project test/demo_project --extension build/GryceDemoExtension.dll
```

## 4. ABI Reference

Header: `engine/api/extension_api.h`. Pure C, `extern "C"` exports, no QuickJS includes, no link against engine libraries.

### 4.1 Entry Point and Register Function

```c
#define GRYCE_EXTENSION_VERSION 1u

typedef void (*GryceExtensionRegisterFn)(GryceExtensionApi* api);
#define GRYCE_EXTENSION_ENTRY_NAME "gryce_register_extension"
```

The `GRYCE_EXTENSION_ENTRY` macro expands to a `gryce_register_extension` definition carrying `dllexport` / `visibility("default")`. Write `GRYCE_EXTENSION_ENTRY { ... }` and never declare the entry name yourself.

### 4.2 `GJsValue`

```c
typedef struct GJsValue {
    int64_t payload;
    int64_t tag;
} GJsValue;
```

16 bytes, passed by value. It is an opaque mirror of `JSValue`: on desktop builds `JSValue` is 16 bytes (`{union, tag}`) and maps byte for byte; under Emscripten (`CONFIG_NAN_BOXING`) `JSValue` is a single 8-byte word, and the bridge places those bits in `payload`.

**Never read or write `payload` / `tag` directly.** Construct and read values only through the virtual table's `arg_*` / `ret_*`, the only stable interface across build configurations (NAN_BOXING, representation changes).

### 4.3 Virtual Table Fields

```c
typedef struct GryceExtensionApi {
    uint32_t version;
    uint32_t size;
    void* js_ctx;
    void (*add_function)(void* js_ctx, const char* ns, const char* name,
                         GJsNativeFn fn, int argc);
    GJsValue (*undefined)(void* js_ctx);
    GJsValue (*ret_number)(void* js_ctx, double v);
    GJsValue (*ret_bool)(void* js_ctx, int v);
    GJsValue (*ret_string)(void* js_ctx, const char* s);
    int (*arg_number)(void* js_ctx, GJsValue v, double* out);
    int (*arg_string)(void* js_ctx, GJsValue v, char* out, int cap);
    int (*arg_bool)(void* js_ctx, GJsValue v);
    GJsValue (*throw_error)(void* js_ctx, const char* msg);
    void (*log)(void* js_ctx, int level, const char* msg);
} GryceExtensionApi;
```

| Field | Semantics | Return / failure |
| --- | --- | --- |
| `version` | ABI version, currently `1`. Validate `api->version == GRYCE_EXTENSION_VERSION` and return without registering on mismatch | — |
| `size` | Size of this struct in bytes. If the host appends fields later this grows; before touching a newer field check `size >= offsetof(field) + sizeof(...)` | — |
| `js_ctx` | The `JSContext` this registration belongs to. In callbacks keep using the `js_ctx` argument (same value); do not cache it across registrations | — |
| `add_function(ns, name, fn, argc)` | Defines function `name` on `engine.<ns>`. `ns` as `NULL` or `""` targets `engine` itself (`engine.add()`). `argc` only sets the JS function's `length` property; it does not limit actual arguments | No return; any of `ns`/`name`/`fn` being null is silently ignored |
| `undefined()` | JS `undefined` | `GJsValue` |
| `ret_number(v)` | Returns a JS number (float64) | `GJsValue` |
| `ret_bool(v)` | Returns a JS boolean; `v != 0` is `true` | `GJsValue` |
| `ret_string(s)` | Returns a JS string (UTF-8, NUL-terminated). `NULL` yields an empty string; embedded `\0` truncates | `GJsValue` |
| `arg_number(v, out)` | Reads a number with ECMAScript `ToNumber` (`"2.5"` → 2.5, `true` → 1). Writes `*out` on success | `1` on success, `0` on failure (`out` untouched); `out == NULL` yields `0` |
| `arg_string(v, out, cap)` | Converts the value to a UTF-8 string into `out`, at most `cap-1` bytes plus a terminating 0. Any value goes through `ToString` (numbers → `"1.5"`, objects → `"[object Object]"`) | Bytes written (excluding the terminator, possibly a truncated length); `-1` on failure (empty `out` / `cap<=0` / `ToString` throws) |
| `arg_bool(v)` | JS truthiness: `0` / `null` / `undefined` / `""` / `NaN` are false, objects and arrays are true | `1` / `0` |
| `throw_error(msg)` | Throws a `TypeError` (`name: message`) and wraps the exception value | **Return it immediately**; do no further work |
| `log(level, msg)` | Writes an engine log line prefixed `[ext]`. `level`: `0` info / `1` warn / `2` error / `3` debug, anything else is info | None |

### 4.4 Namespace Resolution

Target resolution in `add_function` (`engine_namespace` in `extension_host.cpp`):

1. Empty `ns` → `engine` itself.
2. `engine[ns]` is an object → use it, so existing namespaces such as `engine.entity` and `engine.input` can be extended.
3. `engine[ns]` is missing or a non-object → a new empty object is created and **overwrites** it.

Registering the same name twice means the later registration wins; extensions install after the app bindings, so an extension *can* shadow an engine interface — avoid that. Use your own namespace (`engine.game`, `engine.mytool`) for isolation.

On the JS side these are plain functions; `this` is the caller object, delivered as the first parameter `this_val` (by value).

## 5. Value Passing Rules

| Direction | Supported types | How |
| --- | --- | --- |
| Argument in | number | `arg_number` |
| Argument in | string | `arg_string` with a fixed buffer |
| Argument in | bool | `arg_bool` |
| Argument in | array / object / struct | Serialize to a JSON string, pass with `arg_string`, parse with `nlohmann::json` (or your own parser) |
| Return | number / bool / string / `undefined` | `ret_number` / `ret_bool` / `ret_string` / `undefined` |
| Return | array / object / struct | Return JSON text via `ret_string`, `JSON.parse` on the JS side |
| Error | — | `throw_error`, then return immediately |

**Ownership**: `GJsValue`s produced by `ret_*` and `throw_error` are taken over by the engine (the bridge hands them back to QuickJS). Extensions must not free them, and there is no release API. The `argv` pointer and `this_val` are valid only for the duration of the call; copy anything you need to keep.

**Never store a `GJsValue` across calls**: it is not a stable reference-counted handle, so keeping one leaks the count or points into a destroyed context after a hot reload.

## 6. Lifecycle and State

| Event | What the extension observes |
| --- | --- |
| Host start | `dlopen` / `LoadLibrary`, registration entry called once (on the first `main.js` compilation) |
| `main.js` hot reload | Context rebuilt → registration entry called **again**; both `api` and `js_ctx` are new |
| `main.js` throws and stops dispatching | Extension functions are no longer called; the library stays loaded |
| Process exit | No unload hook is called; static destructors run at process teardown under the loader. Do not rely on destructors for cleanup |

Extension state is **process-level** and does not reset on scene load or `main.js` reload. Initialise lazily inside functions, or expose an explicit `engine.game.reset()` for JS to call.

Contrast with script components: each entity's component context is separate and extensions do not participate, so `engine.game` is `undefined` in component scripts.

## 7. Building and Distribution

| Approach | Command |
| --- | --- |
| MinGW / GCC (same shape on Linux) | `g++ -shared -O2 extension.cpp -I <engine> -o extension.dll` |
| MSVC | `cl /nologo /LD /O2 /I <engine> extension.cpp /Fe:extension.dll` |
| Repository sample target | `cmake --build build --target GryceDemoExtension` |

Notes:

- Only one include path (`<engine>/engine`) is needed; no `GryceCore` link and no QuickJS headers.
- Cross-compiler: only C types and PODs cross by value — no C++ classes, STL containers, exceptions, or RTTI — so there is no C++ ABI coupling; on the same platform and architecture, extensions built by MSVC and MinGW can both be loaded by the host. The repository has only verified the MinGW toolchain so far; if mixing compilers and something misbehaves, check calling conventions and aggregate passing first.
- Bitness must match the host. Mixing Debug/Release is fine (no C++ runtime boundary is crossed), but never pass `new`-allocated memory across the boundary — whoever allocates frees.
- Ship it as `<project>/extension.dll`; the packager copies the project directory, and the host loads by absolute path, so the extension's position relative to the host DLLs does not matter.

## 8. Debugging

- `g_api->log(g_api->js_ctx, 0, "msg")` writes straight to the engine log with an `[ext]` prefix — the cheapest channel.
- JS-side errors come from `throw_error` as `TypeError` with the message verbatim in the stack; a parameter check at the top of every function saves the most debugging time.
- Registration problems (wrong entry name, version mismatch, null `add_function` argument) never reach JS; only a load failure logs `[app] extension load failed`. If `engine.<ns>.<name>` is still `undefined` after loading, check whether the version guard returned early.
- When a crash happens inside an extension function, the last `[ext]` line or the nearby script line is the clue. The usual causes are a cached stale `js_ctx` (section 2) or a `GJsValue` stored in a callback.

## 9. Limits

- **Unsupported on the Web build**: no dynamic library loading in wasm; `load_extension_library` returns an error (`extensions are not supported on the Web build`), and `GryceApp` is not part of the Web build at all.
- Only scalar channels are exposed; structs go through JSON, so weigh the cost of calling with large objects at high frequency.
- Cannot register modules, extend JS syntax, or wrap existing functions (same-name registration only overwrites).
- Extensions cannot see scene or renderer objects; when engine capability is needed, add a C API on the engine side and expose it through the virtual table.
- The internal function table is append-only: every `main.js` reload appends one entry per `add_function`. Functionally harmless but a slow growth under frequent reloads; keep registration to a few functions where rapid reloading matters.

## 10. Troubleshooting

| Symptom | Cause | Action |
| --- | --- | --- |
| `engine.game` is `undefined` | Library not loaded (wrong path / missing `--extension`), entry name is not `gryce_register_extension`, or the version guard returned early | Check `[app] extension load failed` and registration logs; verify the export name |
| Loader reports a missing entry | C++ name mangling (no `extern "C"`) or `GRYCE_EXTENSION_ENTRY` not used | Define the entry with the macro |
| First call works, crash after editing `main.js` | Cached old `api` / `js_ctx` and did not refresh on registration | Overwrite the cached pointer in the registration entry |
| Exit code 139 / 0xC0000005 after a call | A `GJsValue` or `argv` pointer kept across calls, or an `api` pointer from a stack-local table | Copy contents by value only; never construct your own `GryceExtensionApi` |
| `arg_string` receives `"[object Object]"` | An object was passed instead of a string | `JSON.stringify` on the JS side, or change the contract |
| `arg_number` returns 0 for something numeric | `undefined` / `null` was passed, or the object cannot `ToNumber` | Check `typeof` first; supply defaults on the JS side |
| Returned strings are truncated or garbled | Buffer too small and truncated mid UTF-8 sequence, or non-UTF-8 passed to `ret_string` | Enlarge the buffer; always use UTF-8 |
| `engine.app.*` is unavailable inside the extension | Extensions can only register functions, not read engine state | Pass the needed data as arguments from `main.js` |
