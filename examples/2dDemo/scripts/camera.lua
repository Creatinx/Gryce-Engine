-- Camera：平滑跟随玩家，并把镜头钳制在关卡边界内。
props = {}

local self_h = 0

function on_start()
    self_h = engine.self()
end

function on_update(dt)
    if self_h == 0 then self_h = engine.self() end
    if self_h == 0 then return end

    local player = engine.entity.find("Player")
    if player == 0 then return end
    local pt = engine.entity.get_transform(player)
    local ct = engine.entity.get_transform(self_h)
    if not pt or not ct then return end

    local common = require("common")
    local world_w, world_h = common.world_bounds()
    local tx, ty = pt.pos.x, pt.pos.y - 60
    local nx = ct.pos.x + (tx - ct.pos.x) * 0.12
    local ny = ct.pos.y + (ty - ct.pos.y) * 0.08
    if world_w > 1280 then
        nx = math.max(640, math.min(nx, world_w - 640))
    end
    if world_h > 720 then
        ny = math.max(360, math.min(ny, world_h - 360))
    end
    engine.entity.set_transform(self_h, { x = nx, y = ny, z = 0 }, nil, nil)
end
