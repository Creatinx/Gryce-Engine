-- common.lua — FPSDemo 共享工具：四元数/向量数学、敌人生命表、游戏参数。
-- 模块级状态（ENEMIES）在脚本实例间共享，且跨场景重载持久存在：
-- 场景重载后运行时句柄映射被清空，enemy_alive 用 get_name 校验过滤陈旧句柄。

local M = {}

M.ENEMIES = {}   -- handle -> hp

M.CFG = {
    move_speed = 6.0,
    jump_force = 7.0,
    eye_height = 0.8,           -- 相机相对刚体中心的抬升
    mouse_sens = 0.0016,        -- 弧度/像素（修复鼠标增量累积后，略低于原值）
    bullet_speed = 220.0,
    bullet_damage = 34,
    bullet_life = 2.5,
    bullet_hit_range = 1.1,     -- 命中判定半径
    enemy_health = 100,
    enemy_damage = 16,
    enemy_attack_range = 3.2,
    enemy_aggro_range = 16.0,   -- 超出不追击
    enemy_chase_speed = 7.5,    -- 玩家移动 6.0，追击必须更快
}

function M.distance3(a, b)
    local dx, dy, dz = a.x - b.x, a.y - b.y, a.z - b.z
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

-- 四元数乘法（xyzw）
function M.qmul(a, b)
    return {
        x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    }
end

-- 用四元数旋转向量
function M.qrotate(q, v)
    local uv = {
        x = q.y * v.z - q.z * v.y,
        y = q.z * v.x - q.x * v.z,
        z = q.x * v.y - q.y * v.x,
    }
    local uuv = {
        x = q.y * uv.z - q.z * uv.y,
        y = q.z * uv.x - q.x * uv.z,
        z = q.x * uv.y - q.y * uv.x,
    }
    return {
        x = v.x + 2.0 * (q.w * uv.x + uuv.x),
        y = v.y + 2.0 * (q.w * uv.y + uuv.y),
        z = v.z + 2.0 * (q.w * uv.z + uuv.z),
    }
end

-- 由 yaw（绕 Y）/pitch（绕 X）组合四元数（前向 -Z 约定）
function M.quat_from_yaw_pitch(yaw, pitch)
    local cy, sy = math.cos(yaw * 0.5), math.sin(yaw * 0.5)
    local cp, sp = math.cos(pitch * 0.5), math.sin(pitch * 0.5)
    return { x = cy * sp, y = sy * cp, z = -sy * sp, w = cy * cp }
end

function M.qnormalize(q)
    local len = math.sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w)
    if len < 1e-9 then return { x = 0, y = 0, z = 0, w = 1 } end
    return { x = q.x / len, y = q.y / len, z = q.z / len, w = q.w / len }
end

function M.normalize(v)
    local len = math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
    if len < 1e-9 then return { x = 0, y = 0, z = 0 } end
    return { x = v.x / len, y = v.y / len, z = v.z / len }
end

function M.register_enemy(h, hp)
    M.ENEMIES[h] = hp or M.CFG.enemy_health
end

-- 存活判定：表里有记录 + 句柄仍然有效（场景重载后旧句柄 get_name 返回空串）
function M.enemy_alive(h)
    return M.ENEMIES[h] ~= nil and engine.entity.get_name(h) ~= ""
end

-- 敌人受击，返回是否死亡
function M.damage_enemy(h, dmg)
    local hp = M.ENEMIES[h]
    if hp == nil then return false end
    hp = hp - dmg
    if hp <= 0 then
        M.ENEMIES[h] = nil
        return true
    end
    M.ENEMIES[h] = hp
    return false
end

return M
