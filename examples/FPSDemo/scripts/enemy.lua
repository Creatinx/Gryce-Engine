-- enemy.lua — FPS 敌人：向玩家追击，近战攻击，受子弹伤害。
props = {}

local common = require("common")

local self_h = 0
local spawn_pos = { x = 0, y = 0, z = 0 }
local rest_y = 0.0
local attack_cooldown = 0.0
local grace_timer = 0.0   -- 出生缓冲期：玩家有反应时间，避免出生即被围殴
local wander_angle = 0.0
local wander_timer = 0.0

local function get_player_h()
    return engine.entity.find("PlayerCamera")
end

function on_start()
    self_h = engine.self()
    if self_h == 0 then return end
    local t = engine.entity.get_transform(self_h)
    if t then
        spawn_pos = { x = t.pos.x, y = t.pos.y, z = t.pos.z }
        rest_y = t.pos.y
    end
    common.register_enemy(self_h, common.CFG.enemy_health)
    attack_cooldown = 0.0
    grace_timer = 2.0
    wander_angle = 0.0
    wander_timer = 0.0
end

local function get_player_pos()
    local cam = get_player_h()
    if cam == 0 then return nil end
    local ct = engine.entity.get_transform(cam)
    if not ct then return nil end
    -- 玩家刚体中心在眼睛下方 EYE 高度
    return { x = ct.pos.x, y = ct.pos.y - common.CFG.eye_height, z = ct.pos.z }
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 then return end

    if not common.enemy_alive(self_h) then return end
    if engine.state.get("game_over") then return end

    -- 出生缓冲期：敌方原地待命，给玩家反应与瞄准时间
    if grace_timer > 0 then
        grace_timer = grace_timer - dt
        return
    end

    local t = engine.entity.get_transform(self_h)
    if not t then return end

    local player_pos = get_player_pos()
    if not player_pos then return end

    local dx = player_pos.x - t.pos.x
    local dz = player_pos.z - t.pos.z
    local dy = math.abs(player_pos.y - t.pos.y)
    local dist = math.sqrt(dx * dx + dz * dz)

    local vx, vz = 0, 0

    if dist > common.CFG.enemy_attack_range then
        -- 玩家在警戒范围内才追击，否则保持原地（避免全图敌人蜂拥而至）
        if dist <= common.CFG.enemy_aggro_range then
            if dist > 0.001 then
                vx = dx / dist * common.CFG.enemy_chase_speed
                vz = dz / dist * common.CFG.enemy_chase_speed
            end
            -- 面向玩家
            local target_yaw = math.atan(-dx, -dz)
            t.rot = common.qnormalize(common.quat_from_yaw_pitch(target_yaw, 0.0))
        end
    else
        -- 攻击
        -- 垂直距离过大（玩家在掩体/高处）不打；无敌期间不打
        local invuln = engine.state.get("invuln") or 0
        if attack_cooldown <= 0 and dy <= 2.0 and invuln <= 0 then
            attack_cooldown = 1.0
            local health = engine.state.get("health") or 100
            health = health - common.CFG.enemy_damage
            if health < 0 then health = 0 end
            engine.state.set("health", health)
            local sfx = engine.entity.find("SFX_Hurt")
            if sfx ~= 0 then engine.audio.play_on(sfx) end
        end
    end

    if attack_cooldown > 0 then attack_cooldown = attack_cooldown - dt end

    -- 移动并保持高度
    t.pos.x = t.pos.x + vx * dt
    t.pos.z = t.pos.z + vz * dt
    t.pos.y = rest_y
    engine.entity.set_transform(self_h, t.pos, t.rot, t.scale)
end
