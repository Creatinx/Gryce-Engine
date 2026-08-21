-- bullet.lua — 子弹：物理飞行 + 线段命中判定（高速不穿透）+ 撞墙销毁。

props = {}

local common = require("common")

local self_h = 0
local lifetime = 0.0
local prev_x, prev_y, prev_z = nil, nil, nil

-- 点 P 到线段 AB 的最短距离
local function dist_point_to_segment(px, py, pz, ax, ay, az, bx, by, bz)
    local abx, aby, abz = bx - ax, by - ay, bz - az
    local apx, apy, apz = px - ax, py - ay, pz - az
    local ab2 = abx * abx + aby * aby + abz * abz
    if ab2 <= 1e-9 then
        return math.sqrt(apx * apx + apy * apy + apz * apz)
    end
    local tt = (apx * abx + apy * aby + apz * abz) / ab2
    if tt < 0 then tt = 0 elseif tt > 1 then tt = 1 end
    local cx, cy, cz = ax + abx * tt, ay + aby * tt, az + abz * tt
    local ddx, ddy, ddz = px - cx, py - cy, pz - cz
    return math.sqrt(ddx * ddx + ddy * ddy + ddz * ddz)
end

function on_start()
    self_h = engine.self()
    lifetime = common.CFG.bullet_life
    local spawn = engine.state.get("bullet_spawn_" .. tostring(self_h))
    if spawn then
        prev_x, prev_y, prev_z = spawn.x, spawn.y, spawn.z
        engine.state.set("bullet_spawn_" .. tostring(self_h), nil)
    end
    local t = engine.entity.get_transform(self_h)
    if t and prev_x == nil then
        prev_x, prev_y, prev_z = t.pos.x, t.pos.y, t.pos.z
    end
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 then return end

    lifetime = lifetime - dt
    if lifetime <= 0 then
        engine.entity.destroy(self_h)
        return
    end

    local t = engine.entity.get_transform(self_h)
    if not t then return end
    if prev_x == nil then prev_x, prev_y, prev_z = t.pos.x, t.pos.y, t.pos.z end

    -- 撞墙/落地：物理停住（restitution=0 不反弹）即销毁，避免穿透或贴墙滞留
    local v = engine.component.get(self_h, "RigidBody", "velocity")
    if v then
        local speed = math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
        if speed < 2.0 then
            engine.entity.destroy(self_h)
            return
        end
    end

    -- 线段命中敌人
    local range = common.CFG.bullet_hit_range
    local nearest_d, nearest = 999, 0
    for h, _ in pairs(common.ENEMIES) do
        if common.enemy_alive(h) then
            local et = engine.entity.get_transform(h)
            if et then
                local d = dist_point_to_segment(
                    et.pos.x, et.pos.y, et.pos.z,
                    prev_x, prev_y, prev_z,
                    t.pos.x, t.pos.y, t.pos.z)
                if d < range and d < nearest_d then
                    nearest_d, nearest = d, h
                end
            end
        end
    end

    prev_x, prev_y, prev_z = t.pos.x, t.pos.y, t.pos.z

    if nearest ~= 0 then
        if common.damage_enemy(nearest, common.CFG.bullet_damage) then
            engine.entity.destroy(nearest)
            engine.state.set("kills", (engine.state.get("kills") or 0) + 1)
            local sfx = engine.entity.find("SFX_EnemyDead")
            if sfx ~= 0 then engine.audio.play_on(sfx) end
        else
            local sfx = engine.entity.find("SFX_Hit")
            if sfx ~= 0 then engine.audio.play_on(sfx) end
        end
        engine.entity.destroy(self_h)
    end
end
