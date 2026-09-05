// ============================================================================
// game.js —— 3D 跑酷（PARKOUR RUN）玩法脚本（QuickJS）
// ----------------------------------------------------------------------------
// 本文件承载全部玩法逻辑，不自持 C++：通过 engine.game.* 桥接原语，
// 在 ECS World 中生成/移动/销毁 3D 盒体实体，读取键盘输入，并更新 .uif 中
// 声明的 UI 控件（标签文本 / 面板可见性）。
//
// 引擎桥（由 C++ 宿主注册的 engine.game 对象）：
//   create(name,sx,sy,sz,r,g,b) -> id         生成一个带颜色的盒子实体
//   move(id,x,y,z) / scale(id,x,y,z) / pos(id)
//   destroy(id) / clear()                      销毁单实体 / 清空全部玩法实体
//   held(name) / pressed(name) -> bool         按键（left/right/space/a/d/r...）
//   setLabel(id,text)                          更新 Label 控件文本
//   setLabelVisible(id,bool)                   显示/隐藏控件
// ============================================================================

"use strict";

// ---- 全局常量 ----
const LANES = [-1.2, 0.0, 1.2];          // 三条跑道 x 坐标
const SPAWN_Z = 42.0;                    // 障碍物出生深度
const KILL_Z = -5.0;                     // 越过玩家后回收的深度
const GRAVITY = 22.0;                    // 重力加速度
const JUMP_SPEED = 8.6;                  // 起跳初速度
const PLAYER_GROUND_Y = 0.55;            // 玩家箱体的底面高度（盒心 y）
const COIN_Y = 1.15;                     // 能量块高度
const BASE_SPEED = 10.0;                 // 基础移动速度
const SPEED_GROW = 0.35;                 // 速度随时间增长
const MAX_SPEED = 34.0;

// ---- 游戏状态 ----
var state = "menu";                      // menu | playing | gameover
var player = 0;                          // 玩家实体 id
var playerLane = 1;
var playerY = PLAYER_GROUND_Y;
var playerVy = 0.0;
var speed = BASE_SPEED;
var score = 0.0;
var elapsed = 0.0;
var spawnTimer = 0.0;
var obstacles = [];                      // {id, lane, z, sy, type}
var lastSpawnLane = -1;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
function setText(id, text) {
    try { engine.game.setLabel(id, text); } catch (e) { /* 忽略 UI 未就绪 */ }
}
function setVisible(id, v) {
    try { engine.game.setLabelVisible(id, !!v); } catch (e) {}
}
function abs(x) { return x < 0 ? -x : x; }
function lerpY() { return 0.0; }

// 随机生成一个障碍/能量块
function spawnItem() {
    // 尽量不把三条跑道同时堵死：barrier 时优先选一个"可通行"的跑道
    let lanes = [0, 1, 2];
    let roll = Math.random();
    let type = roll < 0.42 ? "barrier" : (roll < 0.70 ? "low" : "coin");

    let lane = 0, minBlocked = 99;
    if (type === "barrier") {
        // 选出"该深度障碍最少的跑道"，保证至少留一条活路
        for (let i = 0; i < 3; i++) {
            let at = 0;
            for (let j = 0; j < obstacles.length; j++) {
                let o = obstacles[j];
                if (o.lane === i && abs(o.z - SPAWN_Z) < 6.0 && o.type === "barrier") at++;
            }
            if (at < minBlocked) { minBlocked = at; lane = i; }
        }
    } else {
        lane = Math.floor(Math.random() * 3);
    }

    let sx, sy, sz, r, g, b;
    if (type === "coin") {
        sx = 0.3; sy = 0.3; sz = 0.3; r = 1.0; g = 0.85; b = 0.1;
    } else if (type === "low") {
        sx = 0.7; sy = 0.5; sz = 0.4; r = 1.0; g = 0.45; b = 0.2;
    } else { // barrier
        sx = 1.05; sy = 1.9; sz = 0.5; r = 0.9; g = 0.15; b = 0.15;
    }

    let id;
    try { id = engine.game.create("Obs", sx, sy, sz, r, g, b); } catch (e) { return; }
    let y = type === "coin" ? COIN_Y : (sy * 0.5);
    engine.game.move(id, LANES[lane], y, SPAWN_Z);
    obstacles.push({ id: id, lane: lane, z: SPAWN_Z, sy: sy, sx: sx, sz: sz, y0: y, type: type });
    lastSpawnLane = lane;
}

// 开始一局 / 重开一局
function startGame() {
    state = "playing";
    score = 0.0;
    elapsed = 0.0;
    speed = BASE_SPEED;
    spawnTimer = 1.0;
    playerLane = 1;
    playerY = PLAYER_GROUND_Y;
    playerVy = 0.0;
    obstacles = [];

    engine.game.clear();
    player = engine.game.create("Player", 0.5, 0.9, 0.5, 0.2, 0.85, 1.0);
    engine.game.move(player, LANES[playerLane], playerY, 0.0);

    setVisible("MenuOverlay", false);
    setVisible("GameOverOverlay", false);
    setText("ScoreLabel", "分数: 0");
    setText("SpeedLabel", "速度 1.0x");
    setText("TipsLabel", "← → / A D 切换跑道 · 空格 跳跃");
}

// ---------------------------------------------------------------------------
// .uif 事件回调（按钮 onClick 引用）
// ---------------------------------------------------------------------------
function onStart(widgetId) {
    if (state === "menu" || state === "gameover") startGame();
}

function onRestart(widgetId) {
    if (state === "gameover") startGame();
}

// ---------------------------------------------------------------------------
// 逐帧驱动（C++ 宿主每帧调用）
// ---------------------------------------------------------------------------
function onGameUpdate(dt) {
    if (state === "menu") {
        // 菜单态：仅给个友好的动态提示
        setText("TipsLabel", "点击「开始游戏」进入跑酷 · ← → 切换跑道 · 空格 跳跃");
        return;
    }

    if (state === "gameover") {
        if (engine.game.pressed("r") || engine.game.pressed("enter")) onRestart();
        return;
    }

    // ---- playing：难度与计时 ----
    dt = Math.min(dt, 0.05);
    elapsed += dt;
    speed = Math.min(BASE_SPEED + SPEED_GROW * elapsed, MAX_SPEED);

    // 玩家横向换道
    if (engine.game.pressed("left") || engine.game.pressed("a")) {
        if (playerLane > 0) playerLane--;
    }
    if (engine.game.pressed("right") || engine.game.pressed("d")) {
        if (playerLane < 2) playerLane++;
    }
    // 跳跃
    if (engine.game.pressed("space")) {
        if (playerY <= PLAYER_GROUND_Y + 0.01) playerVy = JUMP_SPEED;
    }
    playerVy -= GRAVITY * dt;
    playerY += playerVy * dt;
    if (playerY < PLAYER_GROUND_Y) { playerY = PLAYER_GROUND_Y; playerVy = 0.0; }
    engine.game.move(player, LANES[playerLane], playerY, 0.0);

    // 累计分数（随距离增长）
    score += speed * dt * 4.0;

    // 生成
    spawnTimer -= dt;
    if (spawnTimer <= 0.0) {
        spawnItem();
        spawnTimer = 1.25 * (MAX_SPEED / speed);
    }

    // 移动 + 碰撞
    let px = LANES[playerLane];
    let dead = false;
    let collected = [];
    for (let i = 0; i < obstacles.length; i++) {
        let o = obstacles[i];
        o.z -= speed * dt;
        engine.game.move(o.id, LANES[o.lane], o.y0, o.z);

        // 越过玩家 → 回收
        if (o.z < KILL_Z) { engine.game.destroy(o.id); collected.push(i); continue; }

        // 同跑道判定
        let dx = abs(LANES[o.lane] - px);
        let dz = abs(o.z);
        if (o.type === "coin") {
            if (dx < 0.55 && dz < 0.7) {
                score += 50; engine.game.destroy(o.id); collected.push(i);
            }
        } else {
            let hitX = dx < (o.sx / 2 + 0.3);
            let hitZ = dz < (o.sz / 2 + 0.45);
            if (hitX && hitZ) {
                if (o.type === "barrier") {
                    dead = true;
                } else { // low：跳得够高则越过
                    if (playerY < o.y0 + o.sy - 0.05) dead = true;
                }
            }
        }
    }
    // 清理已回收/已收集
    for (let k = collected.length - 1; k >= 0; k--) obstacles.splice(collected[k], 1);

    // ---- 更新 HUD ----
    setText("ScoreLabel", "分数: " + Math.floor(score));
    setText("SpeedLabel", "速度 " + speed.toFixed(1) + "x");

    if (dead) {
        state = "gameover";
        setText("FinalScoreLabel", "得分: " + Math.floor(score) + " · 坚持 " + Math.floor(elapsed) + " 秒");
        setText("TipsLabel", "按 R 或 Enter 重新开始");
        setVisible("GameOverOverlay", true);
        setVisible("MenuOverlay", false);
    }
}

// 预留：让脚本能感知脚本自身已就绪
console.log("game.js loaded (Parkour Run).");