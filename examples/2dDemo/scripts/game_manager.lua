-- GameManager：HUD、昼夜循环、重力、关卡流程（每关场景挂到 GameManager 实体）。
props = {}

local common = require("common")

local function set_label(name, text)
    local h = engine.entity.find(name)
    if h ~= 0 then
        engine.component.set(h, "Label", "text", text)
    end
end

local function apply_daynight(phase)
    local pal, pname = common.sample_daynight(phase)
    local amb = engine.entity.find("AmbientLight")
    if amb ~= 0 then
        engine.component.set(amb, "AmbientLight2D", "color", pal.ambient)
        engine.component.set(amb, "AmbientLight2D", "intensity", pal.ai)
    end
    local sky = engine.entity.find("Skybox")
    if sky ~= 0 then
        engine.component.set(sky, "Skybox2D", "color", pal.sky)
    end
    local sun = engine.entity.find("DirectionalLight")
    if sun ~= 0 then
        engine.component.set(sun, "Light2D", "color", pal.sun)
        engine.component.set(sun, "Light2D", "intensity", pal.si)
    end
    local star = engine.entity.find("Starfield")
    if star ~= 0 then
        engine.component.set(star, "ParallaxBackground", "tint", pal.tint)
    end
    return pname
end

function on_start()
    common.ensure_levels()
    engine.log.info("2dDemo: game_manager started (levels=" .. #common.levels .. ")")
    if engine.state.get("score") == nil then
        engine.state.set("score", 0)
        engine.state.set("coins", 0)
        engine.state.set("lives", 3)
        engine.state.set("gun_level", 1)
        engine.state.set("level_index", 0)
        engine.state.set("game_over", false)
        engine.state.set("victory", false)
        engine.state.set("level_complete", false)
    end
    -- 每关重力（来自 levels.json）
    engine.physics.set_gravity(0, common.current_gravity())
end

function on_update(dt)
    local info, idx = common.current_level()

    local score = engine.state.get("score") or 0
    local coins = engine.state.get("coins") or 0
    local lives = engine.state.get("lives") or 3
    local gun = engine.state.get("gun_level") or 1
    local over = engine.state.get("game_over") or false
    local victory = engine.state.get("victory") or false
    local complete = engine.state.get("level_complete") or false

    -- 昼夜循环（每关从 levels.json 的 start_time 相位开始）
    local elapsed = engine.time.elapsed()
    local phase = ((info.start_time or 0) + elapsed / common.cycle_seconds) % 1
    local pname = apply_daynight(phase)

    set_label("ScoreLabel", "Score: " .. score)
    set_label("CoinsLabel", "Coins: " .. coins)
    set_label("LivesLabel", "Lives: " .. lives)
    set_label("GunLabel", gun >= 2 and "Gun: Lv.2 (三发)" or "Gun: Lv.1 (单发)")
    set_label("LevelLabel", string.format("%s  [%d/%d]", info.name or "Level",
                                          idx + 1, #common.levels))
    set_label("TimeLabel", string.format("Time: %s %3.0f%%", pname,
                                         math.floor(phase * 100)))

    -- 过关流程：goal.lua 置 level_complete；计时结束后进下一关/胜利
    if complete then
        local t = (engine.state.get("complete_timer") or 1.3) - dt
        if t <= 0 then
            engine.state.set("level_complete", false)
            if idx + 1 >= #common.levels then
                engine.state.set("victory", true)
                engine.log.info("2dDemo: all levels complete, victory!")
            else
                engine.state.set("level_index", idx + 1)
                engine.scene.load(common.levels[idx + 2].scene)
            end
        else
            engine.state.set("complete_timer", t)
            set_label("MessageLabel", "关卡完成! +100 分")
        end
        return
    end

    if victory then
        set_label("MessageLabel", string.format(
            "VICTORY! 全部通关! 总分 %d (R 重新开始)", score))
    elseif over then
        set_label("MessageLabel", string.format(
            "GAME OVER - 总分 %d (R 重新开始)", score))
    else
        set_label("MessageLabel", "")
    end

    -- R 重新开始
    if engine.input.key_down(82) then
        engine.state.set("score", 0)
        engine.state.set("coins", 0)
        engine.state.set("lives", 3)
        engine.state.set("gun_level", 1)
        engine.state.set("level_index", 0)
        engine.state.set("game_over", false)
        engine.state.set("victory", false)
        engine.state.set("level_complete", false)
        engine.scene.load(common.levels[1].scene)
    end
end
