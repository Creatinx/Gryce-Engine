props = { speed = 1.0, label = "hello" }

function on_start()
    engine.log.info("rotate.lua: on_start (GryceSRT)")
end

function on_update(dt)
    local h = engine.self()
    if h == 0 then return end
    local t = engine.entity.get_transform(h)
    if t then
        t.rot.z = t.rot.z + dt * props.speed
        engine.entity.set_transform(h, t.pos, t.rot, t.scale)
    end
end
