// rt_main.js — 硬件光追反射（Vulkan ray query）DEMO 入口
//
// 与 main.js 的区别：加载 rt_showcase.gesc（镜面地板 + 金属/彩色物体 +
// 旋转体），并默认开启硬件光追反射、关闭 SSR，让反射纯粹来自 ray query。
// 运行中可按键实时对比两种反射方案：
//   R  切换 硬件光追反射（RT，Vulkan ray query）
//   T  切换 SSR（屏幕空间反射，作对比）
//   Esc 退出
//
// 操作：鼠标转视角（窗口有焦点时锁定指针），WASD 移动，空格/左Ctrl 升降，
//       Shift 加速，Esc 退出。

const KEY = {
    SPACE: 32, A: 65, D: 68, E: 69, Q: 81, S: 83, W: 87, R: 82, T: 84,
    ESC: 256, LEFT_SHIFT: 340, LEFT_CONTROL: 341,
};

const DEG2RAD = Math.PI / 180.0;
const RAD2DEG = 180.0 / Math.PI;

let camera = null;
let yaw = 0.0;
let pitch = 0.0;
let position = [0, 0, 0];
let speed = 3.0;
let sensitivity = 0.12;
let rt_toggle_prev = false;
let ssr_toggle_prev = false;

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

function log_state() {
    engine.log.info('RT reflection=' + (engine.render.is_rt_reflection() ? 'ON' : 'off') +
                    '  SSR=' + (engine.render.is_ssr() ? 'ON' : 'off'));
}

export function on_start() {
    engine.log.info('rt_main.js: start (hardware ray-traced reflection demo)');

    if (!engine.window.create({ title: 'Gryce RT Reflection', width: 1280, height: 720 })) {
        engine.log.error('rt_main.js: window.create failed');
        return;
    }

    if (!engine.scene.load('res:/scenes/rt_showcase.gesc')) {
        engine.log.error('rt_main.js: scene load failed');
        return;
    }

    camera = engine.entity.find('MainCamera');
    if (!camera) {
        engine.log.error('rt_main.js: MainCamera not found');
        return;
    }

    // 默认：RT 反射开、SSR 关，让反射纯粹来自 ray query。
    engine.render.set_rt_reflection(true);
    engine.render.set_ssr(false);
    log_state();

    const q = camera.rotation();
    const f = rotate(q, [0, 0, -1]);
    pitch = Math.asin(clamp(f[1], -1, 1)) * RAD2DEG;
    yaw = Math.atan2(-f[0], -f[2]) * RAD2DEG;
    position = camera.position();

    engine.input.set_mouse_locked(true);
    engine.log.info('rt_main.js: camera ready, entities=' + engine.entity.count());
}

export function on_update(dt) {
    if (!camera) return;

    if (engine.input.key_down(KEY.ESC)) {
        engine.app.quit();
        return;
    }

    // 实时切换反射方案：R = 硬件光追，T = SSR（对比）。
    // engine.input 只暴露 key_down（电平），用上一帧状态做边沿检测。
    const rt_now = engine.input.key_down(KEY.R);
    if (rt_now && !rt_toggle_prev) {
        engine.render.set_rt_reflection(!engine.render.is_rt_reflection());
        log_state();
    }
    rt_toggle_prev = rt_now;

    const ssr_now = engine.input.key_down(KEY.T);
    if (ssr_now && !ssr_toggle_prev) {
        engine.render.set_ssr(!engine.render.is_ssr());
        log_state();
    }
    ssr_toggle_prev = ssr_now;

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
    engine.log.info('rt_main.js: destroy');
}
