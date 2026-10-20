# Editor API

This page collects the interfaces between an **external editor process** and the engine: edit commands, undo/redo, viewport output and the ImGui bridge, dual viewports, the transform gizmo, display modes, and asset sidecar files. All of them are C ABI — a header and the import library are enough, no internal C++ types involved.

Nothing here needs new low-level engine work: this page documents **capabilities that are already in place**.

Widgets (`GUi_*`) have their own page: [ui-api.md](ui-api.md).

## 1. Edit commands (`api/edit_api.h`)

Every edit goes through one channel: `GCore_PushCommand` → command buffer → consumed inside `GCore_BeginFrame` (up to 512 commands and a 4 ms budget per frame; anything left over is requeued for the next frame). The editor therefore never mutates the scene directly.

```c
// 1 = queued, 0 = rejected. The return value only says "queued", not "applied".
GEdit_CreateEntity("Wall", 0);          /* 0 = scene root */
GEdit_SetTransform(handle, &pos, NULL, NULL);
```

`edit_api.h` also publishes each command's payload layout (`GCmdCreateEntity` … `GCmdGizmoManipulate`) with `static_assert(sizeof(...) <= 256)` compile-time guards. These structs used to live only inside `core_api.cpp`, leaving integrators to copy the byte layout by hand — one field change on the engine side and the editor silently reads garbage.

| Command | Wrapper | Notes |
| --- | --- | --- |
| `ECMD_CREATE_ENTITY` | `GEdit_CreateEntity(name, parent)` | Creates and **selects** the entity (`GEntity_GetSelected` returns it) |
| `ECMD_DESTROY_ENTITY` | `GEdit_DestroyEntity(e)` | Destroys the whole subtree |
| `ECMD_RENAME_ENTITY` | `GEdit_RenameEntity(e, name)` | |
| `ECMD_REPARENT_ENTITY` | `GEdit_ReparentEntity(e, parent)` | Safe reparent; never destroys the moved entity |
| `ECMD_SELECT_ENTITY` | `GEdit_SelectEntity(e)` | Fires `OnEntitySelected` |
| `ECMD_SET_TRANSFORM` | `GEdit_SetTransform(e, pos, rot, scale)` | `NULL` components keep their **current** value; an all-zero `rot` means "keep rotation" |
| `ECMD_TRANSLATE_ENTITY` | `GEdit_TranslateEntity(e, delta)` | Local-space delta; the current value is read **at execution time**, so several can stack in one frame |
| `ECMD_SET_PROPERTY` | `GEdit_SetProperty(e, type_hash, name, value, size)` | `value` is the raw reflected field bytes |
| `ECMD_ADD_COMPONENT` | `GEdit_AddComponent(e, type_name)` | Fully qualified type name |
| `ECMD_REMOVE_COMPONENT` | `GEdit_RemoveComponent(e, type_hash)` | |
| `ECMD_SET_MATERIAL` | `GEdit_SetMaterial(e, mesh, r, g, b, rough, metal)` | Adds a `MeshRenderer` when missing |
| `ECMD_SET_RENDER_TARGET` | `GEdit_SetRenderTargetSize(target, w, h)` | `target`: 0 = SceneView, 1 = GameView |
| `ECMD_SAVE_SCENE` | `GEdit_SaveScene(path)` | |
| `ECMD_IMPORT_ASSET` | `GEdit_ImportAsset(path)` | Entity + `MeshRenderer` + selection |
| `ECMD_PLAY_MODE` | `GEdit_SetPlayMode(mode)` | 0 = stop, 1 = play, 2 = toggle pause |
| `ECMD_STEP_FRAME` | `GEdit_StepFrame(frames)` | Advances frames while playing and paused |

String fields are fixed-size arrays; over-long input is truncated and always `'\0'`-terminated.

### Several transforms in one frame must use the delta variant

`GEdit_SetTransform` fills its omitted components with the *current* value **at queue time**.
So calling `SetTransform(pos1)` and then translating in the same frame reads the old position
for the second call, produces an absolute value, and silently overwrites the first one — only
one of the two edits survives.

```c
// Wrong: the final position is old + delta, pos1 is lost
GEdit_SetTransform(e, &pos1, NULL, NULL);
GEdit_TranslateEntity(e, &delta);       // would be wrong if done as SetTransform(old + delta)

// Right: the delta command reads the position when it executes, so both apply in order
GEdit_SetTransform(e, &pos1, NULL, NULL);
GEdit_TranslateEntity(e, &delta);       // => pos1 + delta
```

Two Inspector fields edited back to back, or a gizmo drag followed by a snap correction,
both hit this.

### Two return conventions — do not mix them

| Family | Convention | Example |
| --- | --- | --- |
| Command queueing (`GEdit_*`) | 1 = queued, 0 = bad argument / full queue | `GEdit_CreateEntity` |
| Queries and scene ops (`GEntity_Get*`, `GScene_*`) | 0 = success, -1 = failure | `GEntity_GetLocalPosition`, `GScene_Save` |

`GEntity_GetName` is the exception: it returns the **name length**; only `len < 0` means failure.

## 2. Undo / redo (`GCore_Undo` family)

Undo uses a **whole-scene JSON snapshot**, not an incremental diff. The serialization path already has to be correct for saving, and restoring keeps entity handles by UUID (`EntityHandleMap::rebuild` preserves the existing mapping), so the editor's selection and panel state survive a rollback.

```c
GCore_Undo();               /* 1 = performed, 0 = nothing to undo */
GCore_Redo();
GCore_GetUndoCount();
GCore_GetRedoCount();
GCore_SetUndoLimit(64);     /* 0 = disable recording */
GCore_ClearUndoHistory();

GCore_BeginEdit("Drag");    /* a drag gesture counts as one step */
GCore_EndEdit();
```

Semantics:

* A snapshot is captured before every **mutating** command; inside `BeginEdit` only one snapshot is taken and pushed on `EndEdit`.
* The stack freezes in Play mode — runtime physics/animation changes do not belong in edit history.
  **The host defaults to runtime semantics (enters Play on startup)**, so an editor project
  must set `"start_in_play": false` in `project.data` or pass `--edit-mode`; forgetting this
  shows up as "edits apply, but undo is always greyed out and the scene never turns dirty"
  (see [app-entry.md](app-entry.md) section 6).
* `GScene_Load`, `GScene_New` and `cmd_load_scene` all clear the history: snapshots hold UUIDs that do not exist in the new scene.
* Default depth 64; the oldest entries are dropped first.

## 3. Viewport output and the ImGui bridge (`api/render_api.h`)

Drawing the 3D view inside an editor panel needs three things: the offscreen colour texture, its ImGui descriptor, and ownership of the ImGui context.

```c
GRender_SetViewportOutput(true);                  /* also honoured before init */
GTextureHandle tex = GRender_GetViewportTexture();/* post-tonemap LDR result */
ImTextureID id = GRender_GetImGuiTextureId(tex);  /* implemented for all backends */
GRender_InvalidateImGuiTexture(tex);              /* required after target resize */
```

`GTextureHandle` is just `ITexture*`, so the handle can be fed straight into `IImGuiBackend::imgui_texture_id` on any backend.

ImGui ownership: the engine only takes over when `ImGui::GetCurrentContext() == nullptr` (guarded by `debug_ui_enabled || ui_callback`). A host that owns the context uses the callback instead and lets the engine draw inside the host's ImGui frame:

```c
GRender_SetUICallback(my_draw_ui, user_ptr);
GRender_HasImGui();          /* is a usable context actually available */
```

Inside the callback you cannot call `GRender_*` (the lock is held); what you can use is `GUi_*` from `api/ui_api.h`, see [ui-api.md](ui-api.md).

### Editor camera: pass a quaternion, not yaw/pitch

```c
GViewport_SetEditorCamera(px, py, pz, yaw_deg, pitch_deg, fov, near, far);
GViewport_SetEditorTransform(px, py, pz, qx, qy, qz, qw, fov, near, far);   /* preferred */
```

`SetEditorCamera` follows the engine's `math::Camera` convention: `forward = (cos yaw, sin pitch, sin yaw)`. Editors usually assume `forward = (-sin yaw, sin pitch, -cos yaw)`. Both are self-consistent, and passing one into the other offsets the image and the picking ray by 90° — which is nearly impossible to tell apart from "the pitch sign is flipped" on a single screenshot.

`GViewport_SetEditorTransform` takes a quaternion whose **rotation is applied to the camera's default -Z**, exactly the same derivation as the scene `Camera` component and `build_scene_camera`. A host that computes its picking ray from the same quaternion is guaranteed to match what is rendered, with no conversion formula to get wrong.

```c
/* Editor construction order: yaw first, then pitch */
Quat q = quat_mul(quat_axis_angle(0, 1, 0, yaw), quat_axis_angle(1, 0, 0, pitch));
GViewport_SetEditorTransform(pos, q.x, q.y, q.z, q.w, fov, 0.1f, 1000.0f);

/* Picking ray: rotate -Z by the same q, do not recompute from yaw/pitch */
Vec3 forward = rotate_by_quat(q, { 0, 0, -1 });
```

## 4. Dual viewports (SceneView / GameView)

Both views share a single HDR / MSAA / post-process chain — the scene is rendered once and `tonemap` picks the destination via `set_game_view_active`. The upside is not duplicating the chain; the cost is that **GameView resolution follows SceneView** (a mismatch is rejected and logged).

```c
GRender_SetGameViewCamera(camera_entity);   /* falls back to the scene main camera */
GRender_RenderGameView();                   /* tonemaps into the GameView target */
GTextureHandle game = GRender_GetGameViewTexture();
```

### Suggested frame order

```
PollEvents → input sync → GCore_BeginFrame(dt)
          → GRender_BeginFrame()
          → GRender_RenderWorld()        /* SceneView (editor camera) */
          → GRender_RenderGameView()     /* GameView (scene camera) */
          → GRender_RenderGizmo()        /* actual drawing happens in the ImGui frame */
          → GRender_EndFrame()
          → GCore_EndFrame()
```

## 5. Transform gizmo

The gizmo draws in **screen space** through the ImGui foreground draw list rather than going through the 3D pipeline: zero cost on all backends, always on top, no depth occlusion to fight.

```c
GRender_SetGizmoEnabled(true);
GRender_SetGizmoMode(0);            /* 0 = world, 1 = local */
GRender_SetGizmoOperation(0);       /* 0 = translate, 1 = rotate, 2 = scale */

int axis = GRender_GizmoHitTest(mouse_x, mouse_y);   /* >=0 = axis hit, -1 = miss */
GRender_BeginDrag(axis, mouse_x, mouse_y);
GRender_Drag(mouse_x, mouse_y);     /* per frame; converted to a world delta */
GRender_EndDrag();

GRender_DrawGizmoOverlay();         /* auto-called by EndFrame when the engine owns ImGui */
```

Route the resulting delta through `GCore_BeginEdit` + `GEdit_SetTransform` instead of touching `Transform` directly, otherwise the undo stack never sees the change.

Pick radius is 9 px, axis length 92 px; axes are red/green/blue, with the hovered or dragged axis highlighted in yellow.

### Set the screen origin whenever the viewport is not the whole window

The gizmo draws on ImGui's foreground draw list, which uses **window-absolute** coordinates, while the viewport texture only knows its own `vw × vh`. `screen_project` therefore maps NDC into `[0,vw] × [0,vh]` — with a panel layout, the gizmo lands in the window's top-left corner, floating above the hierarchy tree.

```c
GViewport_SetSize((int)w, (int)h);   /* viewport interior: the NDC-to-pixel divisor */
GViewport_SetScreenOrigin(x, y);     /* viewport's top-left in the window: pixel offset */
GViewport_GetScreenOrigin(&x, &y);
```

Contract:

| Entry point | Coordinate space |
| --- | --- |
| `GRender_GizmoHitTest(mx, my)` | **window-absolute** (once an origin is set) |
| `GRender_BeginDrag` / `GRender_Drag(mx, my)` | same |
| `GScene_PickRay(ox, oy, ...)` and editor-side `screen_ray` | **viewport-local** = `mouse_abs - origin` |

The default origin is `(0,0)`, which means "viewport covers the window" and reproduces the old behaviour exactly — intentional, so plain 3D demos need no changes.

Sync the origin every frame in `tick` from the **actual drawn rect** (`GUi_LastItemRect`), not from the requested size: on window resize the two differ by a pixel or two, the gizmo drifts, and the symptom is "picking error grows toward the bottom-right".


## 6. Display modes

```c
GRender_SetDisplayMode("Wireframe");
GRender_IsDisplayModeSupported("Wireframe");   /* 1 = this backend can really render it */
```

Wireframe only covers the **geometry passes** (GBuffer / main forward pass), never full-screen post-processing — a full-screen quad turned into wireframe leaves nothing but two diagonals on screen.

| Backend | Wireframe |
| --- | --- |
| OpenGL (desktop) | Supported (`glPolygonMode`) |
| WebGL | Not supported (GLES3 has no such entry point) |
| Vulkan | Not supported (needs `VK_EXT_extended_dynamic_state3`) |
| D3D12 | Not supported (needs a wireframe PSO per shader) |

`GRender_IsDisplayModeSupported` exists so the editor can **disable** the menu item instead of offering a switch that does nothing.

## 7. Asset sidecar files (`<asset>.meta`)

The editor needs an asset identity that is independent of its path: hierarchy references, material slots and scene fragments point at a `guid`, so renames and moves never break a link. The sidecar also stores import settings that are applied on the next import.

```c
GAssetMetaInfo info;
GAsset_EnsureMeta("res:/models/wall.fbx", &info);   /* creates and writes when missing */
info.import_materials = 0;
GAsset_WriteMeta("res:/models/wall.fbx", &info);    /* writes and invalidates cached meshes */
GAsset_IsMetaStale("res:/models/wall.fbx");         /* source changed → offer reimport */
```

The sidecar lives next to the source as `<source-name>.meta` (original extension kept, so `texture.png.meta` and `texture.meta` cannot collide), JSON, UTF-8. Fields:

| Field | Default | Purpose |
| --- | --- | --- |
| `guid` | generated | 32 hex chars, assigned on first import and never changed |
| `importer` | `mesh` | selects which settings apply |
| `scale` | 1.0 | source units → metres (centimetre photogrammetry uses 0.01) |
| `up_axis` | 1 | 0 = X-up, 1 = Y-up, 2 = Z-up (Z-up is rotated -90° about X) |
| `generate_lod` | true | build the LOD chain on import |
| `import_materials` | true | false = clay model, ignore file materials |
| `roughness_scale` | 1.0 | multiplier on top of the derived roughness |
| `source_mtime_ns` / `source_size` | recorded | source fingerprint for staleness |

Implementation notes:

* Missing fields fall back to defaults and unknown fields are ignored — a sidecar written by a newer editor must not break an older engine.
* Invalid settings (`scale <= 0`, out-of-range `up_axis`) fall back to defaults, but the **guid is kept**: identity must not be damaged by a settings field.
* Read-only directories and assets inside `.gpack` bundles are not written to (in-memory result only) — a normal degradation.
* Scaling and up-axis conversion live in `apply_import_options`, shared by the built-in OBJ loader and the Assimp path so OBJ and FBX do not end up misaligned in the same project.
* `GRYCE_NO_ASSET_META=1` disables the whole feature (runtime-only builds, read-only asset folders).

### Directory listing (`GAsset_ScanDirectory`)

The asset browser needs to list a folder, but so do packaging manifests, hot-update validation and
CI asset checks — which makes this an engine capability rather than an editor private:

```c
char buf[8192];
int n = GAsset_ScanDirectory("res:/models", /*recursive=*/0, buf, sizeof(buf));
if (n >= (int)sizeof(buf)) {            // too small: n is the required length, retry bigger
    std::vector<char> big(n + 1);
    GAsset_ScanDirectory("res:/models", 0, big.data(), n + 1);
}
```

Output is single-line JSON: `{"dir","count","truncated","items":[{"path","name","ext","size","guid","has_meta","stale","kind"}]}`.

* Filesystem walk only; **nothing is loaded** — probing each path with `GAsset_Load` is both slow and unable to tell "missing" from "unsupported format".
* `.meta` sidecars are not assets and are skipped.
* Results are sorted by `path`, so repeated listings of the same folder keep the same order (no UI jitter).
* `kind` (mesh / texture / audio / scene / script / shader / other) is a grouping hint only; it does not mean the file can be imported.
* Return value: bytes written; when the buffer is too small it returns the **required** length (greater than `buf_size`); -1 on failure.

### Visitor form (`GAsset_ScanDirectoryEach`)

The asset panel needs both JSON (script side, `panel_assets_json()`) and direct structs (the UI needs them every frame; parsing a string you just generated is slow and adds a "parse failed → empty list" failure mode). Both versions share one implementation, so traversal order, exclusion rules and sidecar fields are identical:

```c
typedef struct GAssetEntry {
    const char* path;  const char* name;  const char* ext;
    const char* kind;  const char* guid;
    unsigned long long size;
    int has_meta;      int stale;
} GAssetEntry;

/* Return non-zero to stop early; the function returns the number visited */
int cb(void* user, const GAssetEntry* e) { /* push to vector */ return 1; }
GAsset_ScanDirectoryEach("res:/models", /*recursive=*/0, cb, &my_vector);
```

The string pointers in `GAssetEntry` are **only valid during the callback** (they point into the walk's scratch buffer); copy anything you keep. This avoids one heap allocation per entry, which matters noticeably with thousands of files.

## 8. Known limits

* Undo is a whole-scene snapshot: per-step cost grows with scene size (hundreds of microseconds on the 1.4 M triangle demo scene), so do not set the depth very high.
* GameView resolution follows SceneView; independent resolutions would require duplicating the entire HDR chain.
* Wireframe is OpenGL-desktop only; other backends report unsupported.
* The gizmo draws axes and gauges only — no multi-selection merge and no snapping (the editor rounds values before calling `GEdit_SetTransform`).
* `screen_project` does perspective projection plus the origin offset only; it knows nothing about ImGui clip rects, so a scrolled or partially occluded viewport still projects outside its rect and relies on the clip rect to hide it.
* The widget layer (`GUi_*`) is covered in [ui-api.md](ui-api.md): no docking, a single dark theme, and non-ASCII glyphs depend on the font atlas the engine loaded.
