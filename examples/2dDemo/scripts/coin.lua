-- Coin：旋转动画 + 玩家靠近收集（+10 分 / +1 金币）。
props = {}

local self_h = 0

function on_start()
    self_h = engine.self()
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 then return end

    local t = engine.entity.get_transform(self_h)
    if t then
        t.rot.z = t.rot.z + dt * 2.5
        engine.entity.set_transform(self_h, t.pos, t.rot, t.scale)
    end

    local player = engine.entity.find("Player")
    if player == 0 then return end
    local pt = engine.entity.get_transform(player)
    local ct = engine.entity.get_transform(self_h)
    if not pt or not ct then return end
    local dx, dy = pt.pos.x - ct.pos.x, pt.pos.y - ct.pos.y
    if dx * dx + dy * dy <= 24 * 24 then
        engine.state.set("coins", (engine.state.get("coins") or 0) + 1)
        engine.state.set("score", (engine.state.get("score") or 0) + 10)
        engine.audio.play_on(engine.entity.find("SFX_Coin"))
        engine.entity.destroy(self_h)
    end
end
