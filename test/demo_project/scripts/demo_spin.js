// demo_spin.js — ScriptComponent 演示：绕 Y 轴匀速旋转 + 日志 + props
//
// 挂载：test/demo_project/scenes/main.gesc 的 ScriptCube 实体。
// 生命周期钩子（模块导出即钩子）：
//   start()   场景开始后一次
//   update(dt) 每帧；this 为组件实例对象，this.props 为场景序列化注入的属性
// 引擎 API：engine.log / engine.time / engine.entity（见 docs/zh/scripting.md）

function quat_mul(a, b) {
    return [
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    ];
}

export function start() {
    engine.log.info('demo_spin: start, speed=' + this.props.speed);
}

export function update(dt) {
    // 绕 Y 轴的增量旋转（四元数），按帧时间累加，与帧率无关
    const half = 0.5 * this.props.speed * dt;
    const s = Math.sin(half);
    const c = Math.cos(half);
    const delta = [0.0, s, 0.0, c];
    const q = engine.entity.rotation();
    engine.entity.set_rotation(quat_mul(delta, q));
}
