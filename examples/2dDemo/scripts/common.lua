-- 2dDemo 公共库：关卡配置缓存、数学/碰撞小工具。
-- 通过 require("common") 使用（脚本共享 package.loaded，只加载一次）。

local M = {}

function M.lerp(a, b, t)
    return a + (b - a) * t
end

function M.lerp_color(c1, c2, t)
    return {
        r = M.lerp(c1.r, c2.r, t),
        g = M.lerp(c1.g, c2.g, t),
        b = M.lerp(c1.b, c2.b, t),
        a = M.lerp(c1.a, c2.a, t),
    }
end

-- AABB 重叠（a/b 为 engine.entity.aabb 返回的 {x,y,w,h}，中心+尺寸）
function M.aabb_overlap(a, b)
    if not a or not b then return false end
    return a.x - a.w / 2 < b.x + b.w / 2 and
           a.x + a.w / 2 > b.x - b.w / 2 and
           a.y - a.h / 2 < b.y + b.h / 2 and
           a.y + a.h / 2 > b.y - b.h / 2
end

-- 关卡配置（levels.json），首次访问时读取
M.levels = nil
M.cycle_seconds = 120

function M.ensure_levels()
    if M.levels then return end
    local j = engine.json.read("res:/levels.json")
    if j and j.levels and #j.levels > 0 then
        M.levels = j.levels
        M.cycle_seconds = j.cycle_seconds or 120
    else
        M.levels = {
            { scene = "res:/scenes/main.gesc", name = "Fallback", start_time = 0, gravity = 1150 },
        }
    end
end

function M.current_level()
    M.ensure_levels()
    local idx = engine.state.get("level_index") or 0
    return M.levels[idx + 1] or M.levels[1], idx
end

function M.current_gravity()
    local info = M.current_level()
    return info.gravity or 1150
end

function M.current_enemy_speed()
    local info = M.current_level()
    return info.enemy_speed or 90
end

-- 关卡世界边界（取自 Level Tilemap，缓存并按场景失效）
local _bounds_scene = nil
local _bounds_w, _bounds_h = 140 * 32, 24 * 32

function M.world_bounds()
    local scn = engine.scene.current()
    if scn ~= _bounds_scene then
        _bounds_scene = scn
        _bounds_w, _bounds_h = 140 * 32, 24 * 32
        local level = engine.entity.find("Level")
        if level ~= 0 then
            local mw = engine.component.get(level, "Tilemap", "map_width")
            local mh = engine.component.get(level, "Tilemap", "map_height")
            local cw = engine.component.get(level, "Tilemap", "cell_width") or 32
            local ch = engine.component.get(level, "Tilemap", "cell_height") or 32
            if mw and mh and mw > 0 and mh > 0 then
                _bounds_w = mw * cw
                _bounds_h = mh * ch
            end
        end
    end
    return _bounds_w, _bounds_h
end

-- 昼夜调色板（相位 0=白天 0.25=黄昏 0.5=夜晚 0.75=黎明）
local DAY = {
    ambient = { r = 0.85, g = 0.85, b = 0.90, a = 1 }, ai = 0.85,
    sky = { r = 1.0, g = 1.0, b = 1.0, a = 1 },
    sun = { r = 0.78, g = 0.82, b = 0.95, a = 1 }, si = 0.55,
    tint = { r = 1.0, g = 1.0, b = 1.0, a = 1 },
}
local DUSK = {
    ambient = { r = 0.52, g = 0.40, b = 0.46, a = 1 }, ai = 0.62,
    sky = { r = 1.0, g = 0.72, b = 0.52, a = 1 },
    sun = { r = 1.0, g = 0.68, b = 0.45, a = 1 }, si = 0.45,
    tint = { r = 0.95, g = 0.82, b = 0.72, a = 1 },
}
local NIGHT = {
    ambient = { r = 0.12, g = 0.14, b = 0.26, a = 1 }, ai = 0.38,
    sky = { r = 0.05, g = 0.07, b = 0.14, a = 1 },
    sun = { r = 0.42, g = 0.50, b = 0.92, a = 1 }, si = 0.38,
    tint = { r = 0.55, g = 0.58, b = 0.85, a = 1 },
}
local DAWN = {
    ambient = { r = 0.70, g = 0.66, b = 0.72, a = 1 }, ai = 0.72,
    sky = { r = 0.95, g = 0.82, b = 0.75, a = 1 },
    sun = { r = 0.90, g = 0.76, b = 0.62, a = 1 }, si = 0.50,
    tint = { r = 0.96, g = 0.90, b = 0.86, a = 1 },
}

local PHASE_NAMES = { "白天", "黄昏", "夜晚", "黎明" }

function M.sample_daynight(phase)
    local keys = { DAY, DUSK, NIGHT, DAWN, DAY }
    local points = { 0, 0.25, 0.5, 0.75, 1 }
    local t = phase - math.floor(phase)
    local seg = 1
    for i = 1, 4 do
        if t >= points[i] and t < points[i + 1] then
            seg = i
            break
        end
    end
    local lt = (t - points[seg]) / (points[seg + 1] - points[seg])
    local a, b = keys[seg], keys[seg + 1]
    return {
        ambient = M.lerp_color(a.ambient, b.ambient, lt),
        ai = M.lerp(a.ai, b.ai, lt),
        sky = M.lerp_color(a.sky, b.sky, lt),
        sun = M.lerp_color(a.sun, b.sun, lt),
        si = M.lerp(a.si, b.si, lt),
        tint = M.lerp_color(a.tint, b.tint, lt),
    }, PHASE_NAMES[seg]
end

return M
