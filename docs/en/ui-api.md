# Built-in UI API (`ui-api`)

The engine compiles Dear ImGui into `GryceRenderer.dll` and exports the widget surface as plain C through `api/ui_api.h`. Hosts and extensions call `GUi_*` only and **never include `imgui.h`**. This page explains why, when calls are legal, and the coordinate conventions.

See also: [editor-api.md](editor-api.md) (viewport output and `GRender_SetUICallback`), [extension.md](extension.md) (extension ABI), [app-entry.md](app-entry.md) (host startup order).

## 1. Why a C layer is mandatory

ImGui's context, font atlas and scratch buffers are **process globals**, and they are per-DLL. `imgui.cpp` is built into `GryceRenderer.dll`; if an extension includes `imgui.h` itself:

* it either links its own copy of `imgui.cpp` — two unrelated `GImGui` contexts, `ImGui::Begin` feeds the extension's context while the engine's `Render()` walks its own, and nothing appears on screen;
* or it only gets the declarations and dies later at run time (`GImGui` is an `extern` thread-local pointer, so the symbol resolves fine while being null).

The first case shows up as **"drew everything, nothing on screen, no compile or link error"** — that is the whole reason this layer exists. The side benefit is version isolation: `ui_api.h` uses its own numbering (`GUI_WIN_*`, `GUI_TEXT_*`, `GUI_MOUSE_*`) and maps internally.

## 2. The two gates

Every function in `api/ui_api.cpp` passes two gates:

| Gate | Condition | Behaviour |
| --- | --- | --- |
| Compile time | `GRYCE_HAS_IMGUI` not defined (WebGL build, `-DGRYCE_ENABLE_IMGUI=OFF`) | The whole implementation degrades to empty functions; links fine, does nothing |
| Run time | Not inside the `GRender_SetUICallback` frame | Returns 0 / no-op |

The second gate is not defensive coding, it is required: hosts typically compute layout sizes inside `tick` ("how much width does the viewport still have"), and those calls happen at a **different stage** than real drawing. Without the gate they would push widgets into an `Idle` ImGui context and ImGui asserts out.

```c
/* Use this to ask "did anything draw"; do not infer it from return values */
int GUi_InFrame(void);

/* Display size in logical pixels */
void GUi_GetDisplaySize(float* out_w, float* out_h);
```

`GUi_InFrame()` is constantly 0 on headless runs (`--frames 1` in CI), so the same UI code runs in headless builds with no `#ifdef`.

## 3. What you must not call inside the callback

The `GRender_SetUICallback` callback runs on a path where `g_renderer.mutex` is **already held** (mid ImGui frame in `render_api.cpp`):

| Forbidden | Reason | Substitute |
| --- | --- | --- |
| `GRender_*` | Nearly all re-lock internally | Fetch in `tick`, pass the value in (texture id, gizmo operation, counters) |
| `GCore_*` | Command buffer and undo stack live under the lock | Record an intent, execute next frame |
| `GEdit_*` | Wrappers over `GCore_*` | Same |
| Mutating `GScene_*` | Same | Same |
| `GEntity_Get*` queries | The read path takes the lock too | Same, or cache once in the panel model |
| `GAsset_ScanDirectory` | Directory walk plus cache writes | Refresh in `tick`, UI reads the cache |

In one sentence: **inside the callback you only draw; the only permitted output is "record the user's action as a plain data structure"**.

### Intent queue

How the editor extension does it: the intent queue is a small vector of plain structs, and the next frame's `tick` calls `flush()` outside the lock:

```cpp
/* Inside the callback (draw.cpp) */
if (GUi_IsItemClicked(GUI_MOUSE_LEFT))
    push_select(handle);          /* just appends to std::vector<Intent> */

/* Next frame, start of tick, outside the lock (ui.cpp) */
intent::flush();                  /* actually calls commands::select() / view::set_size() */
```

The cost is a **one-frame delay** (click to effect); the payoff is that the callback can never deadlock. `flush()` swaps the queue out before executing, because executing may enqueue more (e.g. "after selecting, request a bounding box"); you cannot iterate while appending.

Selection intents (`kSelect` / `kToggleSelect`) collapse within a frame: rubber-banding ten objects leaves one entry, so the undo stack does not grow by ten.

## 4. Widget surface

### Containers

```c
GUi_BeginWindow(name, x, y, w, h, flags);   /* flags: GUI_WIN_* */
GUi_BeginChild(id, w, h, border);           /* w/h = 0 fills the remainder */
GUi_BeginMenuBar(); GUi_BeginMenu(label); GUi_MenuItem(label, shortcut, enabled);
```

`GUI_WIN_NO_SAVED_SETTINGS` stops ImGui from writing `imgui.ini`. The editor always sets it — once layout is persisted, screenshot comparison and integration tests stop being reproducible.

### Widgets

| Category | Functions |
| --- | --- |
| Text | `GUi_Text` / `GUi_TextColored(color, text)` / `GUi_LabelText` / `GUi_Separator` / `GUi_Spacing` / `GUi_SameLine(spacing)` |
| Buttons | `GUi_Button` / `GUi_ButtonSize` / `GUi_SmallButton` / `GUi_Selectable(label, selected)` / `GUi_SetItemAllowOverlap` |
| Values | `GUi_Checkbox` / `GUi_SliderFloat` / `GUi_DragFloat` / `GUi_DragFloat2` / `GUi_DragFloat3` / `GUi_DragFloat4` / `GUi_ColorEdit4` |
| Text input | `GUi_InputText` / `GUi_SetKeyboardFocusHere` / `GUi_IsItemDeactivatedAfterEdit` |
| Enums | `GUi_Combo(label, current, items, count)` |
| Trees | `GUi_TreeNode(label, default_open)` + `GUi_TreePop` / `GUi_CollapsingHeader` |
| Images | `GUi_Image(tex, w, h)` |
| Popups | `GUi_OpenPopupOnItemClick` / `GUi_OpenPopup` / `GUi_BeginPopup` / `GUi_EndPopup` / `GUi_CloseCurrentPopup` |
| Drag and drop | `GUi_BeginDragDropSource` / `GUi_SetDragDropPayloadString` / `GUi_EndDragDropSource` / `GUi_BeginDragDropTarget` / `GUi_AcceptDragDropPayloadString` / `GUi_EndDragDropTarget` |
| Layout | `GUi_SetCursorPos` / `GUi_GetCursorPos` / `GUi_GetContentAvail` / `GUi_SetNextItemWidth` / `GUi_GetTextLineHeight` / `GUi_GetFrameHeight` / `GUi_CalcTextWidth` |
| Hit testing | `GUi_LastItemRect` / `GUi_IsItemHovered` / `GUi_IsItemClicked` / `GUi_IsItemActive` / `GUi_IsAnyItemActive` |
| Mouse | `GUi_MousePos` / `GUi_MouseDeltaX` / `GUi_MouseDeltaY` / `GUi_IsMouseDown` / `GUi_IsMouseClicked` / `GUi_IsMouseReleased` / `GUi_GetModifiers` |
| Capture | `GUi_WantCaptureMouse` / `GUi_WantCaptureKeyboard` |
| Style | `GUi_PushTextStyle(kind)` / `GUi_PopTextStyle` |

Return convention: `int` widgets return 1 when clicked / edited this frame, 0 otherwise. In-place pointer arguments (`float*`, `char*`, `int*`) already hold the new value when 1 is returned.

`GUi_Selectable` is for "whole row clickable and shows a selected state" (hierarchy tree, asset list). A row that also needs a context menu must call `GUi_SetItemAllowOverlap()` right after it, otherwise clicks get stolen by the neighbouring row's hit rect.

### Splitting width between label and widget

ImGui draws the label to the **right** of the widget. So `GUi_SetNextItemWidth(-1)` makes the widget eat the whole row and pushes the label outside the panel (leaving half a character on screen). Subtract the label width with `GUi_CalcTextWidth`:

```c
float avail = 0.0f, unused = 0.0f;
GUi_GetContentAvail(&avail, &unused);
float reserve = GUi_CalcTextWidth("Position") + 14.0f;
if (reserve > avail * 0.6f) reserve = avail * 0.6f;   /* long labels get 60% at most */
GUi_SetNextItemWidth(-reserve);
GUi_DragFloat3("Position", v, 0.02f);
```

Only widgets without a visible label (`"##name"`) should use `-1`.

### Drag and drop

Payloads are strings only. Everything an editor needs to move around (entity handles, asset paths) already has a textual form, and text buys two things: no memory-layout dependency across the DLL boundary, and a tooltip that actually shows the content while dragging.

```c
/* Source: right after the item */
if (GUi_BeginDragDropSource()) {
    char id[24];
    std::snprintf(id, sizeof(id), "%lld", (long long)handle);
    GUi_SetDragDropPayloadString("GRYCE_ENTITY", id);
    GUi_EndDragDropSource();
}

/* Target: also "the previous item" */
if (GUi_BeginDragDropTarget()) {
    char buf[24];
    int r = GUi_AcceptDragDropPayloadString("GRYCE_ENTITY", buf, sizeof(buf));
    if (r == GUI_DRAG_DELIVER) { /* released, buf is valid */ }
    GUi_EndDragDropTarget();
}
```

`GUi_AcceptDragDropPayloadString` returns three states, and the distinction matters:

| Value | Meaning | Typical use |
| --- | --- | --- |
| `GUI_DRAG_NONE` | No matching payload hovering | Do nothing |
| `GUI_DRAG_PREVIEW` | Hovering, not released yet | Draw highlight, change hint text |
| `GUI_DRAG_DELIVER` | Released this frame, `out_buf` filled | Perform the action |

Acting only on `DELIVER` is enough for correctness; `PREVIEW` exists so the target can say "you can drop here" while hovering. The mouse button is always the left button — ImGui has no switch for dragging with another one.

### Modifiers

```c
const int mods = GUi_GetModifiers();      /* GUI_MOD_CTRL / SHIFT / ALT / SUPER */
if (mods & GUI_MOD_SHIFT)  ...            /* add to selection */
else if (mods & GUI_MOD_CTRL) ...         /* toggle */
else ...                                   /* replace selection */
```

The hierarchy tree uses this to distinguish replace / add / toggle, matching what most editors do.


## 5. The viewport image and coordinates

The viewport is an ImGui image, but **how large it actually is** is only known after drawing (window resize, panel drag). `GUi_LastItemRect` is the only reliable source:

```c
GUi_Image(tex, avail_w, avail_h);
float x, y, w, h;
GUi_LastItemRect(&x, &y, &w, &h);   /* absolute coords, top-left origin */
```

Feed the rect back to the engine or two things drift apart:

```c
/* Executed in tick, outside the lock */
GViewport_SetScreenOrigin(x, y);    /* offset for gizmo screen projection */
GViewport_SetSize((int)w, (int)h);  /* divisor for viewport-local coords */
```

* **Gizmo** goes through `screen_project`, which converts NDC to pixels and adds `(origin_x, origin_y)`. Skip it and the gizmo draws in the window's top-left corner, floating over the hierarchy tree.
* **Picking** (`screen_ray`) uses viewport-**local** coordinates, i.e. `mouse_abs - origin`; it does not need `SetScreenOrigin`.
* Mixing the two produces "what I see is in the middle, what I click is below middle".

Also call `GRender_InvalidateImGuiTexture(tex)` on resize: the old `ImTextureID` points at a released texture once the target is rebuilt.

### Texture orientation

`GUi_Image` decides on its own whether to flip V, and callers **must not** pass UVs themselves (the API does not accept them). The reason is that "the first row of the texture" means different things per backend:

| Backend | First row of a render target | Flip V |
|---|---|---|
| OpenGL | the image's **bottom** row (GL origin is bottom-left) | yes |
| D3D12 / Vulkan | the image's **top** row | no |

ImGui's `Image()` defaults to UV `(0,0)-(1,1)`, meaning "row 0 of the data is at v=0". A GL render target's row 0 is the bottom row, so sampling it as-is turns the picture upside down — the symptom is the ground flying into the sky and the lit top face of a cube appearing along its bottom edge, while all panel text and icons look perfectly normal (glyphs come from the font atlas, which has nothing to do with render targets).

So "the picture is inverted but the text is fine" is almost always this, never the camera or the projection. The engine marks the current backend with `IImGuiBackend::texture_needs_v_flip()` and caches it right after `GRender_Init` builds the ImGui backend; drawing only reads the cached flag — UI callbacks run with the renderer lock held, so asking the renderer from inside a callback would deadlock on a second lock.

## 6. Layout convention

`ui_api.h` deliberately offers **no docking**. Tool-style interfaces that need several panels use one main window with fixed partitions:

```
menu bar
toolbar
├─ hierarchy 236 px ─┬─ viewport (fills) ─┬─ inspector 306 px ─┤
└──────────────── asset list / stats 168 px ───────────────────┘
```

Docking state is persisted in `imgui.ini` and drifts with user drags, which makes screenshot comparison and script assertions unreproducible. Fixed partitions also guarantee the viewport has a deterministic rect at any window size, which is what makes assertions possible.

## 7. Continuous edits and undo grouping

Dragging a slider emits "value changed" for dozens of frames, but that should be **one** undo step. The test is whether a widget is still active:

```c
/* Editing a number in the inspector */
if (GUi_IsAnyItemActive()) {
    if (!g_edit_open) { GCore_BeginEdit("Inspector"); g_edit_open = true; }
    GEdit_SetProperty(e, type, name, &v, sizeof(v));
} else if (g_edit_open) {
    GCore_EndEdit();
    g_edit_open = false;
}
```

While `GUi_IsAnyItemActive()` is true you must **not** close the group — closing it means the next drag opens a fresh one and a single drag becomes dozens of steps. Note it also returns true while a text field has focus, so "typing a new name" collapses into one step too, which is what you want.

### Drag widgets must keep their own copy of the value

The engine only sees a change on the **next** frame (writes go through the command queue). If you re-initialise a `DragFloat` from the panel model every frame, the value snaps back to the previous frame's number while dragging — the feel is "drag, and it bounces back".

```c
/* Sync only when the target changed or nothing is being edited */
if (changed_target || !GUi_IsAnyItemActive()) {
    buf[0] = model_value;
}
if (GUi_DragFloat("Speed", buf, 0.01f)) { push(Intent{...}); }
```

The buffer must survive across frames (keep it in the panel state struct), not be a stack local.

## 7.5 The op queue and controlled execution

UI does not need the renderer's frame rate, so "the UI wants to change engine state" is never executed on the spot: the callback only enqueues, and the host drains under a policy outside the lock.

```c
/* Inside the callback (lock held, plain data only) */
GUiOp op = { 0 };
op.type = UI_OP_SET_TRANSFORM;
op.key  = entity_handle;      /* same (type,key) keeps only the newest value */
op.vec[0] = x; op.vec[1] = y; op.vec[2] = z;
GUi_PushOp(&op);              /* called every frame while dragging; collapses to one */

/* Host tick (outside the lock) — only drained ops may become GEdit_* / GCore_* */
GUiOp out[32];
int n = GUi_DrainOps(out, 32);
for (int i = 0; i < n; ++i) apply(out[i]);   /* apply() may call engine APIs freely */
```

**Coalescing**: `key == 0` is a discrete op (click, create, delete) and never merges; `key != 0` is a continuous edit (a transform being dragged) and keeps only the newest value while queued — a thirty-frame drag leaves one entry, i.e. one undo step. Merging is an **in-place overwrite**: FIFO position and `seq` stay put.

**Controlled execution** comes from the policy (`GUi_SetOpPolicy`):

| Parameter | Meaning |
| --- | --- |
| `max_per_drain` | Cap per drain; `<=0` = unlimited. UI ops land in batches instead of tracking the frame rate |
| `min_frames_between_drain` | Minimum frames between drains; `<=0` = every advanced frame may drain. The frame clock is advanced by `GUi_AdvanceOpFrame()` (the render layer calls it every frame; hosts with their own loop call it themselves) |

**Full-queue semantics**: capacity is fixed at 256. When full, only the **oldest coalescing op** is evicted (a drag re-pushes the same key every frame, so losing an intermediate value loses nothing final); discrete ops are never evicted, and if nothing is evictable the push is rejected and returns 0 (counted in `stats.rejected`) — "the consumer is not draining" must be visible, never silently swallowed. Read the counters with `GUi_GetOpQueueStats`; `evicted` / `rejected` / `throttled` exist for exactly this diagnosis.

**UI frame throttling** (`GRender_SetUiFrameInterval(n)`) is the other half of the same idea: the ImGui frame (callback plus gizmo) is rebuilt every `n`th render frame, and in-between frames simply redraw a deep copy of the last draw data — the UI stays on screen, its contents just update at the interval, and input is sampled at the same rate. `n <= 1` is the default (rebuild every frame). The demo host exposes it as `--ui-interval N`.

Verified end to end with the demo host: at `--frames 30 --ui-interval 3` the screenshot lands on frame 30, a cached-redraw frame, and its statistics are byte-identical to the every-frame-build run (and clearly different from the no-UI run) — the throttled frame draws exactly what a rebuilt frame draws.

## 8. Sharing input with the host

When the UI eats mouse/keyboard, host navigation and shortcuts must yield:

```c
/* Start a viewport drag only when the viewport is hovered and the UI did not take the mouse */
if (!GUi_WantCaptureMouse() && GUi_IsItemHovered()) { ... }

/* Guard shortcut dispatch: otherwise typing "Wire" in a name field also switches tool mid-word */
if (GUi_WantCaptureKeyboard()) return;   /* dispatch nothing */
```

Missing the keyboard guard looks like: renaming an entity, pressing `W` starts translating the object, pressing `R` switches to the rotate tool and eats a character from the name.

## 9. Known limits

* No docking, no multi-window docking, no `ImPlot` or node-editor style add-ons — add a new `GUi_*` function for those instead of pulling ImGui into the extension.
* Drag/drop payloads are a single string: no multi-selection dragging, no custom payload types, and no cross-process or cross-window dragging.
* One dark theme plus semantic text colours (`GUI_TEXT_*`). To restyle, change the engine's `debug_imgui` style; do not hardcode colours in callers.
* `GUi_Image` takes no custom UVs or border tinting; the viewport shows the full texture.
* Non-ASCII text depends on the font atlas the engine loaded; missing glyphs render as boxes rather than crashing.
