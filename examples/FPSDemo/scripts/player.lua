-- player.lua — FPS 第一人称玩家：WASD 移动（RigidBody 水平速度）+ 鼠标视角 + 射击。
-- 玩家实体为不可见刚体（含主相机），相机/武器每帧贴到角色眼睛位置。
props = {}

local common = require("common")

local self_h = 0
local cam_h = 0
local weapon_h = 0

local yaw = 0.0
local pitch = 0.0
local mouse_init = false

local shoot_cooldown = 0.0
local jump_cooldown = 0.0

-- 地面/身体参数
local GROUND_Y = 0.0
local BODY_REST = 0.8          -- 站立时刚体中心高度（底面贴地）
local EYE = common.CFG.eye_height

-- 模型校准：m1911.glb 根节点是 Z-up→Y-up 旋转（绕 X 轴 -90°），但导入器未将该根节点
-- 变换烘焙进网格，网格局部空间里枪管沿 +Y（朝天）。此四元数（绕 X -90°）把枪管转到
-- -Z（前向），使枪械朝向与相机朝向一致。
local WEAPON_CALIB = common.qnormalize({ x = -0.70710678, y = 0, z = 0, w = 0.70710678 })

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
    -- 瞄准方向 = 相机完整朝向的 forward
    local rot = common.quat_from_yaw_pitch(yaw, pitch)
    local q = common.qnormalize(rot)
    local fwd = common.qrotate(q, { x = 0, y = 0, z = -1 })
    local right = common.qrotate(q, { x = 1, y = 0, z = 0 })
    local up = common.qrotate(q, { x = 0, y = 1, z = 0 })
    fwd = common.normalize(fwd)

    -- 从枪口射出：枪身位于眼位右下方前方，弹道与枪口对齐（枪身 0.55 + 枪管长约 0.4 → 0.95）
    local b = engine.entity.create("Bullet")
    local origin = {
        x = cam_t.pos.x + fwd.x * 0.95 + right.x * 0.28 - up.x * 0.22,
        y = cam_t.pos.y + fwd.y * 0.95 + right.y * 0.28 - up.y * 0.22,
        z = cam_t.pos.z + fwd.z * 0.95 + right.z * 0.28 - up.z * 0.22,
    }
    engine.entity.set_transform(b, origin, { x = 0, y = 0, z = 0, w = 1 }, { x = 0.08, y = 0.08, z = 0.08 })
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

    -- 枪声
    local sfx = engine.entity.find("SFX_Shoot")
    if sfx ~= 0 then engine.audio.play_on(sfx) end
end

function on_start()
    self_h = engine.self()
    if self_h == 0 then return end
    cam_h = engine.entity.find("PlayerCamera")
    weapon_h = engine.entity.find("Weapon")
    yaw = 0.0
    pitch = 0.0
    mouse_init = true
    shoot_cooldown = 0.0
    jump_cooldown = 0.0
    -- 锁定并隐藏鼠标，实现 FPS 视角
    engine.input.mouse_locked(true)
    engine.log.info("FPSDemo: player script started")
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 then return end

    if engine.state.get("game_over") then return end

    local t = engine.entity.get_transform(self_h)
    if not t then return end

    -- 鼠标视角：使用本帧增量（锁定后绝对位置受限，delta 才可靠）
    local dx, dy = engine.input.mouse_delta()
    yaw = yaw - dx * common.CFG.mouse_sens
    pitch = pitch - dy * common.CFG.mouse_sens
    if pitch > 1.55 then pitch = 1.55 end
    if pitch < -1.55 then pitch = -1.55 end

    -- 移动方向（相对 yaw）
    local yq = quat_yaw_only()
    local fwd = common.qrotate(yq, { x = 0, y = 0, z = -1 })
    local right = common.qrotate(yq, { x = 1, y = 0, z = 0 })

    local in_fwd, in_back, in_l, in_r = 0, 0, 0, 0
    if engine.input.key_down(87) then in_fwd = 1 end   -- W
    if engine.input.key_down(83) then in_back = 1 end  -- S
    if engine.input.key_down(65) then in_l = 1 end     -- A
    if engine.input.key_down(68) then in_r = 1 end     -- D

    local move = {
        x = (in_r - in_l) * right.x + (in_fwd - in_back) * fwd.x,
        y = 0,
        z = (in_r - in_l) * right.z + (in_fwd - in_back) * fwd.z,
    }
    local mvlen = math.sqrt(move.x * move.x + move.z * move.z)
    if mvlen > 1 then
        move.x = move.x / mvlen
        move.z = move.z / mvlen
    end

    -- 读取当前速度，改写水平分量，保留垂直（重力/跳跃）
    local vx, vy, vz = get_vel(self_h)
    local target_vx = move.x * common.CFG.move_speed
    local target_vz = move.z * common.CFG.move_speed

    -- 接地检测（平台游戏近似：地面为平面 y=GROUND_Y）
    local grounded = t.pos.y <= BODY_REST + 0.06

    -- 跳跃
    -- 跳跃键：仅空格（265 是无效键码，已移除）
    local jump = engine.input.key_down(32)
    if jump and grounded and jump_cooldown <= 0 and not engine.state.get("jumping") then
        target_vx = target_vx  -- 保持水平
        vy = common.CFG.jump_force
        engine.state.set("jumping", true)
        jump_cooldown = 0.15
        local sfx = engine.entity.find("SFX_Jump")
        if sfx ~= 0 then engine.audio.play_on(sfx) end
    end
    if grounded and vy <= 0.2 then
        engine.state.set("jumping", false)
    end
    if jump_cooldown > 0 then jump_cooldown = jump_cooldown - dt end

    set_vel(self_h, target_vx, vy, target_vz)

    -- 刚体朝向仅用 yaw（保持直立），避免物理翻转
    local body_rot = quat_yaw_only()
    engine.entity.set_transform(self_h, nil, common.qnormalize(body_rot), nil)

    -- 相机贴到眼睛位置
    if cam_h ~= 0 then
        local cam_pos = { x = t.pos.x, y = t.pos.y + EYE, z = t.pos.z }
        local cam_rot = common.quat_from_yaw_pitch(yaw, pitch)
        engine.entity.set_transform(cam_h, cam_pos, common.qnormalize(cam_rot), nil)
    end

    -- 武器贴到相机右下方
    if weapon_h ~= 0 then
        local wep_rot = common.quat_from_yaw_pitch(yaw, pitch)
        local wq = common.qnormalize(wep_rot)
        local fview = common.qrotate(wq, { x = 0, y = 0, z = -1 })
        local rview = common.qrotate(wq, { x = 1, y = 0, z = 0 })
        local uview = common.qrotate(wq, { x = 0, y = 1, z = 0 })
        local cam_pos = { x = t.pos.x, y = t.pos.y + EYE, z = t.pos.z }
        local wp = {
            x = cam_pos.x + fview.x * 0.55 + rview.x * 0.28 - uview.x * 0.22,
            y = cam_pos.y + fview.y * 0.55 + rview.y * 0.28 - uview.y * 0.22,
            z = cam_pos.z + fview.z * 0.55 + rview.z * 0.28 - uview.z * 0.22,
        }
        -- 武器朝向 = 相机朝向 * 模型校准旋转（先本地校准，再跟随视角）
        local weapon_rot = common.qnormalize(common.qmul(wq, WEAPON_CALIB))
        engine.entity.set_transform(weapon_h, wp, weapon_rot, nil)
    end

    -- 射击（左键 / 空格旁的 F 键 70）
    if shoot_cooldown > 0 then shoot_cooldown = shoot_cooldown - dt end
    local shooting = engine.input.mouse_down(0) or engine.input.key_down(70)
    if shooting and shoot_cooldown <= 0 then
        shoot_cooldown = 0.16
        spawn_bullet()
    end

    -- 掉落重生
    if t.pos.y < -20 then
        local lives = engine.state.get("lives") or 5
        if lives > 0 then
            engine.state.set("lives", lives - 1)
            -- 回到出生点
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

    -- 无敌倒计时（存入 engine.state，供 enemy.lua 攻击时检查）
    local inv = engine.state.get("invuln") or 0
    if inv > 0 then
        inv = inv - dt
        if inv < 0 then inv = 0 end
        engine.state.set("invuln", inv)
    end
end
