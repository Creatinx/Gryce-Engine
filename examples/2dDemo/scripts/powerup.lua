-- PowerUp：枪械升级拾取（Lv.1 单发 -> Lv.2 三发散射）。
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
        t.rot.z = t.rot.z + dt * 3.0
        engine.entity.set_transform(self_h, t.pos, t.rot, t.scale)
    end

    local player = engine.entity.find("Player")
    if player == 0 then return end
    local pt = engine.entity.get_transform(player)
    local ct = engine.entity.get_transform(self_h)
    if not pt or not ct then return end
    local dx, dy = pt.pos.x - ct.pos.x, pt.pos.y - ct.pos.y
    if dx * dx + dy * dy <= 26 * 26 then
        local gun = engine.state.get("gun_level") or 1
        if gun < 2 then
            engine.state.set("gun_level", gun + 1)
        end
        engine.state.set("score", (engine.state.get("score") or 0) + 50)
        engine.audio.play_on(engine.entity.find("SFX_Powerup"))
        local fx = engine.entity.find("HitFx")
        if fx ~= 0 then
            engine.entity.set_transform(fx, { x = ct.pos.x, y = ct.pos.y, z = 0 })
            engine.fx.burst(fx)
        end
        engine.entity.destroy(self_h)
    end
end
