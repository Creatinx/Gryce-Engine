// demo_spin.js — ScriptComponent 演示：绕 Y 轴匀速旋转 + 日志 + props
//
// 挂载：test/webgl_demo/scenes/main.gesc 的 CubeGlossy 实体。
// 与桌面 demo 的脚本同源；API 与约束见 docs/zh/scripting.md。

function quat_mul(a, b) {
    return [
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    ];
}

export function start() {
    engine.log.info('demo_spin(web): start, speed=' + this.props.speed);
}

export function update(dt) {
    const half = 0.5 * this.props.speed * dt;
    const s = Math.sin(half);
    const c = Math.cos(half);
    const delta = [0.0, s, 0.0, c];
    const q = engine.entity.rotation();
    engine.entity.set_rotation(quat_mul(delta, q));
}
