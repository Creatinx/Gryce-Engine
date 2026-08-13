-- Bullet：直线飞行（RigidBody2D 速度），命中敌人销毁，超时/撞停消失。
props = {}

local self_h = 0
local life = 2.0

function on_start()
    self_h = engine.self()
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 then return end

    life = life - dt
    if life <= 0 then
        engine.entity.destroy(self_h)
        return
    end

    local v = engine.component.get(self_h, "RigidBody2D", "velocity")
    if v then
        if math.abs(v.x) + math.abs(v.y) < 40 then
            engine.entity.destroy(self_h) -- 撞墙停下
            return
        end
    end

    local common = require("common")
    local ba = engine.entity.aabb(self_h)
    for _, h in ipairs(engine.entity.find_all("Enemy")) do
        local b = engine.entity.aabb(h)
        if common.aabb_overlap(ba, b) then
            engine.entity.destroy(h)
            engine.entity.destroy(self_h)
            engine.state.set("score", (engine.state.get("score") or 0) + 25)
            engine.audio.play_on(engine.entity.find("SFX_Stomp"))
            local fx = engine.entity.find("HitFx")
            if fx ~= 0 then
                engine.entity.set_transform(fx, { x = b.x, y = b.y, z = 0 })
                engine.fx.burst(fx)
            end
            return
        end
    end
end
