props = { interval = 2.0 }

local elapsed = 0

function on_start()
    elapsed = 0
end

function on_update(dt)
    elapsed = elapsed + dt
    if elapsed >= props.interval then
        elapsed = 0
        gryce.log.info(string.format("timer tick at %.1fs", gryce.time.elapsed()))
    end
end
