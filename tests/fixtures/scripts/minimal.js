// fixture: 仅含 on_update，无 on_start/on_destroy（测试可选生命周期方法）
export const props = { hp: 7 };

export function on_update(dt) {
    exports.props.hp = 7;
}
