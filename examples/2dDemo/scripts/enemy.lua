-- Enemy：按 PatrolLeft_EnemyN / PatrolRight_EnemyN 两个标记实体巡逻。
props = {}

local self_h = 0
local min_x, max_x = 0, 0
local dir = 1
local speed = 90

function on_start()
    self_h = engine.self()
    if self_h == 0 then return end
    local name = engine.entity.get_name(self_h)
    local idx = name:match("^Enemy(%d+)$") or ""
    local base = 0
    local t = engine.entity.get_transform(self_h)
    if t then base = t.pos.x end
    local left = engine.entity.find("PatrolLeft_Enemy" .. idx)
    local right = engine.entity.find("PatrolRight_Enemy" .. idx)
    if left ~= 0 then
        local lt = engine.entity.get_transform(left)
        if lt then min_x = lt.pos.x end
    end
    if right ~= 0 then
        local rt = engine.entity.get_transform(right)
        if rt then max_x = rt.pos.x end
    end
    if max_x <= min_x then
        min_x, max_x = base - 64, base + 64
    end
    local common = require("common")
    speed = common.current_enemy_speed()
end

function on_update(dt)
    if self_h == 0 then return end
    local t = engine.entity.get_transform(self_h)
    if not t then return end
    local x = t.pos.x
    if x <= min_x then dir = 1 end
    if x >= max_x then dir = -1 end
    local v = engine.component.get(self_h, "RigidBody2D", "velocity")
    engine.component.set(self_h, "RigidBody2D", "velocity",
                         { x = speed * dir, y = v and v.y or 0 })
end
