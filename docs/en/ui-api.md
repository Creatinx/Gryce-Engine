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

How the editor does it (`editor/ext/ui/intent.h`):

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
| Values | `GUi_Checkbox` / `GUi_SliderFloat` / `GUi_DragFloat` / `GUi_DragFloat3` / `GUi_DragFloat4` |
| Text input | `GUi_InputText` / `GUi_SetKeyboardFocusHere` / `GUi_IsItemDeactivatedAfterEdit` |
| Enums | `GUi_Combo(label, current, items, count)` |
| Trees | `GUi_TreeNode(label, default_open)` + `GUi_TreePop` / `GUi_CollapsingHeader` |
| Images | `GUi_Image(tex, w, h)` |
| Popups | `GUi_OpenPopupOnItemClick` / `GUi_OpenPopup` / `GUi_BeginPopup` / `GUi_EndPopup` |
| Layout | `GUi_SetCursorPos` / `GUi_GetCursorPos` / `GUi_GetContentAvail` / `GUi_SetNextItemWidth` / `GUi_GetTextLineHeight` / `GUi_GetFrameHeight` |
| Hit testing | `GUi_LastItemRect` / `GUi_IsItemHovered` / `GUi_IsItemClicked` / `GUi_IsItemActive` / `GUi_IsAnyItemActive` |
| Mouse | `GUi_MousePos` / `GUi_MouseDeltaX` / `GUi_MouseDeltaY` / `GUi_IsMouseDown` / `GUi_IsMouseClicked` / `GUi_IsMouseReleased` |
| Capture | `GUi_WantCaptureMouse` / `GUi_WantCaptureKeyboard` |
| Style | `GUi_PushTextStyle(kind)` / `GUi_PopTextStyle` |

Return convention: `int` widgets return 1 when clicked / edited this frame, 0 otherwise. In-place pointer arguments (`float*`, `char*`, `int*`) already hold the new value when 1 is returned.

`GUi_Selectable` is for "whole row clickable and shows a selected state" (hierarchy tree, asset list). A row that also needs a context menu must call `GUi_SetItemAllowOverlap()` right after it, otherwise clicks get stolen by the neighbouring row's hit rect.

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

## 6. Layout convention

`ui_api.h` deliberately offers **no docking**. The editor uses one main window with fixed partitions:

```
menu bar
toolbar
├─ hierarchy 236 px ─┬─ viewport (fills) ─┬─ inspector 306 px ─┤
└──────────────── asset list / stats 168 px ───────────────────┘
```

Docking state is persisted in `imgui.ini` and drifts with user drags, which makes screenshot comparison and `editor_selftest` unreproducible. Fixed partitions also guarantee the viewport has a deterministic rect at any window size, which is what makes assertions possible.

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
* One dark theme plus semantic text colours (`GUI_TEXT_*`). To restyle, change the engine's `debug_imgui` style; do not hardcode colours in callers.
* `GUi_Image` takes no custom UVs or border tinting; the viewport shows the full texture.
* Non-ASCII text depends on the font atlas the engine loaded; missing glyphs render as boxes rather than crashing.
