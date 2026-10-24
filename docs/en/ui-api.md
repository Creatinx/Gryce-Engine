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
| Values | `GUi_Checkbox` / `GUi_SliderFloat` / `GUi_DragFloat` / `GUi_DragFloat2` / `GUi_DragFloat3` / `GUi_DragFloat4` / `GUi_ColorEdit4` / `GUi_ColorButton(label, rgba)` |
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

## 7.6 The model layer (`engine/ui/`) and the demo host

`GUi_*` are **draw primitives**; everything that has nothing to do with "where it draws or how it looks" — cross-frame state, rectangle math, styling, input gating — lives in `engine/ui/` as **pure logic with no ImGui dependency**. The same sources compile in headless CI and in builds with ImGui disabled.

| File | Responsibility | Key rules |
| --- | --- | --- |
| `ui_id.h` | `(label, seed)` → 64-bit stable id | Never 0 (0 marks an empty state slot); collisions are not handled |
| `ui_state.h` | Cross-frame widget state store (open addressing + linear probing, deletion by backward shift) | The frame number doubles as the aging clock; entries unreferenced for more than `kMaxAgeFrames` (~900) are reclaimed. Under UI throttling a visible widget is referenced every build frame, so it is never falsely aged out |
| `ui_layout.h` | Deterministic layout stack: same call sequence → same rects | `end_row` advances by `max(row height, tallest cell)` — an explicit `cell(0, 30)` must not be covered by the next row; never mix with ImGui auto-layout on the same region |
| `ui_theme.h` | Style tokens (semantic colors, purpose-named metrics) | `apply()` is a no-op without an ImGui context; switching themes = `GUi_ThemeSetPreset` + `GUi_ThemeApply` |
| `ui_input.h` | Input gating and the shortcut table | Modifiers must match exactly; conflicts are reported at registration time, not when the user presses the key |
| `ui_snapshot.h` | Panel view model (serializable pure data) | 64-bit ids go through JSON as **decimal strings** (`JSON.parse` has 53-bit precision); reads are tolerant — missing fields default, bad rows are skipped |
| `ui_component.h` | Component tree layout (retained mode) | The only positioning rule is "is the parent a layout component"; ids increase monotonically and are never reused; `resolve()` is idempotent |

The C surface is already open in `ui_api.h`: `GUi_MakeId` / `GUi_State*` / `GUi_Layout*` / `GUi_Theme*` / `GUi_ShouldDispatchShortcut` / `GUi_Shortcut*`. The usage pattern:

```c
uint64_t id = GUi_MakeId("Opacity", entity_uuid);   // same id across frames
float v = 0.5f;
GUi_StateGetFloat(id, &v);                           // read (default if absent)
if (GUi_SliderFloat("Opacity", &v, 0, 1, "%.2f")) {  // draw
    GUi_StateSetFloat(id, v);                        // write back (engine-owned, not a stack buffer)
}
```

`GryceUiSmoke.exe` (`test/ui_smoke.cpp`) is the end-to-end demo for all of this. It is not part of the gate and needs a window:

```bash
build/GryceUiSmoke.exe                    # dark theme, draws one demo panel
build/GryceUiSmoke.exe --theme light      # the light variant of the same token table
build/GryceUiSmoke.exe --ops-burst 200    # push 200 ops: coalesced≈99, drained=101 in the stats
build/GryceUiSmoke.exe --ui-interval 3    # throttled screenshot is byte-identical to per-frame builds (same md5)
```

`--ops-burst` pushes half discrete (`key=0`) and half continuous (`key=1`) ops: the continuous ones coalesce into a single entry (only the last value lands) while the discrete ones land in per-frame batches — the full closed loop of "low UI refresh rate, controlled execution".

## 7.7 Component tree layout (retained mode)

`GUi_Layout*` is the **immediate-mode** rectangle stack (same call sequence → same rects). The component tree is the **retained-mode** half: build the tree once, then only mutate parameters and call `GUi_CompResolve()` each frame to recompute rects. It suits panels whose structure is stable while their contents follow state.

```c
GUiCompSpec root{};                       // parent=0 = no parent
root.kind = GUI_COMP_KIND_COLUMN;
root.x = 460; root.y = 24; root.w = 320; root.h = 480;   // a root must set pos/size
root.gap = 6;
for (int i = 0; i < 4; ++i) root.padding[i] = 8;         // top right bottom left
uint64_t col = GUi_CompAdd(&root);        // the return value is the component id (never 0)

GUiCompSpec row{};
row.parent = col; row.kind = GUI_COMP_KIND_ROW;
row.align = GUI_COMP_ALIGN_STRETCH;       // fill the cross axis
uint64_t head = GUi_CompAdd(&row);        // x/y/w/h are filled by the layout; 0 is fine

GUiCompSpec leaf{};
leaf.parent = head; leaf.kind = GUI_COMP_KIND_LEAF;
leaf.flex = 1.0f;                         // share the main-axis free space by flex weight
GUi_CompAdd(&leaf);

GUi_CompResolve();                        // idempotent: call once per frame
GRUiRect r;
GUi_CompRect(leaf, &r);                   // read the resolved rect
```

**There is exactly one positioning rule: is the parent a layout component.**

* Parent is `Column` / `Row` / `Grid` → the children's `x/y/w/h` are arranged by the layout (caller-written values are ignored; passing 0 is fine).
* Parent is a `Leaf`, or there is no parent → children **keep the pos the caller wrote**, untouched by the layout. So flipping a component from leaf to `Column` immediately inverts the positioning of all its children — that is a feature, not a bug.

The rest of the semantics:

| Topic | Rule |
| --- | --- |
| Container's own size | The layout **only positions**; it never decides a container's own size. Its `w/h` *is* its outer frame |
| Content area | Container content area = its rect shrunk by `padding`; flex distributes the main-axis free space inside that area |
| flex | Main axis only; when the free space is negative (overflow) it clamps to 0 — never negative sizes |
| align | Cross-axis alignment: `Start` / `Center` / `End` / `Stretch` (fills the content area, minus the cross margin) |
| `Column` / `Row` | Stack along the main axis; `gap` is the main-axis spacing |
| `Grid` | Equal-width columns (`cols`); row height = max requested height in the row; children fill their cell |
| Resolution order | `resolve_node` arranges its own children (writing rects back) *before* recursing — a parent layout's write-back always happens before "laying out grandchildren with the new rect" |
| id | Monotonic, **never reused**; removing a subtree only frees the slot. `add`/`set` ignore the caller's id field; the return value is the real id |
| `set` reparenting | Cycle-checked (hanging a component under its own descendant is rejected); the child list position is untouched when the parent is unchanged |
| `remove` | Takes the whole subtree; implemented as **collect-then-free** — freeing point by point would write `next_sib` on freed nodes, and the free list is threaded exactly through that field |

The C surface lives in the "component tree layout (retained mode)" section of `ui_api.h`: `GUi_CompAdd/Set/Remove/Clear/Resolve/Rect/Get/Kind/Parent/ChildCount/ChildAt/Count`. Capacity is 512; out-of-range enum values are rejected outright.

**Two iron rules when drawing one window per component** (this is exactly what the Layout page of `GryceUiSmoke` does, see `test/ui_smoke.cpp`):

1. **Do not pass `GUI_WIN_NO_BRING_TO_FRONT`.** In imgui that flag makes a newly created window enter the display order at the **front** (drawn at the very bottom) — children are created after their parent, so they end up *below* it, and the parent's opaque window background hides the whole subtree except the root. Display order *is* creation order: children created later naturally draw above their parent, which is precisely what you want.
2. **Do not size components below 32px.** imgui's `WindowMinSize` defaults to (32, 32); an explicit `SetNextWindowSize` smaller than that is clamped anyway. If a container draws its label on its own first line, reserve that line with the top `padding` (24 in the demo) instead of expecting the window to shrink.

Demo: `--page layout` (or `Ctrl+3`) draws a 20-component tree — a root Column, a flex 1:2 title row, a middle row showing all four cross-axis alignments, a 3×2 grid, a footnote, plus a "leaf parent" control group (its two children write their own pos; the layout does not touch them). The page sliders mutate the root gap and the grid column count, exercising the full "Get → mutate → Set → Resolve" retained-mode loop.

## 7.8 Theme presets and beautification

The token table has two parts: **17 anchor colors** (what every preset must supply) and **40 derived colors** (computed from the anchors by fixed rules; presets never set them directly). All derivation rules live in one place, `derive_extended()` in `ui_theme.cpp`:

| Derived token | Rule |
| --- | --- |
| `CHECK_MARK` / `SLIDER_GRAB` / `NAV_HIGHLIGHT` | = `ACCENT` |
| `SLIDER_GRAB_ACTIVE` / `RESIZE_GRIP_ACTIVE` / `SEPARATOR_ACTIVE` | = `ACCENT_ACTIVE` / `ACCENT` |
| `SEPARATOR` / `TABLE_BORDER_STRONG` | = `BORDER` |
| `TITLE_BG` / `MENUBAR_BG` / `TABLE_HEADER_BG` | = `HEADER` |
| `POPUP_BG` | = `WINDOW_BG` |
| `SCROLLBAR_GRAB_HOVERED` / `ACTIVE` | = `BUTTON_HOVER` / `BUTTON_ACTIVE` |
| The rest (scrollbar track, grips, tabs, plots, table zebra stripes, nav dim …) | Fixed-ratio `mix` / `shade` (lighten/darken) between anchors |

Two benefits: **a new theme only needs 17 values**, and "which tokens must be equal after switching presets" becomes a unit test (`derived_tokens_consistent_across_presets` asserts `CHECK_MARK == ACCENT`-style identities for every preset). The day a preset starts hand-copying derived values, that test goes red — the early warning of "switching presets drifts half the UI".

Presets (`GUI_THEME_PRESET_*`, numbers aligned with `ThemePreset` and pinned by static asserts):

| # | Name | Positioning |
| --- | --- | --- |
| 0 | `Dark` | Default dark (close to ImGui's built-in dark, no visual jump) |
| 1 | `Light` | Light |
| 2 | `Midnight` | Deep navy night |
| 3 | `Nord` | Arctic blue-grey |
| 4 | `HighContrast` | Pure black high contrast (projection / outdoor) |

Beautification metrics (shared by all presets; geometry does not fork per theme): `WindowRounding=6`, `ChildRounding=4`, `FrameRounding=4`, `GrabRounding=3`, `PopupRounding=4`, `TabRounding=4`, `ScrollbarSize=12`, `GrabMinSize=8`, on top of the existing padding / spacing / indent / border. `apply()` pushes these into the ImGui `Style` in one shot (a no-op without an ImGui context).

```c
int n = GUi_ThemePresetCount();                 // 5
char name[24];
GUi_ThemePresetName(GUI_THEME_PRESET_NORD, name, sizeof(name));   // "Nord"
GUi_ThemeSetPreset(GUI_THEME_PRESET_MIDNIGHT);
GUi_ThemeApply();                               // call outside the frame; once per switch
```

**Per-token overrides do not survive preset switches**: `set_preset` refills the whole table (derived tokens included), so any `GUi_ThemeSetColor` tweaks made on top of a preset are reset. Deliberate — otherwise "switching presets" never switches cleanly.

Demo: the `Theme preset` combo at the top of the Controls page (the switch goes through the op queue, same rule as "UI only enqueues, the host executes outside the lock"), or the launch flag `--theme dark|light|midnight|nord|highcontrast`.

## 7.9 The animation system (two blocks)

The animation system is deliberately split in two; the boundary is "**does it need cross-frame state**":

* **Block 1 (pure functions)**: `GUi_AnimEase` / `GUi_AnimTween` / `GUi_AnimColorLerp` / `GUi_AnimEaseName`. Given a time, they return a value; they hold no state. "Change a property and let it transition smoothly" needs nothing else — button hover lift, panel slide-in, value rollover are all "know elapsed, compute one property value".
* **Block 2 (runtime)**: `GUiAnimClip` (track table) + the player (`GUi_AnimPlay/Pause/Stop/Resume/Seek/Advance/SetSpeed`) + named channel reads. You need it when one interface runs several interactive animations at once (staggered entrances, color cycling, pulsing).

The boundary is not a line in the sand: the runtime is implemented entirely with block 1's pure functions (`evaluate()` is just repeated `anim_ease` / `AnimTrack::eval` calls) — there is no second copy of the math.

### Block 1: pure functions

```c
// Easing: 11 kinds, f(0)=0 and f(1)=1 always hold; Back/Elastic exceed [0,1] mid-span
float e = GUi_AnimEase(GUI_ANIM_EASE_OUT_BACK, 0.7f);

// Tween: from -> to over duration seconds, delay seconds late; t is seconds since the tween started
float v;
GUi_AnimTween(0.0f, 1.0f, 1.2f, 0.0f, GUI_ANIM_EASE_IN_OUT_CUBIC, t, &v);
GUi_ProgressBar(v, -1, 0, nullptr);        // use it directly as a property

// Color interpolation
float col[4];
GUi_AnimColorLerp(red, blue, v, col);
GUi_ColorButton("swatch", col);
```

Tween boundary semantics: `t < delay` → `from` (nothing moves during the delay); `t >= delay + duration` → **exactly `to`** (independent of the easing shape); `duration <= 0` → `to` (zero duration means instant). Looping is not the pure layer's business — folding time (`fmod`) belongs to the caller, which is exactly what the demo does.

### Block 2: clips / tracks / channels / player

```c
GUiAnimClip clip{};                        // capacity: 8 tracks / 16 keys per track
clip.loop = GUI_ANIM_LOOP_LOOP;
clip.duration = 2.0f;                      // >0 = explicit; 0 = derived from tracks

GUiAnimTrack t{};                          // scalar track
strncpy(t.channel, "slide.x", sizeof(t.channel) - 1);
t.key_count = 3;
t.keys[0].t = 0.0f; t.keys[0].v = 0.0f;   t.keys[0].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
t.keys[1].t = 1.0f; t.keys[1].v = 200.0f; t.keys[1].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
t.keys[2].t = 2.0f; t.keys[2].v = 0.0f;   t.keys[2].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
clip.tracks[clip.track_count++] = t;

GUiAnimTrack c{};                          // color track: endpoint interpolation
c.is_color = 1; c.duration = 1.6f; c.ease = GUI_ANIM_EASE_IN_OUT_QUAD;
c.color_from[0] = 1; c.color_to[2] = 1;
clip.tracks[clip.track_count++] = c;

GUi_AnimPlay(&clip, 1);                    // play (copies the clip internally)
GUi_AnimAdvance(dt);                       // advance in the frame loop; the time source is the caller's
float x;
GUi_AnimChannelFloat("slide.x", &x);       // read a channel; 1 = that scalar channel exists
```

Semantics:

| Topic | Rule |
| --- | --- |
| Channels | Tracks write values into **named channels**; scalar and color channels are strictly typed — reading the wrong type returns 0 (`channel_float` on a color channel and `channel_color` on a scalar channel both return 0) |
| Segment easing | On a scalar track, key *i*'s `ease` governs the interval `[keys[i], keys[i+1])` ("which curve to use from this key to the next") |
| Outside the range | Before the first key it holds the first key's value; after the last key it holds the last key's value — no extrapolation |
| Track delay | Local time = clip time − delay; while negative it holds the initial value (scalar = first key's value, color = `color_from`) — this is what staggered entrances use |
| Loop | `Once` (sets finished at the end), `Loop` (`fmod` wrap), `PingPong` (triangle fold); `GUi_AnimTime()` reports the **unfolded** accumulated time |
| Clip duration | Explicit `duration` wins; otherwise the max of `(delay + length)` over all tracks; tracks shorter than it hold their final value in the tail |
| `play` | Copies the clip internally (the caller's clip can change afterwards); `restart=0` keeps the current time (for changing loop mode / parameters) |
| Channel table | Rebuilt from the playing clip's tracks on every `play` — after switching clips you cannot read stale values from the previous one |
| Transport | `stop` halts but **channels keep their last values** (the picture freezes); `pause`/`resume`; `resume` after completion restarts from the beginning; `set_speed` clamps to [0, 64] |
| Threading | Pure data, no locks; call `advance` outside the frame loop; reading channels inside the callback is the same tier as `GUi_CompRect` |

**Working with the component tree**: the animation system does not know about components, and components do not depend on animation. The host writes channel values into component rects (`GUi_CompGet` → mutate → `GUi_CompSet`). Those components should have parent 0 — otherwise a layout parent would overwrite the animation's pos on `resolve()`. Put the other way around: the 7.7 rule "no layout parent → the written pos is kept" is exactly what the animation system reuses: **animation writes pos directly; the layout does not touch it**.

Demo: `--page anim` (or `Ctrl+4`). The panel is block 1 (easing combo + tween progress bar + color-lerp swatch, timed by the host's own clock); the stage on the right is block 2 — one 2-second clip drives four components: `slide` (ease-in-out-cubic keyframes), `pulse` (two pulses), `hop` (two uneven bounces), `stag.0..3` (staggered entrances with delay 0/0.15/0.30/0.45). The page lets you change the loop mode (without resetting progress), the speed, and drag the seek slider, while the channel list shows every channel's live value.

## 7.10 The built-in UI render path (leaving ImGui) and UIDrawList

`GUi_*` currently lands through Dear ImGui. The engine is building a **built-in UI runtime**: a retained control tree (`ui_component`) plus self-drawn rendering, with no ImGui dependency at the end. Four phases, each independently verifiable:

| Phase | Content | Status |
| --- | --- | --- |
| 1 | `ui_drawlist` command buffer (model layer, pure logic) | done |
| 2 | Render bridge: drawlist → `IRenderer2D`; all three backends inherit it; no new shader | done (7.11) |
| 3 | Retained control layer: hit testing + event dispatch + control library (Button/Label/Slider/Checkbox/TextInput) | done (7.11) |
| 4 | `GUi_*` dual-track switch; ImGui retreats to an optional debug backend | done (7.11) |

ImGui's role on this path changes accordingly: **from "the UI system itself" to "one backend for editor/debug UI"**. The UI model layer (7.6), component tree (7.7) and animation (7.9) contain no ImGui dependency, so swapping the backend does not touch them.

**Why the render bridge needs no new shader**: the rasterization carrier is the engine's existing `IRenderer2D` (`engine/render/render2d.h`) — `draw_rect` / `draw_text` / `draw_sprite_region` / `set_scissor` / `top_left_origin` (that interface's comment literally says "UI/HUD layer") — already implemented on all three backends (D3D12 / Vulkan / GL); text goes through the engine's own `FontAtlas` (stb_truetype, multi-source Latin+CJK fallback, glyph UV/advance). The built-in UI only supplies what is missing: the command buffer and the control semantics.

### UIDrawList semantics (`engine/ui/ui_drawlist.h`)

| Topic | Rule |
| --- | --- |
| Positioning | Model layer: records only "what to draw" (commands + clip stack + string pool); never touches the GPU, so it is unit-testable headless |
| Commands | Five kinds: `Rect` / `Text` / `Sprite` / `PushClip` / `PopClip`; POD shared by the render bridge and the tests |
| Coordinates | Top-left origin, +y down, matching `IRenderer2D`'s `top_left_origin` |
| Clip intersection | `push_clip` intersects with the current clip; an empty intersection (w≤0 or h≤0) enters the cull state |
| Cull state | Subsequent **draw commands** are dropped and counted in `culled()`; `PushClip`/`PopClip` are still recorded (otherwise there is no way back) |
| Per-command clip tag | Every command carries `clip` = the clip-stack depth **in effect after executing it** (PushClip records the depth after pushing, PopClip after popping, draw commands the current depth; 0 = no clip); the render bridge switches scissor by depth instead of storing a clip rect per command |
| Clip stack full | The push is rejected and counted in `dropped()`; `pop` only unwinds by depth, so a rejected push never unbalances later pops |
| Command table full | Dropped and counted in `dropped()` (capacity 4096) |
| String pool | 32KB; if a whole string does not fit, the **entry is dropped and counted, never truncated** — half a string is harder to debug than none |
| Empty text | `nullptr` or an empty string produces no command (not counted as culled) |
| Geometry | No normalization: negative w/h is recorded verbatim; geometric correctness belongs to the layout layer |

Demo host check: `GryceUiSmoke --ui-backend builtin --page controls|queue|layout|anim|struct` — no ImGui involved; the built-in immediate-mode layer produces the drawlist and the render bridge draws it, for screenshot regression (same host code as `--ui-backend imgui`, only the backend differs). The `struct` page demos collapsing headers, trees, a scrolling child region, popups, a modal and a tooltip; `--ui-scale N` exercises HiDPI.

## 7.11 The built-in UI track (`GRender_SetUIBackend(1)`)

Once all four phases landed, `GUi_*` has two implementations and **the same host code** runs on both.

```c
GRender_SetUIBackend(1);      /* must be before GRender_Init: decides whether an ImGui context is created */
GRender_SetUICallback(cb, user);   /* callback signature unchanged, identical on both tracks */
GRender_Init(&desc);
```

| Topic | Rule |
| --- | --- |
| Backend choice | Process-wide, effective before init. 0 = ImGui (default, behaviour identical to before); 1 = built-in |
| Who draws | ImGui track: ImGui draw data → commands. Built-in track: `UiDrawList` → 2D overlay pass |
| Callback position | Same: inside `GRender_EndFrame`, with `g_renderer.mutex` held |
| Throttling | Same: `GRender_SetUiFrameInterval` means "rebuild every N frames"; non-build frames redraw the previous result |
| Context | The built-in track creates **no** ImGui context (`GRender_HasImGui()` returning false is the normal state) |

### Frame order (`engine/api/ui_builtin_backend.h`)

```
debug_ui_submit_locked()            main thread, lock held
  ├─ gather input (GInput_IsMouseButton*/GInput_PullChars/backspace) → UiInputFrame
  ├─ ui.begin_frame(...)            inject theme + text metrics
  ├─ host callback                  submit controls / enqueue ops only
  ├─ ui.end_frame() + ui.draw()     close the frame, sweep unsubmitted controls, produce a UiDrawList
  └─ renderer2d->begin_overlay_pass / bridge / end_overlay_pass
```

**Why it draws synchronously here instead of pushing a render command like ImGui**: `Renderer2D`'s members (camera, vertex batches, screen size) are written only by the main thread; the render thread only executes the commands it packs. Handing the overlay pass to the render thread races with the next frame's main-thread writes — the symptom is random heap corruption (`0xC0000374`), not a stable crash. The overlay pass itself only records vertices and packs them into commands, so GL calls still land after the scene commands and the UI still covers the final image.

### What the built-in track supports

The built-in track now has the **same control set** as the ImGui track: one host code base runs the full flow on either track. Only the differences and the exact semantics are listed here.

| Topic | Built-in behaviour |
| --- | --- |
| `BeginWindow` / `EndWindow` | Supported. `GUI_WIN_TITLE_BAR` adds a title bar (draggable, 22px collapse box on the left); `GUI_WIN_NO_MOVE` / `NO_COLLAPSE` / `NO_SCROLLBAR` / `NO_RESIZE` are honoured. Offset, collapsed and scroll state persist per window id, even though the host re-supplies the rect every frame |
| `Text` / `TextColored` / `Separator` / `Spacing` / `SameLine` / `LabelText` / `Indent` / `Unindent` | Supported; `Text` **wraps** to the content width |
| `Button` / `ButtonSize` / `SmallButton` / `Checkbox` / `SliderFloat` / `InputText` / `ColorButton` / `ProgressBar` | Supported (`SmallButton` = a normal button; `ColorButton` draws only the swatch, its label is an id) |
| `DragFloat` | Real dragging: while held the value follows the **mouse delta** (`speed` units per pixel, Ctrl for 1/10 fine-tuning). There is no "jump where you clicked" semantics |
| `DragFloat2/3/4` | Degrades to a `DragFloat` on the first component; the rest are read-only |
| `ColorEdit4` | Swatch plus a popup with R/G/B/A sliders; edits write back immediately |
| `Combo` | Real dropdown: popup plus menu items, a selection writes back and closes it |
| `BeginChild` / `EndChild` | Supported: own clip rect (out-of-view items are invisible *and* unclickable), own wheel scrolling and offset. `size=(0,0)` fills the remaining space, as in ImGui |
| `TreeNode` / `TreePop` / `CollapsingHeader` | Supported: open state persists across frames, children indent automatically |
| `BeginMenuBar` / `BeginMenu` / `EndMenu` / `MenuItem` | Supported: a menu is a popup anchored under its button; clicking an item closes it |
| Popups: `OpenPopup` / `OpenPopupOnItemClick` / `BeginPopup` / `EndPopup` / `CloseCurrentPopup` / `IsPopupOpen` | Supported. Popups draw on layer 2 (always above windows), auto-size their height, and close on an outside click or Esc — `BeginPopup` already returns 0 on that same frame, matching ImGui's NewFrame semantics |
| Modal: `OpenModal` / `BeginModal` | Supported: centred, a dim full-screen mask underneath, **outside clicks do not close it**; only Esc or `CloseCurrentPopup` |
| Tooltip: `SetTooltip` | Supported: follows the mouse; delayed 0.35 s when `time > 0` (with `time = 0` it shows immediately, which keeps unit tests and screenshots reproducible) |
| Drag and drop: `BeginDragDropSource` / `SetDragDropPayloadString` / `EndDragDropSource` / `BeginDragDropTarget` / `AcceptDragDropPayloadString` | Supported: activates past a 4px threshold; payload = (type string, text); delivered when the button is released over the target |
| Queries: `LastItemRect` / `IsItemHovered` / `IsItemClicked` / `IsItemActive` / `IsAnyItemActive` / `MousePos` / `IsMouse*` / `GetModifiers` | Supported |
| `MouseDeltaX/Y` / `SetCursorPos` | Always 0 / no-op (read `GInput_*` for deltas) |
| `WantCaptureMouse` | True while the mouse is inside any panel or popup rect |
| `WantCaptureKeyboard` | True while a text input holds focus |

### Text editing (what `InputText` really does on the built-in track)

| Input | Behaviour |
| --- | --- |
| Click / drag | The caret is mapped from x through the **real text width** into a byte offset that always sits on a UTF-8 code-point boundary; dragging selects |
| ← → / Home / End | Move by code point / to line start and end; with Shift it extends the selection |
| Ctrl+← → | Move by word (`text_word_left/right`) |
| Backspace / Delete | Deletes the selection when there is one, otherwise the previous/next code point |
| Ctrl+A / C / X / V | Select all / copy / cut / paste through the process-wide clipboard `ui_clipboard()` (hosts may read and write it to mirror the OS clipboard) |
| Esc | Clears the selection and drops focus |
| IME | The pre-edit string (`UiInputFrame::preedit`) is drawn **underlined** at the caret and belongs only to the input that holds focus this frame; composing never writes into the caller's buffer |
| Caret blink | Driven by `UiInputFrame::time` (1.06 s period, 0.53 s on); `time == 0` keeps it always visible, so tests and screenshots stay reproducible |
| Long text | Scrolls horizontally to keep the caret visible; text never bleeds outside the box |

### Three prerequisites for text and coordinates

1. **One source of metrics**: auto-sizing (button width = text width + padding) and text centring both use `IRenderer2D::measure_text` (sum of glyph advances, UTF-8 by code point); when the renderer provides no metrics it falls back to `ui_approx_text_width`. They must be the same source, otherwise "layout by estimate, draw by truth" drifts apart.
2. **`draw_text` resolves glyphs per code point**: the 2D text path now decodes UTF-8 (`Renderer2D::draw_text`), so CJK renders; code points missing from the atlas are skipped.
3. **HiDPI goes through `GRender_SetUIScale`**: the model layer always works in **logical pixels** (layout, hit testing and text metrics are logical); input coordinates are divided by the scale in `begin_frame` and the render bridge multiplies rects, font size and scissor boxes back. The host sets the scale once after init and must not scale panel coordinates itself.

### Renderer additions this track needed (`engine/render/render2d.h`)

| Interface | Purpose |
| --- | --- |
| `measure_text(text, size)` | Text width (0 = no metrics, callers fall back to the estimate) |
| `begin_overlay_pass(w,h)` / `end_overlay_pass()` | The UI overlay gets its own pass: save/restore the camera, switch to top-left 1:1, skip bloom and shadows |
| `set_scissor` / `reset_scissor` flush batches first | Clipping is state, geometry is vertices: pending vertices must be submitted before the clip changes, or both segments inherit the last clip rect |
| `GRender_SetUIScale(f)` / `GRender_GetUIScale()` | HiDPI scale. The GL backend enables `GL_SCISSOR_TEST` on demand in `set_scissor` and disables plus invalidates the cache when the render target changes (otherwise an offscreen pass inherits a screen-space clip) |
| `GInput_GetWheel(dx,dy)` / `GInput_InjectPreedit(s)` / `GInput_GetPreedit(buf,cap)` | Wheel delta (take-and-clear) and the IME pre-edit string |

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
* Windows can be dragged and collapsed but not resized by their border (`GUI_WIN_NO_RESIZE` always applies: the host layout owns the size).
* The font atlas covers ASCII plus a CJK fallback; there is no rich text or mixed font sizes — one control has one font size.
* Five built-in theme presets (Dark / Light / Midnight / Nord / HighContrast, see 7.8); callers reference tokens only — do not hardcode colours in callers.
* `GUi_Image` takes no custom UVs or border tinting; the viewport shows the full texture.
* Non-ASCII text depends on the font atlas the engine loaded; missing glyphs render as boxes rather than crashing.
