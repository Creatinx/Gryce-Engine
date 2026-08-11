props = { speed = 3.0 }

function on_update(dt)
    local h = engine.self()
    if h == 0 then return end
    local t = engine.entity.get_transform(h)
    if not t then return end

    local dx, dy, dz = 0, 0, 0
    if engine.input.key_down(87)  then dz = dz - 1 end   -- W
    if engine.input.key_down(83)  then dz = dz + 1 end   -- S
    if engine.input.key_down(65)  then dx = dx - 1 end   -- A
    if engine.input.key_down(68)  then dx = dx + 1 end   -- D
    if engine.input.key_down(32)  then dy = dy + 1 end   -- Space
    if engine.input.key_down(17)  then dy = dy - 1 end   -- Ctrl

    t.pos.x = t.pos.x + dx * props.speed * dt
    t.pos.y = t.pos.y + dy * props.speed * dt
    t.pos.z = t.pos.z + dz * props.speed * dt
    engine.entity.set_transform(h, t.pos, t.rot, t.scale)
end
