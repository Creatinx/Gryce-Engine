// main.js — 项目入口（替代原来的 main.cpp）
//
// 宿主（GryceApp.exe）只做三件事：初始化 Core、装载本文件、每帧调用
// on_update。窗口、场景、相机、退出全部在这里用 engine.* 决定，所以项目
// 目录不需要自己的 C++ 入口。
//
// 生命周期钩子：
//   on_start()        入口装载后一次（热重载成功后重放）
//   on_update(dt)     每帧；此时 engine.input.* 已是本帧输入
//   on_destroy()      进程退出前一次
//
// 操作：鼠标转视角（窗口有焦点时锁定指针），WASD 移动，空格/左Ctrl 升降，
//       Shift 加速，Esc 退出；R 切换硬件光追反射，T 切换 SSR（Vulkan 下可对比）。
//
// 场景从 project.data 的 main_scene 读，脚本里不写死路径：同一个入口既跑普通
// 演示场景也跑 rt_showcase，换场景只改配置。一个项目只应有一个入口脚本。

const KEY = {
    SPACE: 32, A: 65, D: 68, E: 69, Q: 81, S: 83, W: 87, R: 82, T: 84,
    ESC: 256, LEFT_SHIFT: 340, LEFT_CONTROL: 341,
};

const DEG2RAD = Math.PI / 180.0;
const RAD2DEG = 180.0 / Math.PI;

let camera = null;
let rt_toggle_prev = false;
let ssr_toggle_prev = false;
let yaw = 0.0;
let pitch = 0.0;
let position = [0, 0, 0];
let speed = 3.0;
let sensitivity = 0.12;

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

// 用四元数旋转向量：v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
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
    const qy = axis_angle(0, 1, 0, yaw * DEG2RAD);
    const qp = axis_angle(1, 0, 0, pitch * DEG2RAD);
    return qmul(qy, qp);
}

function log_render_state() {
    engine.log.info('reflection: RT=' + (engine.render.is_rt_reflection() ? 'ON' : 'off') +
                    '  SSR=' + (engine.render.is_ssr() ? 'ON' : 'off'));
}

export function on_start() {
    engine.log.info('main.js: start');

    if (!engine.window.create({ title: 'Gryce Demo', width: 1280, height: 720 })) {
        engine.log.error('main.js: window.create failed');
        return;
    }

    // 不传参 = 载入 project.data 的 main_scene；改配置就换场景。
    if (!engine.scene.load()) {
        engine.log.error('main.js: scene load failed');
        return;
    }

    camera = engine.entity.find('MainCamera');
    if (!camera) {
        engine.log.error('main.js: MainCamera not found');
        return;
    }

    // 从场景里的相机朝向反解 yaw/pitch，保证接管视角时针头不跳。
    const q = camera.rotation();
    const f = rotate(q, [0, 0, -1]);
    pitch = Math.asin(clamp(f[1], -1, 1)) * RAD2DEG;
    yaw = Math.atan2(-f[0], -f[2]) * RAD2DEG;
    position = camera.position();

    engine.input.set_mouse_locked(true);

    engine.log.info('main.js: camera ready, entities=' + engine.entity.count());
    log_render_state();
}

export function on_update(dt) {
    if (!camera) return;

    if (engine.input.key_down(KEY.ESC)) {
        engine.app.quit();
        return;
    }

    // engine.input 只有电平接口（key_down），用上一帧状态做边沿检测。
    const rt_now = engine.input.key_down(KEY.R);
    if (rt_now && !rt_toggle_prev) {
        engine.render.set_rt_reflection(!engine.render.is_rt_reflection());
        log_render_state();
    }
    rt_toggle_prev = rt_now;

    const ssr_now = engine.input.key_down(KEY.T);
    if (ssr_now && !ssr_toggle_prev) {
        engine.render.set_ssr(!engine.render.is_ssr());
        log_render_state();
    }
    ssr_toggle_prev = ssr_now;

    // 只在指针锁定时转视角；解锁时保持朝向（和 Alt-Tab 切回来的体验一致）
    if (engine.input.mouse_locked()) {
        const delta = engine.input.mouse_delta();
        yaw -= delta[0] * sensitivity;
        pitch = clamp(pitch - delta[1] * sensitivity, -89.0, 89.0);
    }

    const held = (code) => engine.input.key_down(code);
    const forward = (held(KEY.W) ? 1 : 0) - (held(KEY.S) ? 1 : 0);
    const side = (held(KEY.D) ? 1 : 0) - (held(KEY.A) ? 1 : 0);
    const up = (held(KEY.SPACE) || held(KEY.E) ? 1 : 0) -
               (held(KEY.LEFT_CONTROL) || held(KEY.Q) ? 1 : 0);

    if (forward !== 0 || side !== 0 || up !== 0) {
        const q = view_quat();
        const f = rotate(q, [0, 0, -1]);
        const flat = Math.hypot(f[0], f[2]);
        const fx = flat > 1e-4 ? f[0] / flat : 0;
        const fz = flat > 1e-4 ? f[2] / flat : 0;

        // right = normalize(forward x worldUp) = (-f.z, 0, f.x)
        let mx = fx * forward - fz * side;
        let my = up;
        let mz = fz * forward + fx * side;
        const len = Math.hypot(mx, my, mz);
        if (len > 1e-4) {
            const sprint = held(KEY.LEFT_SHIFT) ? 2.5 : 1.0;
            const scale = speed * sprint * dt / len;
            position[0] += mx * scale;
            position[1] += my * scale;
            position[2] += mz * scale;
        }
    }

    camera.set_position(position[0], position[1], position[2]);
    camera.set_rotation(view_quat());
}

export function on_destroy() {
    engine.input.set_mouse_locked(false);
    engine.log.info('main.js: destroy');
}
