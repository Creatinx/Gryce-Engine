props = { speed = 1.0, label = "hello" }

function on_start()
    gryce.log.info("rotate.lua: on_start (GryceSRT)")
end

function on_update(dt)
    local h = gryce.self()
    if h == 0 then return end
    local t = gryce.entity.get_transform(h)
    if t then
        t.rot.z = t.rot.z + dt * props.speed
        gryce.entity.set_transform(h, t.pos, t.rot, t.scale)
    end
end
