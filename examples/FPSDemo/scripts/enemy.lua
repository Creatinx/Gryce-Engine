-- enemy.lua — FPS 敌人：待机（停动画）→ 警戒范围内追击（播放动画）→ 近战攻击。
-- 纯脚本驱动（无碰撞体），玩家/子弹命中走距离判定。

props = {}

local common = require("common")

local ARENA_LIMIT = 19.0
local ENEMY_RADIUS = 0.5
-- 竞技场内的掩体 AABB（x/z 范围），用于阻止纯脚本敌人穿墙/穿掩体。
local COVERS = {
    { minx = 3.5, maxx = 6.5, minz = 2.2, maxz = 3.8 },
    { minx = -5.25, maxx = -2.75, minz = -3.6, maxz = -2.4 },
    { minx = 2.2, maxx = 3.8, minz = -8.4, maxz = -5.6 },
    { minx = -7.6, maxx = -4.4, minz = 8.5, maxz = 9.5 },
}

local self_h = 0
local rest_y = 0.0
local attack_cd = 0.0
local grace = 2.0            -- 出生缓冲期，给玩家反应时间
local chasing = false

local function clamp_arena(x, z)
    if x < -ARENA_LIMIT then x = -ARENA_LIMIT elseif x > ARENA_LIMIT then x = ARENA_LIMIT end
    if z < -ARENA_LIMIT then z = -ARENA_LIMIT elseif z > ARENA_LIMIT then z = ARENA_LIMIT end
    return x, z
end

local function push_out_of_covers(x, z)
    local nx, nz = x, z
    for _, c in ipairs(COVERS) do
        local cx = math.max(c.minx, math.min(x, c.maxx))
        local cz = math.max(c.minz, math.min(z, c.maxz))
        local dx, dz = x - cx, z - cz
        local d2 = dx * dx + dz * dz
        if d2 < 1e-9 then
            -- 圆心已进入掩体内部：沿最小穿透轴推出。
            local left = x - c.minx
            local right = c.maxx - x
            local top = z - c.minz
            local bottom = c.maxz - z
            local m = math.min(left, right, top, bottom)
            if m == left then
                nx = c.minx - ENEMY_RADIUS
            elseif m == right then
                nx = c.maxx + ENEMY_RADIUS
            elseif m == top then
                nz = c.minz - ENEMY_RADIUS
            else
                nz = c.maxz + ENEMY_RADIUS
            end
        else
            local d = math.sqrt(d2)
            if d < ENEMY_RADIUS then
                local push = ENEMY_RADIUS - d
                nx = x + dx / d * push
                nz = z + dz / d * push
            end
        end
    end
    return nx, nz
end

local function resolve_world_collision(x, z)
    x, z = clamp_arena(x, z)
    x, z = push_out_of_covers(x, z)
    return x, z
end

function on_start()
    self_h = engine.self()
    if self_h == 0 then return end
    local t = engine.entity.get_transform(self_h)
    if t then rest_y = t.pos.y end
    common.register_enemy(self_h, common.CFG.enemy_health)
    engine.component.set(self_h, "SkinnedMeshRenderer", "playing", false)
    attack_cd = 0.0
    grace = 2.0
    chasing = false
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 or not common.enemy_alive(self_h) then return end
    if engine.state.get("game_over") then return end

    if grace > 0 then
        grace = grace - dt
        return
    end

    local t = engine.entity.get_transform(self_h)
    if not t then return end

    local cam = engine.entity.find("PlayerCamera")
    local ct = cam ~= 0 and engine.entity.get_transform(cam) or nil
    if not ct then return end

    -- 玩家刚体中心在眼睛下方 EYE 高度
    local px, py = ct.pos.x, ct.pos.y - common.CFG.eye_height
    local pz = ct.pos.z
    local dx, dz = px - t.pos.x, pz - t.pos.z
    local dy = math.abs(py - t.pos.y)
    local dist = math.sqrt(dx * dx + dz * dz)

    local vx, vz = 0, 0
    if dist > common.CFG.enemy_attack_range then
        if dist <= common.CFG.enemy_aggro_range then
            if not chasing then
                chasing = true
                engine.component.set(self_h, "SkinnedMeshRenderer", "playing", true)
            end
            if dist > 0.001 then
                vx = dx / dist * common.CFG.enemy_chase_speed
                vz = dz / dist * common.CFG.enemy_chase_speed
            end
            t.rot = common.qnormalize(common.quat_from_yaw_pitch(math.atan(-dx, -dz), 0.0))
        elseif chasing then
            chasing = false
            engine.component.set(self_h, "SkinnedMeshRenderer", "playing", false)
        end
    else
        -- 攻击：垂直距离过大不打、无敌期间不打
        local inv = engine.state.get("invuln") or 0
        if attack_cd <= 0 and dy <= 2.0 and inv <= 0 then
            attack_cd = 1.0
            local hp = (engine.state.get("health") or 100) - common.CFG.enemy_damage
            if hp < 0 then hp = 0 end
            engine.state.set("health", hp)
            local sfx = engine.entity.find("SFX_Hurt")
            if sfx ~= 0 then engine.audio.play_on(sfx) end
        end
    end
    if attack_cd > 0 then attack_cd = attack_cd - dt end

    -- 移动并保持高度（做简单的竞技场/掩体碰撞，防止敌人穿墙）
    local next_x = t.pos.x + vx * dt
    local next_z = t.pos.z + vz * dt
    t.pos.x, t.pos.z = resolve_world_collision(next_x, next_z)
    t.pos.y = rest_y
    engine.entity.set_transform(self_h, t.pos, t.rot, t.scale)
end
