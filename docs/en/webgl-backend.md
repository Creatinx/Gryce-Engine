# WebGL Backend Implementation Record

> Status: **Complete** (2026-10-17)
> Corresponding plan: `plans/webgl-backend-plan.md` (M0-M5 all landed)
> About **1,900 new lines** (shim 734 + ES300 converter 181 + probe 237 + shell 101 + shader variants 61 + `#ifdef __EMSCRIPTEN__` inline blocks and CMake branches about 600), with the GL backend's 4,393 lines **reused with zero rewrites**.

---

## 1. Quick Start

```bash
# Environment (one-time): emsdk installed, e.g. at /d/emsdk
export EMSDK=/d/emsdk
export EMSDK_NODE=/d/emsdk/node/<version>_64bit/bin/node
export EM_CONFIG=/d/emsdk/.emscripten
export PATH="/d/emsdk/upstream/emscripten:/d/emsdk/node/<version>_64bit/bin:$PATH"

CMAKE="D:/JetBrains/CLion 2026.1/bin/cmake/win/x64/bin/cmake.exe"
NINJA="D:/JetBrains/CLion 2026.1/bin/ninja/win/x64/ninja.exe"

# Configure (separate build-webgl/, does not touch the desktop build/)
emcmake "$CMAKE" -S . -B build-webgl -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE=Release -DGRYCE_ENABLE_D3D12=OFF

# Build
"$NINJA" -C build-webgl
```

### Single-file distribution (`GryceGC webpack`, independent of the build step)

CMake only produces multiple files (`--preload-file`, fast incremental links, 33% smaller). For the UE5-style "one html, double-click to run" form, use the packager as a **post-link** step: it does not touch the build system and does not re-link:

```bash
toolchain/build/GryceGC.exe webpack --build-dir build-webgl --name GryceRenderSmoke
# Output: build-webgl/GryceRenderSmoke.single.html (~5.1MB, zero external references)
```

| | Multi-file (CMake default) | Single file (`GryceGC webpack`) |
|---|---|---|
| Artifacts | `.html` + `.js` + `.wasm`(2.8M) + `.data`(639K) | **One ~5.1MB `.html`** |
| Resource packaging | `--preload-file` (separate .data, fetched at runtime) | inlined post-link (zero network requests at runtime) |
| Double-click from `file://` | no, CORS blocks fetch (origin 'null') | yes |
| HTTP server required | yes | no |
| Incremental link | fast (only wasm re-links) | fast (packaging is an independent post-link step) |
| Size | baseline | +33% (.wasm/.data inlined as base64) |

Implementation (`toolchain/src/core/web_pack.cpp`): inline Emscripten's `.js` into the html shell, then eliminate the remaining network requests via two public runtime hooks -- `Module.locateFile` returns a base64 data: URL for `.wasm`, and `Module.getPreloadedPackage` hands `.data` to the file packager directly as an ArrayBuffer. When `.data` does not exist it is skipped automatically (wasm only inlined). Render output is **byte-identical** to the multi-file HTTP build (960x540 screenshot mean|d|=0).

### Running

**Single-file mode**: double-click `dist/GryceRenderSmoke.single.html`, or open it from the command line. Arguments still pass through the URL query (`file://` URLs support `?query` too):

```bash
"/c/Program Files/Google/Chrome/Application/chrome.exe" --headless --no-sandbox --disable-gpu \
    --enable-unsafe-swiftshader --window-size=960,540 --virtual-time-budget=60000 \
    --screenshot="D:\\Gryce-Engine\\shot_webgl.png" \
    "file:///D:/Gryce-Engine/dist/GryceRenderSmoke.single.html?--api=webgl&--frames=8"
```

**Multi-file mode**: HTTP is mandatory (under `file://`, Chrome blocks the `.wasm`/`.data` fetch via CORS, reporting `Aborted(both async and sync fetching of the wasm failed)`):

```bash
cd build-webgl && python -m http.server 8932 --bind 127.0.0.1 &
"/c/Program Files/Google/Chrome/Application/chrome.exe" --headless --no-sandbox --disable-gpu \
    --enable-unsafe-swiftshader --window-size=960,540 --virtual-time-budget=60000 \
    --screenshot="D:\\Gryce-Engine\\shot_webgl.png" \
    "http://127.0.0.1:8932/GryceRenderSmoke.html?--api=webgl&--frames=8"
```

`build-webgl/index.html` (placed manually, not a CMake artifact) redirects the root path `/` to the app page while preserving the query.

**Real-GPU verification**: the two commands above both carry `--disable-gpu --enable-unsafe-swiftshader` and run software rasterization (SwiftShader reports 32 texture units and relaxes some WebGL validation), so they **cannot represent real hardware**. For real-GPU testing, switch to `--headless=new` and drop those three switches:

```bash
"$CHROME" --headless=new --no-sandbox --window-size=960,540 --virtual-time-budget=30000 \
    --screenshot=shot_webgl_gpu.png \
    --enable-logging=stderr --v=0 \
    "file:///D:/Gryce-Engine/dist/GryceRenderSmoke.single.html?--api=webgl&--frames=8&--no-debug-ui&--no-fps" \
    2>&1 | grep -o "GL_INVALID[A-Z_]*: [a-zA-Z:. ]*" | sort | uniq -c
```

The `GL_INVALID_OPERATION` count must be **0** -- WebGL sampler-type conflicts / unit overruns report once here and then silently drop draws, so judging by the picture alone is misleading.

**Command-line arguments** pass through the URL query (parsed by `test/webgl_shell.html`): `?--api=webgl&--frames=8&--scene=res%3A%2Fscenes%2Fstress.gesc`. Rules: the query is split on `&`, segments containing `=` become two argv entries, otherwise they pass as standalone flags. So both `--api=webgl` and `--api webgl` are expressible.

**Log readback**: the shell collects `Module.print`/`printErr` into `<pre id="log">`, and `--dump-dom` retrieves the full text; `--enable-logging=stderr --v=0` additionally captures console.

**Default page behavior (persistent interaction)**: without `--frames` / `--bench` / `--play-frames`, the web build enters the `emscripten_set_main_loop_arg` requestAnimationFrame loop and renders forever without exiting; clicking the canvas locks the pointer for mouse-look, WASD moves, Shift accelerates, Esc releases. The one-shot path ("render N frames -> screenshot -> exit") only runs when frame counts are explicitly requested (headless verification must be terminable). Three implementation constraints:

- `Window` / `FpsCamera` must be heap-allocated under `__EMSCRIPTEN__`: `set_main_loop` unwinds main's stack via exceptions, and stack-object destructors would destroy the canvas with it (symptom: blank page, getContext invalid).
- Pointer lock can only be triggered by a user gesture (browser security policy), and GLFW's `CURSOR_DISABLED` issued from the frame loop gets rejected -- the shell adds a `requestPointerLock()` in the canvas's mousedown.
- Under pointer lock the cursor position is a **monotonically accumulated quantity** with no "recentering" semantics: `FpsCamera::update` skips `set_cursor_pos` under `__EMSCRIPTEN__` and uses the actual position directly as the delta baseline (otherwise the view flies off).

**The page does not show logs by default**: `#log` / `#status` and the screenshot extraction link are `display:none` by default, shown only with `?debug=1` in the URL (`debug` is the shell's own switch, not forwarded to `main`). CSS hiding does not affect `--dump-dom` reading log text, so headless verification is unaffected. The log panel keeps only the most recent ~60KB so the persistent loop cannot bloat the DOM. In debug mode the canvas yields the lower half of the page (`body.debug` is a flex column); otherwise the full-bleed canvas pushes logs out of the viewport.

**Canvas fills the whole page**: the shell's CSS makes `#canvas` fill the viewport (`width:100%; height:100%`), and the render buffer is synchronized to the same size by the platform layer at window creation -- changing only the CSS makes the browser stretch the existing buffer, distorting the aspect ratio and showing low resolution at the CSS size. Conventions:

- Under `__EMSCRIPTEN__`, `Window` reads `emscripten_get_element_css_size` after creating the GLFW window, then writes back with **`glfwSetWindowSize`**. `emscripten_set_canvas_element_size` must not be used: it only changes `canvas.width/height`; the GLFW-side window size (the basis for pointer coordinate conversion) and framebuffer size are not updated, and no resize callback fires -- the symptom is the viewport stuck at the old value with a stretched picture.
- Pointer coordinates depend on this sync: GLFW converts mouse position using `canvas.width / CSS width`; when the two agree, this factor is the render scale, and view deltas after pointer lock are accurate.
- Browser resize events are coalesced for 80ms before syncing once (rebuilding render targets is a GPU resource operation, doing it per-event is visibly janky); the framebuffer callback then hands off to `GViewport_SetSize(w, h)` to rebuild HDR / G-Buffer / SSAO / SSR / SSIL targets, and the camera aspect is recomputed per frame from the viewport.
- Render scale `?rs=<factor>` (default 1 = CSS pixels, cap 2). Use it for native-pixel sharpness on high-DPI screens; cost grows with pixel area: rs=2 at 1080p is 4x the pixels.
- `--w/--h` no longer determine the viewport on the web -- there is no "window size" in a browser; canvas size is decided by the page layout.

Tiered verification (CDP `Emulation.setDeviceMetricsOverride` to change the viewport dynamically): `1258x566 -> 1600x900 -> 800x600`, with canvas CSS size, `canvas.width/height`, and the engine `viewport` consistent each time, `GL_INVALID_OPERATION` count 0, and correct aspect after two resizes.

---

## 2. Architecture: Single-Header Shim, Zero Desktop Awareness

WebGL2 = GLES 3.0, a **strict subset** of desktop GL. All differences are collected in one header:

```
cmake/web/GL/glew.h          (12 lines) becomes the <GL/glew.h> stand-in when prepended to the include path
   └── engine/render/opengl/gl_webgl_shim.h   (734 lines, active only under #ifdef __EMSCRIPTEN__)
```

9 existing `#include <GL/glew.h>` lines are **unchanged**. `cmake/web` is only added to the include path in the `EMSCRIPTEN` branch; desktop builds never enter that directory.

The shim has four internal categories, divided by "whether it is actually reachable":

| Category | Handling | Examples |
|---|---|---|
| DSA entry points (actually reachable) | bind-based **real implementation**, target reverse-looked-up from a registry | `glCreateTextures` / `glTextureStorage2D` / `glTextureSubImage2D` / `glNamedFramebufferTexture` / `glCompressedTextureSubImage2D` |
| Missing capability but translatable semantics | **real implementation** + capability translation | `glDrawBuffer`->`glDrawBuffers(1,&buf)`; `GL_CLAMP_TO_BORDER`->`GL_CLAMP_TO_EDGE` (see section 4) |
| Desktop-only capabilities | loud-error placeholders (`unsupported_call()` logs once) | `glDispatchCompute` / `glDrawElementsIndirect` / `glNamedBufferData` / `glGetBufferSubData` / `glMemoryBarrier` |
| Pure compile placeholder enums | `#define` to desktop values | 22 sampler/uniform type enums, `GL_COMPUTE_SHADER`, `GL_DEBUG_SEVERITY_*`, `GL_SHADER_STORAGE_BLOCK` |

**Target registry** (`std::unordered_map<GLuint, GLenum>`): DSA calls do not carry a target, while bind-based equivalents must know it. Both the `glCreateTextures` and the wrapped `glBindTexture` creation paths register; `glDeleteTextures` invalidates synchronously, avoiding cross-labeling after id reuse.

**Macro renaming** is placed at the **very end** of the file (after all inline definitions), because the definition bodies call the real ES3 functions -- putting it first would recurse infinitely.

### 2.1 A pitfall worth remembering: DSA signatures carry no target

Initially a `GLenum target` parameter was added to `gryce_webgl_texture_storage2d`, and all 12 call sites failed to match at compile time. Desktop GL's `glTextureStorage2D(GLuint texture, GLsizei levels, GLenum internalformat, ...)` **never had a target** -- that is exactly why a registry is needed. Signatures must match desktop word for word.

---

## 3. ES300 Shader Conversion (runtime, in-memory, zero file copies)

`to_glsl_es300()` in `gl_shader.cpp` (181 lines) is called inside the compile loop of `GLShader::compile` / `link_program` / `link_compute_program`. Four mechanical conversions:

1. **Version line rewrite**: `#version 330 core` / `430 core` -> `#version 300 es`. Both ESSL and ANGLE require `#version` on the **first line** (even leading comments trigger `#version directive must occur on the first line of the shader`), so the original version line and everything before it are discarded wholesale; non-comment prefixes produce a warning.
2. **Precision block injection**: ES 300 fragment shaders have no default float/int precision; without declarations compilation fails outright. Injects `precision highp float/int/sampler2D/samplerCube/sampler2DShadow/sampler2DArray`.
3. **Uniform initializer stripping**: ESSL 3.00 **forbids** it (ANGLE reports `cannot initialize this type of qualifier`), desktop GLSL 330 allows it. The engine has 78 uniforms with initializers (tonemap / TAA / SSR switches). Because `uniform` is not a valid local-variable qualifier in GLSL, any line starting with `uniform` must be a global declaration, so **no brace-depth tracking is needed**. Stripping cuts comments along `//` and `/*` before finding `=`, otherwise comments like `// 0 = vertex attribute instance stream` get misjudged. All engine uniforms are written per frame from the CPU, so stripping does not change behavior; stripped names are still listed one by one via GLOG_INFO, so project override shaders relying on shader-side defaults surface here.
4. **`texture2D(` -> `texture(`**: defensive; measured zero usage in the engine.

Only one shader variant was written: `webgl_instanced.vert` (61 lines, drops the SSBO, goes through the vertex-attribute instance stream). `scene_pbr_ssbo` is not a separate file; `scene_shader_forward_clustered.cpp` routes it to `scene_pbr` under `__EMSCRIPTEN__`.

### 3.1 The parser must know the current api

`GLShader::load_program` / `load_compute` previously hardcoded `RenderAPI::OpenGL`, so the `webgl_{name}` priority branch in `shader_source_resolver` never took effect (symptom: getting the SSBO version of `instanced.vert`, reporting `std430 : invalid layout qualifier`). It now switches to `RenderAPI::WebGL` under `#ifdef __EMSCRIPTEN__`.

---

## 4. Platform Differences, Item by Item

| WebGL2 constraint | Handling | Evidence |
|---|---|---|
| No DSA | shim bind-based real implementation + target registry | probe confirms `glCreateTextures`/`glNamedFramebufferTexture` both absent |
| No compute shader | `GLEW_VERSION_4_3 0` -> `supports_compute_shaders()=false` -> existing CPU culling path | `render.h` already requires "fall back to CPU path when the backend does not implement it" |
| No indirect draw | loud-error placeholder; the engine submits on the CPU by default | -- |
| No `sampler2DMS` / MSAA render-to-texture | `max_msaa_color_samples=1`, `RenderPipeline` disables MSAA automatically | probe: `WEBGL_multisampled_render_to_texture = no` |
| No `GL_CLAMP_TO_BORDER` / `GL_TEXTURE_BORDER_COLOR` | the `glTexParameteri/SamplerParameteri` wrapper maps BORDER wrap to EDGE; `TEXTURE_BORDER_COLOR` calls are dropped and logged once | see below |
| No `GL_TIME_ELAPSED` | `GLEW_VERSION_3_3 0` -> GPU profiling off | probe: `EXT_disjoint_timer_query_webgl2 = no` |
| No `glGetBufferSubData` | loud-error placeholder + dst zeroed (no heap leak) | SSBOs themselves do not exist, blocked at runtime by null checks |
| RGBA16F needs an extension | runtime probe of `EXT_color_buffer_float`, explicit GLOG_ERROR failure when missing | probe: **available** |
| Anisotropic filtering | `GLEW_EXT_texture_filter_anisotropic` becomes a runtime extension probe function | probe: **available** |

**Why border clamp can degrade safely**: the engine uses it only on depth/shadow maps, expecting "out-of-bounds sampling returns depth 1.0 = fully lit = no projection". This semantics is guaranteed shader-side -- `cascade_shadow()` in `pbr.frag` / `skinned_pbr.frag` / `deferred_lighting.frag` all have `if (proj.x<0.0 || proj.x>1.0 || proj.y<0.0 || proj.y>1.0) return 1.0;` explicit early-out, plus a 5% edge `smoothstep` fade to fully lit; the PCF kernel is only a few texels wide (<< 5% of the map edge), fully covered by the fade region.

### 4.1 Texture unit budget: 16, a compile-time hard constraint

Real-GPU WebGL2 provides only **16** fragment texture units (ANGLE/D3D11 reports 16; SwiftShader reports **32**, so anything that "works under headless software rasterization but is all black on real hardware" should suspect this first). GLSL sampler counting happens at **compile time**: declared and referenced counts as occupying a unit; runtime `if` and constant folding cannot save it.

So the pbr-family shaders trim features per backend: the ESSL300 conversion in `gl_shader.cpp` injects the four `GRYCE_HAS_*` switches as 0 (`GRYCE_HAS_PCSS / _VSM / _GI / _POINT_SHADOW`), with the shaders using `#ifndef` fallbacks to 1. Note it must be "a single `#ifndef` fallback" and not the `#ifdef/#else` two-branch pattern -- D3D12's vulkanize replays all `#define`s, and two branches give the same macro two substitution values, making shaderc report `Macro redefined` (see the last row of the troubleshooting table).

| Shader | Desktop | WebGL | Trimmed |
|---|---|---|---|
| `pbr.frag` | 25 | 14 | PCSS raw depth(4), VSM/ESM(4), SDFGI(1), point-light shadows(2) |
| `instanced.frag` | 25 | 14 | same |
| `skinned_pbr.frag` | 18 | 14 | PCSS raw depth(4) |

What remains is the PBR main chain: 6 material maps + 4 cascade PCF + IBL trio + SSAO. Trimmed features **degrade silently** under WebGL (PCSS->PCF, VSM/ESM->PCF, no GI indirect light, point lights do not cast shadows); the demo scene uses a directional light with GI already off, so there is no visual difference.

The slot -> unit mapping is tiered accordingly (`gl_slot_map.h`): desktop 32 units keep the original table, 16 units use a compact table -- the forward main pass's 14 samplers land in 0..15, postprocessing reuses 0..7 (not coexisting with forward), and samplers already trimmed by WebGL (PCSS depth, point-light shadows, VSM) go to the 16+ dead zone **bound but not sampled**. WebGL additionally requires that a single unit not be both `sampler2D` and `samplerCube`, so IBL's two cubemaps occupy 4/5 alone.

---

## 5. Threading Model: Three Synchronous Degradations

No `-pthread` (avoiding the SharedArrayBuffer / COOP / COEP deployment threshold), so `std::thread` construction throws `system_error: thread constructor failed`. Three places degrade by the same pattern, with the **API surface unchanged**:

| Component | Degradation |
|---|---|
| `AsyncLogger` in the `GLog` singleton | `log()` writes synchronously straight to the inner backend. This is the most hidden one: `GLog::GLog()` constructs on the first `GLOG_*`, and nobody catches the exception -> the whole runtime `Aborted()` with no information |
| `AsyncLoader` (resource loading thread pool) | `submit()` runs the worker immediately and pushes into `completed_`; `poll()` executes callbacks on the main thread as usual -- the semantic contract is unchanged, only the "async" window disappears |
| `RenderThread` | `GRender_Init` forces `sync_mode=true` under `__EMSCRIPTEN__` (WebGL context is main-thread bound), and the thread never starts |

Three more are **link-time** problems, handled with empty `INTERFACE IMPORTED` targets to keep the link line unchanged, with source stubbed via `#ifdef`: Box2D / Jolt / Amplitude (prebuilt MinGW `.a` cannot link for wasm). Assimp is OBJ-only in the first version (built-in `ObjLoader`, zero dependencies).

---

## 6. Build Details (`build-webgl/`)

- **DLL -> static**: `_gryce_lib_type` in `engine/CMakeLists.txt` takes STATIC under `EMSCRIPTEN` (cmake 4.3.1 does not support emscripten shared libraries; the warning is harmless).
- **`SUFFIX ".html"`**: Emscripten only applies `--shell-file` when generating html output; otherwise it warns `--shell-file ignored when not generating html output` and **silently discards** the shell.
- **preload payload**: `test/webgl_demo@/webgl_demo` (MEMFS project) + `engine/render/shaders@/shaders`. The latter is required: `core_shaders::engine_shaders_dir()` falls to `current_path()='/'` on the web, whose candidate #1 is exactly `<base>/shaders`.
- **Payload changes must trigger a re-link**: `--preload-file` packs at link time, so not re-linking on changed/added files leaves `.data` stale, showing up as "changed the scene but nothing happened". `target_sources` + `HEADER_FILE_ONLY` only attach to the compile step, not the link step (ninja's LINK statement depends only on `.o` and `.a`), so instead use `set_source_files_properties(test/renderer_smoke.cpp PROPERTIES OBJECT_DEPENDS <glob>)` + `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`: payload changes -> recompile that `.o` -> guaranteed re-link.
- **`GRYCE_WEBGL_DEBUG`** (default ON): `-sASSERTIONS=1 -sDISABLE_EXCEPTION_CATCHING=0 -sSTACK_OVERFLOW_CHECK=1`. Without it, abort reports only `Aborted()` and section 5 class problems cannot be located.
- **`-sEXPORTED_RUNTIME_METHODS=FS`**: the shell's screenshot extraction reads MEMFS. Emscripten 6.x made unexported runtime methods "abort on access" -- touching `Module.FS` in the shell triggers `Aborted('FS' was not exported. add it to EXPORTED_RUNTIME_METHODS ...)`, killing the whole runtime mid-`main()` (symptom: only the first one or two log lines print). The shell side also has try/catch defense, double insurance.
- **x86-v3 ISA flags** are excluded under `EMSCRIPTEN` (wasm does not accept `-mavx2`).
- CTest and the toolchain are skipped under `EMSCRIPTEN`; `GryceSyncRuntimeDlls` is skipped wholesale.

---

## 7. Verification Results

### 7.1 M0 capability probe (Chrome headless + SwiftShader)

`test/webgl_probe/`, result `PROBE_RESULT PASS`:

| Probe | Result | Meaning for the engine |
|---|---|---|
| `EXT_color_buffer_float` | **yes** | RGBA16F HDR pipeline available |
| `OES_texture_float_linear` | yes | float textures can be mipmapped / linear filtered |
| `EXT_texture_filter_anisotropic` | yes | AF available (the runtime probe was the right call) |
| `EXT_disjoint_timer_query_webgl2` | no | GPU profiling disabled |
| `WEBGL_multisampled_render_to_texture` | no | MSAA render-to-texture unavailable -> `max_msaa_color_samples=1` |
| DSA `glCreateTextures` / `glNamedFramebufferTexture` | **no** | proves the DSA shim is necessary |
| FBO RGBA16F / DEPTH24 | both COMPLETE | HDR + depth texture attachments available |
| MEMFS `--preload-file` | 21 bytes read back | resource loading chain works |
| `readPixels` center | `(230,76,51,255)` | frame capture / readback chain works |
| `glGetError` | `0x0` | no hidden errors |

### 7.2 Same-scene GL vs WebGL A/B (`test/webgl_demo`, 960x540, `--no-debug-ui --no-fps --no-ssr`)

| `--debug` | Desktop GL | WebGL | Diff |
|---|---|---|---|
| 0 (final composite) | (138.6,138.3,144.7) | (138.7,138.4,144.8) | 0.1/255 |
| 1 (albedo) | (135.8,135.0,139.2) | (135.8,135.0,139.2) | **0** |
| 2 (normal) | (131.1,146.3,134.8) | (131.1,146.3,134.8) | **0** |
| 3 (roughness) | (139.2,139.2,142.6) | (139.2,139.2,142.6) | **0** |
| 4 (metallic) | (51.6,51.6,55.0) | (51.6,51.6,55.0) | **0** |
| 5 (shadow) | (141.1,141.1,144.5) | (141.2,141.2,144.5) | 0.1/255 |
| 6 (direct) | (127.4,126.6,131.7) | (127.4,126.7,131.8) | 0.1/255 |
| 7 (ambient/IBL) | (105.3,106.9,117.0) | (105.3,107.0,117.0) | 0.1/255 |
| 8 (cascade) | (55.4,134.4,61.4) | (55.4,134.4,61.3) | 0.1/255 |

4/9 are identical per channel, the rest 0.1/255 -- far below the noise floor of two runs at the same settings (0.03%). albedo/normal/roughness/metallic all equal means texture upload, sRGB, mipmap, and uniform paths are entirely correct; shadow/direct/ambient/cascade mean HDR, shadow atlas, PCF, IBL, and tonemap are all correct.

> The table above is a snapshot from before `slope_bias` was adjusted to x2. bias only affects the three shadow-bearing rows (0 composite / 5 shadow factor / 6 direct light); other rows are backend-independent and numerically unchanged. The post-adjustment composite baselines: desktop GL `(140.1,139.8,145.9)`, WebGL `(139.5,139.2,145.4)` (the WebGL viewport is an in-page canvas of 938x442, different from the desktop 960x540, so the two differ by ~0.6/255 to begin with).

### 7.3 GPU culling CPU fallback A/B

| Config | Result |
|---|---|
| Desktop GL, GPU culling | (120.6,108.4,109.5) 50.30% 367 bands |
| Desktop GL, `--no-gpu-cull` | (120.6,108.4,109.5) 50.30% 367 bands -- **byte-identical** |
| WebGL (CPU fallback) | (120.8,108.6,109.6) 50.27% 247 bands -- differs by 0.2/255 |

### 7.4 Stress scene

`test/webgl_demo/scenes/stress.gesc`: 115 entities / 112 meshes (10x10 cube array, 6 materials cycling: glossy / chrome / gold / plastic / rough / dark-metal + 12 spheres). Renders correctly, see `shot_webgl_stress.png`.

### 7.5 ImGui ES3

`engine/CMakeLists.txt` adds `IMGUI_IMPL_OPENGL_ES3` to `GryceRenderer` (otherwise imgui's `imgui_impl_opengl3.h` auto-selects **ES2** on Emscripten). The debug panel renders correctly. `FontAtlas` cannot find `C:\Windows\Fonts\*.ttf` (no system fonts in MEMFS) and falls back to ImGui's built-in default font, **non-fatal**; Chinese/monospace fonts need separate preloading.

### 7.6 Frame capture readback (verified)

`GLBackend::capture_frame_rgba` -> `read_framebuffer_rgba_topdown` (`glReadPixels`) -> `stbi_write_png` -> the smoke program `stbi_load`s it back to compute means. The write+read round trip holds on the web (the `[smoke] image 960x540 mean=...` from `--shot shot_webgl.png` is the evidence).

The shell additionally has **MEMFS screenshot extraction**: it polls png/bmp under `/`, reads them as base64 data URLs into the DOM, and `--dump-dom` retrieves them directly. Verified decoding of 2,074,523 bytes of valid PNG (both the picture and the ImGui debug panel present), i.e. the whole `capture_frame_rgba -> PNG -> out of the browser` chain works. Note this requires `-sEXPORTED_RUNTIME_METHODS=FS`, see sections 6 and 9.

### 7.7 Zero desktop regression

| Check | Result |
|---|---|
| Desktop `ninja -C build` full build | clean pass |
| CTest | **16/16 all green** |
| `--api opengl` mean | (92.3,87.6,85.3) -- +0.2 after bias x2 (self-occlusion stripes removed) |
| `--api vulkan` mean | (94.3,89.2,85.8) -- consistent with historical baseline |
| `--api dx12` mean | (94.6,89.4,86.1) -- +0.3 after bias x2; also fixed the pbr compile failure from `GRYCE_HAS_*` macro redefinition |

---

## 8. Known Limitations / Unverified

| Item | Status |
|---|---|
| **Firefox / Edge cross-verification** | **Not done** -- neither browser is installed on this machine; all verification went through Chrome headless (SwiftShader software rendering). Acceptance criterion 1 completed 1/3 |
| Real GPU (non-SwiftShader) | **Verified** (2026-10-17, `--headless=new` via ANGLE/D3D11): render results consistent with SwiftShader (mean\|d\|=0.11, differences from power-saver mode and shadow filtering degradation). See sections 4.1 and 9 for two real-hardware-only troubleshooting items |
| Assimp (FBX/glTF import) | Not done. Web first version is OBJ-only; `--preload-file` with a 92MB FBX is unsuitable for web distribution |
| Physics / audio | stubbed silent. Building Box2D / Jolt / Amplitude from source under Emscripten is listed as a future option |
| ImGui Chinese fonts | require preloading font files into MEMFS |
| Mobile GPUs | unverified |
| Release build with `GRYCE_WEBGL_DEBUG` off | size/behavior differences unverified |

---

## 9. Troubleshooting Cheat Sheet

| Symptom | Cause |
|---|---|
| `Aborted(both async and sync fetching of the wasm failed)` | Opened via `file://`. HTTP is required |
| `Aborted()` with no other information | `GRYCE_WEBGL_DEBUG` not enabled; or a `std::thread` was constructed without pthread |
| `std::__2::system_error: thread constructor failed: Not supported` | Another new `std::thread` user missed the synchronous degradation (AsyncLogger was the earliest hit) |
| `#version directive must occur on the first line of the shader` | The converter did not put `#version` on the first line (old bug, fixed) |
| `'esprecision' : invalid version directive` | Missing `\n` after `#version 300 es`, glued to the precision block (old bug, fixed) |
| `cannot initialize this type of qualifier` | Uniform initializers not stripped (old bug, fixed) |
| `'std430' : invalid layout qualifier` | Got the SSBO shader: `load_program` did not pass `RenderAPI::WebGL` (old bug, fixed) |
| `'sampler2DShadow' : Illegal use of reserved word` | `#version` rejected, degrading to ES 1.00 semantics; usually a side effect of the previous problem |
| `failed to resolve 'pbr' in 'webgl_demo/shaders'` | The shader tree was not preloaded to `/shaders` |
| `still waiting on run dependencies: datafile_single/GryceRenderSmoke.data` spamming, `main()` not executing | Assuming `-sSINGLE_FILE=1` inlines `.data` too -- **it only inlines wasm**; `--preload-file`'s `.data` still fetches. Single-file distribution has moved to `GryceGC webpack` post-link processing (inline js + `Module.locateFile`/`Module.getPreloadedPackage`); CMake no longer offers a single-file mode |
| Changed a scene/shader but nothing happened | payload did not trigger a re-link (the `OBJECT_DEPENDS` section) |
| `--shell-file ignored when not generating html output` | the target is missing `SUFFIX ".html"` |
| `Aborted('FS' was not exported...)` | The shell read `Module.FS` without `-sEXPORTED_RUNTIME_METHODS=FS`; **it kills the runtime mid-main()**, showing as only one or two log lines |
| `WebGL2 shim: glTexParameterfv(GL_TEXTURE_BORDER_COLOR) is not available` | **Expected**; border clamp has degraded to edge clamp per section 4 |
| **Only ImGui on screen, all 3D black**, log shows `FRAGMENT shader texture image units count exceeds MAX_TEXTURE_IMAGE_UNITS(16)` + `RenderPipeline: failed to load shader program 'pbr'` + `falling back to clear-only` | Sampler count over the limit. Desktop GL gives 32 fragment texture units, **real-GPU WebGL2 only 16** (ANGLE/D3D11), while SwiftShader reports 32 -- so software rasterization never exposes it, real hardware is always black. pbr-family shaders had 25 samplers. Fix: the ESSL300 conversion in `gl_shader.cpp` injects `GRYCE_HAS_PCSS/_VSM/_GI/_POINT_SHADOW` as 0, and pbr/instanced/skinned_pbr trim PCSS raw depth(4), VSM/ESM(4), GI(1), point-light shadows(2) accordingly, 25->14, 18->14 |
| Picture entirely **pure green** (RGB 0,219,0) or garbled, log shows `GL_INVALID_OPERATION: glDrawElements: Two textures of different types use the same sampler location` | Texture unit slot mapping collapsed under 16 units: `gl_texture_unit_for_slot` used to drop all slot>=16 onto unit 9, so emissive, cascades 1..3, IBL's three (samplerCube), and tonemap's HDR/bloom/LUT/exposure all crowded onto one unit, and sampler2D and samplerCube collided directly -> every draw was rejected by WebGL. Fix: `gl_slot_map.h` tiers by `max_units`, 16 units use a compact table (forward's 14 samplers land in 0..15, postprocessing reuses 0..7, WebGL-trimmed samplers go to the 16+ dead zone) |
| **Mouse does not turn the view at all** (WASD moves, pointer lock log missing or locked with no response) | Two overlapping browser semantics: ① Emscripten's GLFW **never sets `GLFW_FOCUSED`** (`libglfw.js` does not wire focus/blur, the attribute is always 0), so `glfwGetWindowAttrib` always returns "unfocused" and the engine's "lock pointer only when focused" policy never triggers -- `Window::has_focus()` now asks `document.hasFocus()` under `__EMSCRIPTEN__`; ② during pointer lock the browser **pins** the cursor position (`clientX/Y` stops changing), while `libglfw.js`'s `calculateMouseCoords` only carries `clientX/Y`, so `glfwGetCursorPos` always returns the same value. Real displacement only exists in `movementX/Y`; the shell accumulates deltas on the canvas itself and `Window::take_pointer_delta()` takes them each frame (`glfwSetCursorPos` is a no-op in Emscripten, so recentering was never a path) |
| Object surfaces/ground covered in **fine diagonal stripes** (clearest under `--debug-view 5`, looks like a layer of texture on the image) | Shadow acne at the borderline: the PCF kernel rotates per pixel by `interleaved gradient noise` (4px period), so exactly self-occluded pixels flip per pixel at that period into stripes. Fix: `slope_bias()` overall x2 (keeping the `base*0.3` lower bound), with pbr/instanced/skinned_pbr/vulkan_*/deferred_lighting's seven copies kept in sync |
| D3D12 first run `shaderc failed: Macro redefined; different substitutions: GRYCE_HAS_PCSS` + `failed to load shader program 'pbr'` | D3D12's `d3d12_vulkanize` replays **all** `#define NAME value` in the source to the top of the file (not distinguishing `#if` branches). With the `#ifdef X / #define F 0 / #else / #define F 1 / #endif` two-branch pattern, the replay gives the same macro two substitution values -> shaderc errors out and the whole pbr stage fails. Rule: **one definition per macro repository-wide**, and branch-dependent switches become `#ifndef F / #define F 1 / #endif`, with 0 injected by the backend prefix (such as the ESSL conversion in `gl_shader.cpp`) |
