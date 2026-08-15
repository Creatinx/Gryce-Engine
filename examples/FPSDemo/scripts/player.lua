-- player.lua — FPS 第一人称玩家：WASD 移动（RigidBody 水平速度）+ 鼠标视角 + 射击。
-- 玩家实体是不可见刚体；相机/武器每帧贴到眼睛位置。

props = {}

local common = require("common")

local self_h, cam_h, weapon_h = 0, 0, 0
local yaw, pitch = 0.0, 0.0
local shoot_cd, jump_cd = 0.0, 0.0

local BODY_REST = 0.8          -- 站立时刚体中心高度（底面贴地）
local EYE = common.CFG.eye_height

local function quat_yaw_only()
    return common.quat_from_yaw_pitch(yaw, 0.0)
end

local function get_vel(h)
    local v = engine.component.get(h, "RigidBody", "velocity")
    if not v then return 0, 0, 0 end
    return v.x or 0, v.y or 0, v.z or 0
end

local function set_vel(h, x, y, z)
    engine.component.set(h, "RigidBody", "velocity", { x = x, y = y, z = z })
end

local function spawn_bullet()
    local cam_t = engine.entity.get_transform(cam_h)
    if not cam_t then return end

    local q = common.qnormalize(common.quat_from_yaw_pitch(yaw, pitch))
    local fwd = common.normalize(common.qrotate(q, { x = 0, y = 0, z = -1 }))
    local right = common.qrotate(q, { x = 1, y = 0, z = 0 })
    local up = common.qrotate(q, { x = 0, y = 1, z = 0 })

    local b = engine.entity.create("Bullet")
    engine.entity.set_transform(b, {
        x = cam_t.pos.x + fwd.x * 0.95 + right.x * 0.28 - up.x * 0.22,
        y = cam_t.pos.y + fwd.y * 0.95 + right.y * 0.28 - up.y * 0.22,
        z = cam_t.pos.z + fwd.z * 0.95 + right.z * 0.28 - up.z * 0.22,
    }, { x = 0, y = 0, z = 0, w = 1 }, { x = 0.08, y = 0.08, z = 0.08 })

    engine.component.set(b, "MeshRenderer", "mesh_path", "res:/models/cube_pbr.obj")
    engine.component.set(b, "RigidBody", "mass", 0.1)
    engine.component.set(b, "RigidBody", "use_gravity", false)
    engine.component.set(b, "RigidBody", "restitution", 0)
    engine.component.set(b, "RigidBody", "linear_damping", 0)
    engine.component.set(b, "RigidBody", "velocity", {
        x = fwd.x * common.CFG.bullet_speed,
        y = fwd.y * common.CFG.bullet_speed,
        z = fwd.z * common.CFG.bullet_speed,
    })
    engine.component.set(b, "SphereCollider", "radius", 0.08)
    engine.component.set(b, "Script", "script_path", "res:/scripts/bullet.lua")

    local sfx = engine.entity.find("SFX_Shoot")
    if sfx ~= 0 then engine.audio.play_on(sfx) end
end

function on_start()
    self_h = engine.self()
    if self_h == 0 then return end
    cam_h = engine.entity.find("PlayerCamera")
    weapon_h = engine.entity.find("Weapon")
    yaw, pitch = 0.0, 0.0
    shoot_cd, jump_cd = 0.0, 0.0
    -- 请求鼠标锁定：独立 exe 由平台回调锁定；编辑器里点击锁定（编辑器不注册
    -- 该回调，仅更新核心状态，光标由编辑器点击控制）。
    engine.input.mouse_locked(true)
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 or engine.state.get("game_over") then return end

    local t = engine.entity.get_transform(self_h)
    if not t then return end

    -- 鼠标视角
    local dx, dy = engine.input.mouse_delta()
    yaw = yaw - dx * common.CFG.mouse_sens
    pitch = pitch - dy * common.CFG.mouse_sens
    if pitch > 1.55 then pitch = 1.55 elseif pitch < -1.55 then pitch = -1.55 end

    -- 移动方向（相对 yaw）
    local yq = quat_yaw_only()
    local fwd = common.qrotate(yq, { x = 0, y = 0, z = -1 })
    local right = common.qrotate(yq, { x = 1, y = 0, z = 0 })

    local in_f, in_b, in_l, in_r = 0, 0, 0, 0
    if engine.input.key_down(87) then in_f = 1 end  -- W
    if engine.input.key_down(83) then in_b = 1 end  -- S
    if engine.input.key_down(65) then in_l = 1 end  -- A
    if engine.input.key_down(68) then in_r = 1 end  -- D

    local move = {
        x = (in_r - in_l) * right.x + (in_f - in_b) * fwd.x,
        y = 0,
        z = (in_r - in_l) * right.z + (in_f - in_b) * fwd.z,
    }
    local mvlen = math.sqrt(move.x * move.x + move.z * move.z)
    if mvlen > 1 then move.x, move.z = move.x / mvlen, move.z / mvlen end

    -- 水平速度 + 保留垂直（重力/跳跃）
    local _, vy, _ = get_vel(self_h)
    local grounded = t.pos.y <= BODY_REST + 0.06

    if engine.input.key_down(32) and grounded and jump_cd <= 0
        and not engine.state.get("jumping") then
        vy = common.CFG.jump_force
        engine.state.set("jumping", true)
        jump_cd = 0.15
        local sfx = engine.entity.find("SFX_Jump")
        if sfx ~= 0 then engine.audio.play_on(sfx) end
    end
    if grounded and vy <= 0.2 then engine.state.set("jumping", false) end
    if jump_cd > 0 then jump_cd = jump_cd - dt end

    set_vel(self_h, move.x * common.CFG.move_speed, vy, move.z * common.CFG.move_speed)

    -- 刚体朝向仅用 yaw
    engine.entity.set_transform(self_h, nil, common.qnormalize(quat_yaw_only()), nil)

    -- 相机贴到眼睛
    if cam_h ~= 0 then
        engine.entity.set_transform(cam_h,
            { x = t.pos.x, y = t.pos.y + EYE, z = t.pos.z },
            common.qnormalize(common.quat_from_yaw_pitch(yaw, pitch)), nil)
    end

    -- 武器贴到相机右下方（模型枪口已对齐 -Z，无需校准旋转）
    if weapon_h ~= 0 then
        local wq = common.qnormalize(common.quat_from_yaw_pitch(yaw, pitch))
        local fview = common.qrotate(wq, { x = 0, y = 0, z = -1 })
        local rview = common.qrotate(wq, { x = 1, y = 0, z = 0 })
        local uview = common.qrotate(wq, { x = 0, y = 1, z = 0 })
        engine.entity.set_transform(weapon_h, {
            x = t.pos.x + fview.x * 0.55 + rview.x * 0.28 - uview.x * 0.22,
            y = t.pos.y + EYE + fview.y * 0.55 + rview.y * 0.28 - uview.y * 0.22,
            z = t.pos.z + fview.z * 0.55 + rview.z * 0.28 - uview.z * 0.22,
        }, wq, nil)
    end

    -- 射击（左键 / F）
    if shoot_cd > 0 then shoot_cd = shoot_cd - dt end
    if (engine.input.mouse_down(0) or engine.input.key_down(70)) and shoot_cd <= 0 then
        shoot_cd = 0.16
        spawn_bullet()
    end

    -- 掉落重生
    if t.pos.y < -20 then
        local lives = engine.state.get("lives") or 5
        if lives > 0 then
            engine.state.set("lives", lives - 1)
            local sp = engine.entity.find("SpawnPoint")
            if sp ~= 0 then
                local st = engine.entity.get_transform(sp)
                if st then
                    set_vel(self_h, 0, 0, 0)
                    engine.entity.set_transform(self_h, st.pos, nil, nil)
                end
            end
            engine.state.set("invuln", 1.5)
            local sfx = engine.entity.find("SFX_Hurt")
            if sfx ~= 0 then engine.audio.play_on(sfx) end
        else
            engine.state.set("game_over", true)
        end
    end

    -- 无敌倒计时
    local inv = engine.state.get("invuln") or 0
    if inv > 0 then
        inv = inv - dt
        if inv < 0 then inv = 0 end
        engine.state.set("invuln", inv)
    end
end
