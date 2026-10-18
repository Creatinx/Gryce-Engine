// ===========================================================================
// engine/script/js_bindings.h
// engine.* 原生绑定（脚本侧 API 面）。
//
// 绑定经 ScriptHost 接口访问宿主（ScriptComponent 实现），不直接依赖组件
// 具体类型：绑定层可在单元测试里用桩宿主验证。宿主指针存进 JSContext 的
// context opaque（每个 ScriptComponent 独占一个 context，天然隔离）。
//
// v1 API 面（冻结，新增绑定走增量计划）：
//   engine.log.info/warn/error/debug(msg)      -> glog
//   engine.time.dt()/frame()/elapsed()         -> 宿主时钟
//   engine.input.key_down/key_up(code)         -> g_input_state
//   engine.input.mouse_x/y/mouse_delta/button/mouse_locked
//   engine.entity.self()                       -> 1（宿主实体句柄）
//   engine.entity.name/set_name
//   engine.entity.position/set_position        -> [x,y,z]
//   engine.entity.rotation/set_rotation        -> [x,y,z,w]（四元数）
//   engine.entity.scale/set_scale              -> [x,y,z]
//   engine.entity.translate(dx,dy,dz)
//   engine.entity.set_enabled(bool)
// 明确不在 v1：跨实体查询（engine.find）、engine.physics（二者都需要
// 组件到 Scene 的反向指针，当前 Entity 不持有，见 docs 排错表）。
// ===========================================================================

#pragma once

#include <cstdint>

extern "C" {
#include "quickjs.h"
}

namespace gryce_engine::scene {
class Entity;
}

namespace gryce_engine::script {

// 绑定层看到的宿主接口（ScriptComponent 实现）
struct ScriptHost {
    virtual ~ScriptHost() = default;
    // 宿主实体；单元测试桩可返回 nullptr（entity.* 绑定此时抛错）
    virtual scene::Entity* host_entity() = 0;
    virtual float host_dt() const = 0;
    virtual uint64_t host_frame() const = 0;
    virtual float host_elapsed() const = 0;
};

// 在 global 上安装 engine 对象（幂等；仅新建 context 后调用一次）。
void install_engine_bindings(JSContext* ctx, ScriptHost* host);

} // namespace gryce_engine::script
