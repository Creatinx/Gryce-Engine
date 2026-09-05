// fixture: on_update 抛异常的脚本，用于测试 PAUSED_ERROR 暂停
export function on_start() {
}

export function on_update(dt) {
    throw new Error("boom");
}
