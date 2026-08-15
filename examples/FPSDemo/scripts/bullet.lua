-- bullet.lua — FPS 子弹：射线检测伤害，超时/撞击后销毁。
props = {}

local common = require("common")

local self_h = 0
local lifetime = 0.0
-- 上一帧位置，用于做“线段”命中检测（避免高速子弹穿透）
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
    local cx = ax + abx * tt
    local cy = ay + aby * tt
    local cz = az + abz * tt
    local dx, dy, dz = px - cx, py - cy, pz - cz
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

function on_start()
    self_h = engine.self()
    lifetime = common.CFG.bullet_life
    local t = engine.entity.get_transform(self_h)
    if t then
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

    -- 撞墙/落地检测：子弹被物理阻挡后速度归零（restitution=0 不反弹），
    -- 直接销毁，避免穿透或贴墙滞留到寿命结束。
    local v = engine.component.get(self_h, "RigidBody", "velocity")
    if v then
        local speed = math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
        if speed < 2.0 then
            engine.entity.destroy(self_h)
            return
        end
    end

    -- 用本帧位移 + 命中半径做“线段”检测：高速子弹整段路径都能命中，不再穿透
    local hit_range = common.CFG.bullet_hit_range
    local nearest_d = 999
    local nearest_enemy = 0

    for enemy_h, _ in pairs(common.ENEMIES) do
        if common.enemy_alive(enemy_h) then
            local et = engine.entity.get_transform(enemy_h)
            if et then
                local d = dist_point_to_segment(
                    et.pos.x, et.pos.y, et.pos.z,
                    prev_x, prev_y, prev_z,
                    t.pos.x, t.pos.y, t.pos.z)
                if d < hit_range and d < nearest_d then
                    nearest_d = d
                    nearest_enemy = enemy_h
                end
            end
        end
    end

    prev_x, prev_y, prev_z = t.pos.x, t.pos.y, t.pos.z

    if nearest_enemy ~= 0 then
        -- 命中敌人，扣血后检查死亡
        local dead = common.damage_enemy(nearest_enemy, common.CFG.bullet_damage)
        if dead then
            engine.entity.destroy(nearest_enemy)
            local k = (engine.state.get("kills") or 0) + 1
            engine.state.set("kills", k)
            local sfx = engine.entity.find("SFX_EnemyDead")
            if sfx ~= 0 then engine.audio.play_on(sfx) end
        else
            local sfx = engine.entity.find("SFX_Hit")
            if sfx ~= 0 then engine.audio.play_on(sfx) end
        end
        engine.entity.destroy(self_h)
        return
    end
end
