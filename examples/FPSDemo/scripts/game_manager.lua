-- game_manager.lua — FPS HUD、玩家生命、胜负判定、背景音乐、重开。

props = {}

local common = require("common")

-- 本局敌人总数：on_start 时捕获（场景实体已就位）。
-- 不能用"每帧实时数实体"做胜利条件——最后一个敌人被销毁后实体数变 0。
local enemy_total = 0

local function set_label(name, text)
    local h = engine.entity.find(name)
    if h ~= 0 then
        engine.component.set(h, "Label", "text", text)
    end
end

local function count_alive_enemies()
    local n = 0
    for h, _ in pairs(common.ENEMIES) do
        if common.enemy_alive(h) then
            n = n + 1
        else
            -- 清理陈旧句柄（场景重载后旧 handle 失效），避免胜利永不触发
            common.ENEMIES[h] = nil
        end
    end
    return n
end

local function count_enemy_entities()
    local n = 0
    local found = engine.entity.find_all("Enemy") or {}
    for _ in ipairs(found) do n = n + 1 end
    return n
end

function on_start()
    -- engine.state 跨场景重载/重进 Play 持久存在：必须无条件重置
    engine.state.set("health", 100)
    engine.state.set("kills", 0)
    engine.state.set("lives", 5)
    engine.state.set("game_over", false)
    engine.state.set("victory", false)
    engine.state.set("jumping", false)
    engine.state.set("invuln", 0)
    enemy_total = count_enemy_entities()

    local bgm = engine.entity.find("BGM")
    if bgm ~= 0 then engine.audio.play_on(bgm) end
end

function on_update(dt)
    local health = engine.state.get("health") or 100
    local kills = engine.state.get("kills") or 0
    local lives = engine.state.get("lives") or 5
    local over = engine.state.get("game_over") or false
    local victory = engine.state.get("victory") or false

    -- 生命耗尽
    if health <= 0 and not over and not victory then
        if lives > 0 then
            engine.state.set("lives", lives - 1)
            engine.state.set("health", 100)
            engine.state.set("invuln", 1.5)
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

    -- 清剿胜利
    if not over and not victory and enemy_total > 0 and count_alive_enemies() == 0 then
        engine.state.set("victory", true)
    end

    -- HUD（ASCII：Roboto 无 CJK 字形）
    set_label("HealthLabel", string.format("HP: %d", health))
    set_label("KillsLabel", string.format("Kills: %d", kills))
    set_label("LivesLabel", string.format("Lives: %d", lives))
    set_label("EnemiesLabel", string.format("Enemies: %d", count_alive_enemies()))
    if over then
        set_label("MessageLabel", "GAME OVER - press R to restart")
    elseif victory then
        set_label("MessageLabel", "VICTORY! Press R to restart")
    else
        set_label("MessageLabel", "WASD move, mouse aim, LMB shoot")
    end

    -- R (82) 重开
    if (over or victory) and engine.input.key_down(82) then
        engine.scene.load(engine.scene.current())
    end
end
