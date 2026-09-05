// fixture: 正常脚本，用于 ScriptSystem 测试
export const props = { hp: 100, name: "player" };

export function on_start() {
    start_count = 1;
}

export function on_update(dt) {
    exports.props.hp = exports.props.hp + 1;
}

export function on_destroy() {
    destroyed = true;
}

export function get_hp() {
    return exports.props.hp;
}

export function set_hp(v) {
    exports.props.hp = v;
}
