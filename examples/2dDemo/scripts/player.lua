-- Player：移动 / 跳跃 / 鼠标瞄准射击（枪械升级三发）/ 受伤 / 掉落重生。
props = {}

local common = require("common")

local self_h = 0
local spawn = { x = 0, y = 0 }
local jump_pressed_last = false
local jump_cooldown = 0
local shoot_cooldown = 0
local invuln = 0

local function get_vel(h)
    local v = engine.component.get(h, "RigidBody2D", "velocity")
    return (v and v.x or 0), (v and v.y or 0)
end

local function set_vel(h, x, y)
    engine.component.set(h, "RigidBody2D", "velocity", { x = x, y = y })
end

local function set_pos(h, x, y)
    local t = engine.entity.get_transform(h)
    if t then
        t.pos.x = x
        t.pos.y = y
        engine.entity.set_transform(h, t.pos, t.rot, t.scale)
    end
end

local function respawn()
    set_pos(self_h, spawn.x, spawn.y)
    set_vel(self_h, 0, 0)
end

local function lose_life()
    local lives = (engine.state.get("lives") or 3) - 1
    engine.state.set("lives", lives)
    if lives <= 0 then
        engine.state.set("lives", 0)
        engine.state.set("game_over", true)
        return false
    end
    return true
end

local function burst_fx_at(name, x, y)
    local fx = engine.entity.find(name)
    if fx ~= 0 then
        engine.entity.set_transform(fx, { x = x, y = y, z = 0 })
        engine.fx.burst(fx)
    end
end

local function spawn_bullet(dx, dy, angle)
    local h = engine.entity.create("Bullet")
    local t = engine.entity.get_transform(self_h)
    if not t then
        engine.entity.destroy(h)
        return
    end
    local a = math.atan2(dy, dx) + angle
    local bdx, bdy = math.cos(a), math.sin(a)
    engine.entity.set_transform(h, { x = t.pos.x + bdx * 20, y = t.pos.y + bdy * 20, z = 0 })
    engine.component.set(h, "Sprite2D", "texture_path", "res:/textures/bullet.png")
    engine.component.set(h, "Sprite2D", "width", 12)
    engine.component.set(h, "Sprite2D", "height", 12)
    engine.component.set(h, "Sprite2D", "color", { r = 1, g = 0.95, b = 0.40, a = 1 })
    engine.component.set(h, "Sprite2D", "lit", true)
    engine.component.set(h, "Sprite2D", "render_order", 20)
    engine.component.set(h, "RigidBody2D", "mass", 0.05)
    engine.component.set(h, "RigidBody2D", "use_gravity", false)
    engine.component.set(h, "RigidBody2D", "is_kinematic", false)
    engine.component.set(h, "RigidBody2D", "restitution", 0)
    engine.component.set(h, "RigidBody2D", "friction", 0)
    engine.component.set(h, "CircleCollider2D", "radius", 5)
    engine.component.set(h, "RigidBody2D", "velocity", { x = bdx * 720, y = bdy * 720 })
    engine.component.set(h, "Light2D", "color", { r = 1, g = 0.85, b = 0.25, a = 1 })
    engine.component.set(h, "Light2D", "intensity", 2.5)
    engine.component.set(h, "Light2D", "radius", 90)
    engine.component.set(h, "Light2D", "render_order", 100)
    engine.component.set(h, "Script", "script_path", "res:/scripts/bullet.lua")

    -- 枪口火光 + 音效（只放一次）
    burst_fx_at("MuzzleFlash", t.pos.x + bdx * 22, t.pos.y + bdy * 22)
    if angle == 0 then
        engine.audio.play_on(engine.entity.find("SFX_Shoot"))
    end
end

function on_start()
    self_h = engine.self()
    if self_h == 0 then return end
    local sp = engine.entity.find("SpawnPoint")
    if sp ~= 0 then
        local t = engine.entity.get_transform(sp)
        if t then spawn = { x = t.pos.x, y = t.pos.y } end
    else
        local t = engine.entity.get_transform(self_h)
        if t then spawn = { x = t.pos.x, y = t.pos.y } end
    end
    engine.log.info("2dDemo: player script started (spawn=" .. spawn.x .. "," .. spawn.y .. ")")
    jump_pressed_last = false
    jump_cooldown = 0
    shoot_cooldown = 0
    invuln = 0
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 then return end

    if engine.state.get("game_over") or engine.state.get("victory") or
       engine.state.get("level_complete") then
        return
    end

    local info = common.current_level()
    local gravity = info.gravity or 1150
    local vx, vy = get_vel(self_h)

    -- 移动
    local left = engine.input.key_down(65) or engine.input.key_down(263)
    local right = engine.input.key_down(68) or engine.input.key_down(262)
    local jump = engine.input.key_down(32) or engine.input.key_down(87) or engine.input.key_down(265)
    local target_vx = 0
    if left then target_vx = -240 end
    if right then target_vx = 240 end
    set_vel(self_h, target_vx, vy)

    -- 跳跃（按当前关重力反推起跳速度，弹跳高度一致）
    local grounded = math.abs(vy) < 18
    if jump and not jump_pressed_last and grounded and jump_cooldown <= 0 then
        local jump_speed = math.sqrt(2 * gravity * 6 * 32)
        set_vel(self_h, target_vx, -jump_speed)
        jump_cooldown = 0.18
        engine.audio.play_on(engine.entity.find("SFX_Jump"))
        local t = engine.entity.get_transform(self_h)
        if t then burst_fx_at("JumpDust", t.pos.x, t.pos.y) end
    end
    jump_pressed_last = jump
    if jump_cooldown > 0 then jump_cooldown = jump_cooldown - dt end

    local t = engine.entity.get_transform(self_h)
    if not t then return end

    -- 瞄准：玩家聚光灯指向鼠标世界坐标
    local mx, my = engine.input.mouse_pos()
    local cam = engine.entity.find("MainCamera")
    local zoom = 1
    local camx, camy = 640, 360
    if cam ~= 0 then
        zoom = engine.component.get(cam, "Camera2D", "zoom") or 1
        local ct = engine.entity.get_transform(cam)
        if ct then camx, camy = ct.pos.x, ct.pos.y end
    end
    local wx = camx + (mx - 640) / zoom
    local wy = camy + (my - 360) / zoom
    local dx, dy = wx - t.pos.x, wy - t.pos.y
    local len = math.sqrt(dx * dx + dy * dy)
    if len > 1 then
        engine.component.set(self_h, "Light2D", "direction", { x = dx / len, y = dy / len })
    end

    -- 射击（鼠标左键 / Z）
    local shooting = engine.input.mouse_down(0) or engine.input.key_down(90)
    if shooting and shoot_cooldown <= 0 then
        shoot_cooldown = (engine.state.get("gun_level") or 1) >= 2 and 0.15 or 0.24
        local dirx, diry = 1, 0
        if len > 1 then dirx, diry = dx / len, dy / len end
        if (engine.state.get("gun_level") or 1) >= 2 then
            spawn_bullet(dirx, diry, -0.12)
            spawn_bullet(dirx, diry, 0)
            spawn_bullet(dirx, diry, 0.12)
        else
            spawn_bullet(dirx, diry, 0)
        end
    end
    if shoot_cooldown > 0 then shoot_cooldown = shoot_cooldown - dt end

    -- 关卡边界 + 掉落
    local world_w, world_h = common.world_bounds()
    if t.pos.x < 32 then t.pos.x = 32 end
    if t.pos.x > world_w - 32 then t.pos.x = world_w - 32 end
    if t.pos.y > world_h + 100 then
        if lose_life() then
            respawn()
            invuln = 1.2
            engine.audio.play_on(engine.entity.find("SFX_Hurt"))
        end
    end

    -- 无敌闪烁
    if invuln > 0 then
        invuln = invuln - dt
        engine.component.set(self_h, "Sprite2D", "enabled", math.floor(invuln * 10) % 2 == 0)
    else
        engine.component.set(self_h, "Sprite2D", "enabled", true)
    end

    -- 敌人交互：踩踏（上方下落）优先，否则受伤
    local pa = engine.entity.aabb(self_h)
    local hurt = false
    for _, h in ipairs(engine.entity.find_all("Enemy")) do
        local b = engine.entity.aabb(h)
        if common.aabb_overlap(pa, b) then
            local _, vy2 = get_vel(self_h)
            local from_top = (pa.y - pa.h / 2) < (b.y - b.h / 2) + 12
            if from_top and vy2 >= -20 then
                engine.entity.destroy(h)
                set_vel(self_h, select(1, get_vel(self_h)), -320)
                engine.state.set("score", (engine.state.get("score") or 0) + 50)
                engine.audio.play_on(engine.entity.find("SFX_Stomp"))
                burst_fx_at("HitFx", b.x, b.y)
            else
                hurt = true
            end
        end
    end

    -- 尖刺伤害
    if invuln <= 0 and not hurt then
        for _, h in ipairs(engine.entity.find_all("Spike")) do
            if common.aabb_overlap(pa, engine.entity.aabb(h)) then
                hurt = true
                break
            end
        end
    end

    -- 受伤
    if hurt and invuln <= 0 then
        if lose_life() then
            invuln = 1.2
            engine.audio.play_on(engine.entity.find("SFX_Hurt"))
            local dir = 1
            if pa.x > world_w * 0.5 then dir = -1 end
            set_vel(self_h, dir * 220, -280)
            burst_fx_at("HitFx", t.pos.x, t.pos.y)
        end
    end
end
