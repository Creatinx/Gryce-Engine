// ===========================================================================
// editor/project/main.js — 编辑器引导脚本
//
// 这里只有"调度"，没有"编辑规则"：读输入 → 调 engine.editor.* → 消费返回。
// 选择集、历史分组、Gizmo 换算、快捷键表全部在 ext/ 里，原因见 ../README.md
// 第 2 节：同一套规则写两遍必然漂移。
//
// 扩展缺失时的降级：engine.editor 为 undefined，本文件退化成"自由相机浏览器"，
// 保证扩展编译失败时编辑器仍能打开、仍能看到场景和日志。
//
// 钩子：on_start() 一次 / on_update(dt) 每帧 / on_destroy() 退出前一次。
// ===========================================================================

const PROJECT = 'Gryce Editor';
const WINDOW_W = 1600;
const WINDOW_H = 900;

// GLFW 键码（与 engine.input.key_down 的取值一致）
const KEY = {
    SPACE: 32, A: 65, D: 68, E: 69, F: 70, G: 71, Q: 81, R: 82, S: 83, W: 87,
    Z: 90, ESC: 256, DEL: 261,
    LEFT_SHIFT: 340, LEFT_CONTROL: 341,
};

const MOUSE_LEFT = 0;
const MOUSE_RIGHT = 1;
const MOUSE_MIDDLE = 2;

const DEG2RAD = Math.PI / 180.0;

// 视口相机。编辑器的"视口相机"不是场景里的 Camera 实体 —— 场景相机属于被编辑
// 的内容，视口走 GViewport_SetEditorCamera 那条独立通道。
const cam = {
    pos: [0.0, 2.2, 6.0],
    yaw: 0.0,
    pitch: -12.0,
    fov: 55.0,
    speed: 6.0,
};

let editor = null;      // engine.editor，扩展缺失时为 null
let uiOn = false;       // 界面接管了视口：鼠标交互交给 ext 的 UI，JS 不再插手
let viewW = WINDOW_W;
let viewH = WINDOW_H;
let dragAxis = -1;      // 正在拖拽的 Gizmo 轴，-1 表示空闲
let navDrag = false;    // 正在用右键拖拽转视角
let wireframe = false;
let lastPrint = 0.0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

function clamp(v, lo, hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

function normalize(q) {
    const len = Math.hypot(q[0], q[1], q[2], q[3]);
    return len > 1e-6 ? [q[0] / len, q[1] / len, q[2] / len, q[3] / len] : [0, 0, 0, 1];
}

function axis_angle(ax, ay, az, radians) {
    const h = radians * 0.5;
    const s = Math.sin(h);
    return normalize([ax * s, ay * s, az * s, Math.cos(h)]);
}

function qmul(a, b) {
    return normalize([
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    ]);
}

// v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
function rotate(q, v) {
    const cx = q[1] * v[2] - q[2] * v[1] + q[3] * v[0];
    const cy = q[2] * v[0] - q[0] * v[2] + q[3] * v[1];
    const cz = q[0] * v[1] - q[1] * v[0] + q[3] * v[2];
    return [
        v[0] + 2.0 * (q[1] * cz - q[2] * cy),
        v[1] + 2.0 * (q[2] * cx - q[0] * cz),
        v[2] + 2.0 * (q[0] * cy - q[1] * cx),
    ];
}

function view_quat() {
    const qy = axis_angle(0, 1, 0, cam.yaw * DEG2RAD);
    const qp = axis_angle(1, 0, 0, cam.pitch * DEG2RAD);
    return qmul(qy, qp);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

export function on_start() {
    engine.log.info('editor: ' + PROJECT + ' booting');

    if (!engine.window.create({ title: PROJECT, width: WINDOW_W, height: WINDOW_H })) {
        engine.log.error('editor: window.create failed');
        return;
    }
    const size = engine.window.size();
    viewW = size[0];
    viewH = size[1];

    if (!engine.scene.load('res:/scenes/main.gesc')) {
        engine.log.error('editor: scene load failed (res:/scenes/main.gesc)');
        return;
    }

    // 扩展是可选的：没有它编辑器降级成场景浏览器。
    if (typeof engine.editor !== 'undefined' && engine.editor) {
        editor = engine.editor;
        engine.log.info('editor: ext ' + editor.version());
        setup_editor();
    } else {
        engine.log.warn('editor: engine.editor missing, running as scene browser '
                        + '(build GryceEditorExtension and drop it next to project.data)');
    }

    engine.log.info('editor: ready, entities=' + engine.entity.count());
}

function setup_editor() {
    // 离屏视口：场景写进纹理，窗口留给 UI 层。
    editor.view_enableOutput(true);
    editor.view_setSize(viewW, viewH);

    // 界面打开时，视口内的鼠标（拾取 / Gizmo 拖拽）由 ext 的 UI 自己处理：
    // 它拿得到视口矩形，能用视口内坐标发射射线。JS 这边只知道窗口坐标，
    // 两边都处理会各选中一次、拖拽量翻倍。
    uiOn = editor.ui_available() === true;
    engine.log.info('editor: ui ' + (uiOn ? 'active' : 'off (headless)'));

    // 默认平移工具、世界空间 —— 与多数 DCC 的初始状态一致。
    editor.gizmo_enable(true);
    editor.gizmo_setMode(0, 0);

    editor.shortcut_bind('undo', KEY.Z, 1);
    editor.shortcut_bind('redo', KEY.Z, 3);      // ctrl + shift
    editor.shortcut_bind('save', KEY.S, 1);
    editor.shortcut_bind('delete', KEY.DEL, 0);
    editor.shortcut_bind('translate', KEY.W, 0);
    editor.shortcut_bind('rotate', KEY.E, 0);
    editor.shortcut_bind('scale', KEY.R, 0);
    editor.shortcut_bind('wireframe', KEY.G, 0);
    editor.shortcut_bind('play', KEY.SPACE, 0);
    editor.shortcut_bind('hover', KEY.F, 0);

    editor.open('res:/scenes/main.gesc');
    engine.log.info('editor: session=' + editor.session());
}

export function on_destroy() {
    engine.input.set_mouse_locked(false);
    if (editor) editor.close();
    engine.log.info('editor: shutdown');
}

// ---------------------------------------------------------------------------
// 每帧
// ---------------------------------------------------------------------------

export function on_update(dt) {
    if (editor) {
        handle_shortcuts();
        // 界面接管视口时不碰鼠标：拾取与 Gizmo 拖拽在 ext 的 UI 里，
        // 用的是视口内坐标（见 setup_editor 的说明）。
        if (!uiOn) handle_mouse();
    }
    navigate(dt);

    // tick 放最后：本帧入队的编辑命令，下一帧 GCore_BeginFrame 消费，
    // 再下一帧 tick 才能确认结果。扩展的 pending 队列就是靠这个节奏工作的。
    if (editor) {
        const events = editor.tick(dt);
        if (events && events.length > 2) {
            engine.log.info('editor: events ' + events);
        }
    }

    lastPrint += dt;
    if (lastPrint > 5.0) {
        lastPrint = 0.0;
        engine.log.info('editor: ' + (editor ? editor.panel_stats() : 'no-ext')
                        + ' fps=' + Math.round(engine.app.fps()));
    }
}

// ---------------------------------------------------------------------------
// 快捷键：绑定表在 ext 里，这里只按名字分发
// ---------------------------------------------------------------------------

function handle_shortcuts() {
    const fired = editor.shortcut_poll();
    if (!fired || fired === '[]') return;

    const names = JSON.parse(fired);
    for (let i = 0; i < names.length; ++i) {
        action(names[i]);
    }
}

function action(name) {
    switch (name) {
        case 'undo':
            editor.undo();
            break;
        case 'redo':
            editor.redo();
            break;
        case 'save': {
            // 保存到会话记录的文档路径；没有路径时扩展返回 0。
            const ok = editor.save('');
            engine.log.info('editor: save -> ' + (ok ? 'queued' : 'rejected'));
            break;
        }
        case 'delete':
            editor.deleteSelection();
            break;
        case 'translate':
            editor.gizmo_setMode(0, gizmo_space());
            break;
        case 'rotate':
            editor.gizmo_setMode(1, gizmo_space());
            break;
        case 'scale':
            editor.gizmo_setMode(2, gizmo_space());
            break;
        case 'wireframe':
            toggle_wireframe();
            break;
        case 'play':
            editor.togglePlay();
            break;
        case 'hover':
            focus_selection();
            break;
        default:
            break;
    }
}

// Gizmo 空间从扩展读回来再原样写回：切换工具时不能把空间重置成世界空间，
// 否则用户刚切的"局部"会在按 W/E/R 时被打回原形。
function gizmo_space() {
    const mode = JSON.parse(editor.gizmo_mode());
    return mode.space;
}

function toggle_wireframe() {
    const mode = wireframe ? 'Shaded' : 'Wireframe';
    if (!editor.view_displayModeSupported(mode)) {
        engine.log.warn('editor: display mode ' + mode + ' not supported by this backend');
        return;
    }
    wireframe = !wireframe;
    editor.view_setDisplayMode(mode);
    engine.log.info('editor: display mode -> ' + mode);
}

// ---------------------------------------------------------------------------
// 鼠标（仅在界面未接管视口时走这条路径：无 ImGui 的构建 / 关掉界面的情况）
// 左键 = Gizmo 拖拽 / 拾取，右键 = 转视角（见 navigate）
// ---------------------------------------------------------------------------

function handle_mouse() {
    const mx = engine.input.mouse_x();
    const my = engine.input.mouse_y();
    const left = engine.input.mouse_button(MOUSE_LEFT);

    // 左键按下的那一帧先问 Gizmo 命中哪个轴，命中就进拖拽，否则做拾取。
    // 顺序不能反：先拾取会把"点在 gizmo 轴上"当成点到了物体表面，
    // 于是每拖一次就换一次选中目标。
    if (left && dragAxis < 0 && !navDrag) {
        const axis = editor.gizmo_hitTest(mx, my);
        if (axis >= 0) {
            dragAxis = axis;
            editor.gizmo_beginDrag(axis);
        } else {
            const hit = editor.pick(mx, my);
            if (hit > 0) {
                editor.select(hit);
                engine.log.info('editor: selected ' + hit);
            } else {
                editor.select(0);
            }
        }
    }
    if (!left && dragAxis >= 0) {
        editor.gizmo_endDrag();
        dragAxis = -1;
    }
    if (dragAxis >= 0) {
        const d = engine.input.mouse_delta();
        // 屏幕像素增量交给扩展换算：轴方向、相机基向量、拖拽灵敏度都在那边，
        // JS 只搬数字，避免同一套换算在两处出现。
        editor.gizmo_drag(d[0] + d[1]);
    }
}

// ---------------------------------------------------------------------------
// 视口相机导航
// ---------------------------------------------------------------------------

function navigate(dt) {
    const held = (code) => engine.input.key_down(code);

    // 右键转视角。界面打开时只在指针位于视口内才启动 —— 否则在层级树、
    // 检视器上右键也会把相机转走。启动之后不再判位置：拖到视口外也继续转，
    // 中途中断会让视角停在半路上。
    const right = engine.input.mouse_button(MOUSE_RIGHT);
    const overView = !uiOn || (editor && editor.ui_pointerInView() === true);
    if (right && overView && dragAxis < 0) navDrag = true;
    if (!right) navDrag = false;

    if (navDrag) {
        const d = engine.input.mouse_delta();
        cam.yaw -= d[0] * 0.15;
        cam.pitch = clamp(cam.pitch - d[1] * 0.15, -89.0, 89.0);
    }

    // 平移只在导航态生效：空闲时 WASD 是 Gizmo 工具切换键，
    // 同时兼顾移动会让按 W 既切工具又让相机飘。
    if (navDrag || engine.input.mouse_button(MOUSE_MIDDLE)) {
        const forward = (held(KEY.W) ? 1 : 0) - (held(KEY.S) ? 1 : 0);
        const side = (held(KEY.D) ? 1 : 0) - (held(KEY.A) ? 1 : 0);
        const lift = (held(KEY.E) ? 1 : 0) - (held(KEY.Q) ? 1 : 0);

        if (forward !== 0 || side !== 0 || lift !== 0) {
            const q = view_quat();
            const f = rotate(q, [0, 0, -1]);
            const flat = Math.hypot(f[0], f[2]);
            const fx = flat > 1e-4 ? f[0] / flat : 0;
            const fz = flat > 1e-4 ? f[2] / flat : 0;

            const mx = fx * forward - fz * side;
            const mz = fz * forward + fx * side;
            const len = Math.hypot(mx, lift, mz);
            if (len > 1e-4) {
                const sprint = held(KEY.LEFT_SHIFT) ? 3.0 : 1.0;
                const s = cam.speed * sprint * dt / len;
                cam.pos[0] += mx * s;
                cam.pos[1] += lift * s;
                cam.pos[2] += mz * s;
            }
        }
    }

    if (editor) {
        editor.view_setEditorCamera(cam.pos[0], cam.pos[1], cam.pos[2],
                                    cam.yaw, cam.pitch, cam.fov);
    } else {
        // 降级路径：没有扩展就直接驱动场景里的相机实体。
        const c = engine.entity.find('MainCamera');
        if (c) {
            c.set_position(cam.pos[0], cam.pos[1], cam.pos[2]);
            c.set_rotation(view_quat());
        }
    }
}

// F 键：把视口相机挪到选中实体的包围盒中心。
function focus_selection() {
    const focus = editor.focusPoint();
    if (!focus || focus.length < 3) return;
    const p = JSON.parse(focus);
    if (!p.ok) return;
    cam.pos = [p.x, p.y, p.z];
    engine.log.info('editor: focus -> ' + p.x.toFixed(2) + ','
                    + p.y.toFixed(2) + ',' + p.z.toFixed(2));
}
