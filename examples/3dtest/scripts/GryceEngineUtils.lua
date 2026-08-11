-- GryceEngineUtils (GryceSRT standard library).
-- New scripts start with: require("GryceEngineUtils")
-- This is the script-side equivalent of C++ "#include <core.h>".

local M = {}

-- --- vector helpers ---------------------------------------------------------
function M.vec3(x, y, z)
    return { x = x or 0, y = y or 0, z = z or 0 }
end

function M.copy_vec(t)
    return { x = t.x or 0, y = t.y or 0, z = t.z or 0 }
end

function M.distance(a, b)
    local dx = (a.x or 0) - (b.x or 0)
    local dy = (a.y or 0) - (b.y or 0)
    local dz = (a.z or 0) - (b.z or 0)
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

-- --- scalar helpers ---------------------------------------------------------
function M.clamp(v, lo, hi)
    return math.max(lo, math.min(hi, v))
end

function M.lerp(a, b, t)
    return a + (b - a) * t
end

function M.smoothstep(edge0, edge1, x)
    local t = M.clamp((x - edge0) / (edge1 - edge0), 0, 1)
    return t * t * (3 - 2 * t)
end

-- --- transform helpers ------------------------------------------------------
-- Rotate the entity around world/local Z by angle (radians).
function M.rotate_z(t, angle)
    local q = t.rot
    local half = angle / 2
    local s = math.sin(half)
    local c = math.cos(half)
    -- q * qz  (Z-axis quaternion {0,0,sin,cos})
    return {
        x = q.x * c + q.y * s,
        y = q.y * c - q.x * s,
        z = q.z * c + q.w * s,
        w = q.w * c - q.z * s
    }
end

-- --- timers -----------------------------------------------------------------
function M.countdown(timer, dt)
    return timer - dt
end

-- --- logging ----------------------------------------------------------------
function M.log(msg)    engine.log.info(msg) end
function M.warn(msg)   engine.log.warn(msg) end
function M.error(msg)  engine.log.error(msg) end

return M
