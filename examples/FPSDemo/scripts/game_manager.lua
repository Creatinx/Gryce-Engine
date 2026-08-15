-- game_manager.lua — FPS HUD、玩家生命、胜负判定、背景音乐。
-- 挂到 GameManager 实体。
props = {}

local common = require("common")

local function set_label(name, text)
    local h = engine.entity.find(name)
    if h ~= 0 then
        engine.component.set(h, "Label", "text", text)
    end
end

local function count_alive_enemies()
    local n = 0
    for h, _ in pairs(common.ENEMIES) do
        if common.enemy_alive(h) then n = n + 1 end
    end
    return n
end

-- 场景中实际存在的 Enemy 实体总数（防止"敌人脚本没注册就判胜利"）
local function count_enemy_entities()
    local n = 0
    local found = engine.entity.find_all("Enemy") or {}
    for _ in ipairs(found) do n = n + 1 end
    return n
end

function on_start()
    -- engine.state 跨场景重载/重进 Play 持久存在：必须无条件重置，
    -- 否则 R 重开会保留 lives=0（软锁）或残留旧 kills/health。
    engine.state.set("health", 100)
    engine.state.set("kills", 0)
    engine.state.set("lives", 5)
    engine.state.set("game_over", false)
    engine.state.set("victory", false)
    engine.state.set("jumping", false)
    engine.state.set("invuln", 0)
    -- 背景音乐：仅在游戏运行时（Play Mode / 独立 exe）播放，编辑器编辑态不播
    local bgm = engine.entity.find("BGM")
    if bgm ~= 0 then engine.audio.play_on(bgm) end
    engine.log.info("FPSDemo: game_manager started")
end

function on_update(dt)
    local health = engine.state.get("health") or 100
    local kills = engine.state.get("kills") or 0
    local lives = engine.state.get("lives") or 5
    local over = engine.state.get("game_over") or false
    local victory = engine.state.get("victory") or false

    -- 生命耗尽判定
    if health <= 0 and not over and not victory then
        if lives > 0 then
            engine.state.set("lives", lives - 1)
            engine.state.set("health", 100)
            local sp = engine.entity.find("SpawnPoint")
            local player = engine.entity.find("Player")
            if sp ~= 0 and player ~= 0 then
                local st = engine.entity.get_transform(sp)
                if st then
                    engine.component.set(player, "RigidBody", "velocity", { x = 0, y = 0, z = 0 })
                    engine.entity.set_transform(player, st.pos, nil, nil)
                end
            end
        else
            engine.state.set("game_over", true)
        end
    end

    -- 清剿胜利：要求场景里确实有 Enemy 实体，且全部被消灭。
    -- 只数 ENEMIES 会在敌人脚本未注册时（加载失败）误判秒胜。
    if not over and not victory and count_enemy_entities() > 0
        and count_alive_enemies() == 0 then
        engine.state.set("victory", true)
    end

    -- HUD
    set_label("HealthLabel", string.format("HP: %d", health))
    set_label("KillsLabel", string.format("击杀: %d", kills))
    set_label("LivesLabel", string.format("生命: %d", lives))
    set_label("EnemiesLabel", string.format("敌人: %d", count_alive_enemies()))

    if over then
        set_label("MessageLabel", "游戏结束！ 按 R 重新开始")
    elseif victory then
        set_label("MessageLabel", "通关！ 你消灭了所有敌人  按 R 重新开始")
    else
        set_label("MessageLabel", "用 WASD 移动，鼠标瞄准，左键射击")
    end

    -- 按 R (82) 重新开始
    if (over or victory) and engine.input.key_down(82) then
        engine.scene.load(engine.scene.current())
    end
end
