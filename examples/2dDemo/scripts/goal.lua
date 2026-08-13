-- Goal：玩家抵达终点旗帜 -> 过关（game_manager 负责切换关卡）。
props = {}

local self_h = 0

function on_start()
    self_h = engine.self()
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 then return end
    if engine.state.get("level_complete") then return end

    local player = engine.entity.find("Player")
    if player == 0 then return end
    local common = require("common")
    local pa = engine.entity.aabb(player)
    local ga = engine.entity.aabb(self_h)
    if common.aabb_overlap(pa, ga) then
        engine.state.set("level_complete", true)
        engine.state.set("complete_timer", 1.3)
        engine.state.set("score", (engine.state.get("score") or 0) + 100 +
                                  (engine.state.get("coins") or 0) * 5)
        engine.audio.play_on(engine.entity.find("SFX_Goal"))
        local fx = engine.entity.find("HitFx")
        if fx ~= 0 then
            engine.entity.set_transform(fx, { x = ga.x, y = ga.y, z = 0 })
            engine.fx.burst(fx)
        end
        engine.log.info("2dDemo: level complete!")
    end
end
