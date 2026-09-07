// ============================================================================
// game.js —— 3D 跑酷（PARKOUR RUN）玩法脚本（QuickJS，GryceGC-A 标准）
// ----------------------------------------------------------------------------
// 本模块由 GryceGame 固定运行时经 Scene -> ScriptComponent -> ScriptSystem
// 驱动：场景根实体 GameDriver 挂着 ScriptComponent（script_path=res:/game.js），
// 引擎在每帧 on_update 前调用本模块导出的 on_start/on_update。
//
// 仅使用标准脚本桥 engine.*：
//   engine.entity.create/set_mesh/destroy/get_name
//   engine.entity.set_transform / get_transform
//   engine.input.key_down(code)    GLFW 键码（left=263 right=262 A=65 D=68
//                                  space=32 R=82 enter=257）
//   engine.log.info/warn/error     输出到引擎日志
//   engine.time.delta / engine.self()
// ============================================================================

"use strict";

// ---- GLFW 键码 ----
const KEY_LEFT = 263;
const KEY_RIGHT = 262;
const KEY_A = 65;
const KEY_D = 68;
const KEY_R = 82;
const KEY_SPACE = 32;
const KEY_ENTER = 257;

// ---- 全局常量 ----
const LANES = [-1.2, 0.0, 1.2];          // 三条跑道 x 坐标
const SPAWN_Z = 42.0;                    // 障碍物出生深度
const KILL_Z = -5.0;                     // 越过玩家后回收的深度
const GRAVITY = 22.0;                    // 重力加速度
const JUMP_SPEED = 8.6;                  // 起跳初速度
const PLAYER_GROUND_Y = 0.6;             // 玩家箱体底面高度（地面 .gesc 顶面≈0.15 → 盒心 0.45 偏移）
const COIN_Y = 1.15;                     // 能量块高度
const BASE_SPEED = 10.0;                 // 基础移动速度
const SPEED_GROW = 0.35;                 // 速度随时间增长
const MAX_SPEED = 34.0;
const MESH_PATH = "res:/models/cube_pbr.obj";

// ---- 游戏状态 ----
var state = "menu";                      // menu | playing | gameover
var player = 0;                          // 玩家实体句柄
var playerLane = 1;
var playerY = PLAYER_GROUND_Y;
var playerVy = 0.0;
var speed = BASE_SPEED;
var score = 0.0;
var elapsed = 0.0;
var spawnTimer = 0.0;
var obstacles = [];                      // {id, lane, z, sy, sx, sz, y0, type}
var lastSpawnLane = -1;
var started = false;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
function abs(x) { return x < 0 ? -x : x; }

// 移动实体：用标准 set_transform 写 position（保持 scale/rotation 不变）
function move(handle, x, y, z) {
    var t = engine.entity.get_transform(handle);
    if (!t) return;
    engine.entity.set_transform(handle, { position: { x: x, y: y, z: z } });
}

// 生成一个带色的盒体实体（返回句柄；失败返回 0）
function makeBox(name, x, y, z, sx, sy, sz, r, g, b) {
    var h = engine.entity.create(name);
    if (h === 0) return 0;
    engine.entity.set_mesh(h, MESH_PATH, r, g, b, 0.6, 0.0);
    engine.entity.set_transform(h, {
        position: { x: x, y: y, z: z },
        scale: { x: sx, y: sy, z: sz }
    });
    return h;
}

// 随机生成一个障碍/能量块
function spawnItem() {
    var lanes = [0, 1, 2];
    var roll = Math.random();
    var type = roll < 0.42 ? "barrier" : (roll < 0.7 ? "low" : "coin");

    var lane = 0;
    var minBlocked = 99;
    if (type === "barrier") {
        // 选出"该深度 barrier 最少的跑道"，保证至少留一条活路
        for (var i = 0; i < 3; i++) {
            var at = 0;
            for (var j = 0; j < obstacles.length; j++) {
                var o = obstacles[j];
                if (o.lane === i && abs(o.z - SPAWN_Z) < 6.0 && o.type === "barrier") at++;
            }
            if (at < minBlocked) { minBlocked = at; lane = i; }
        }
    } else {
        lane = Math.floor(Math.random() * 3);
    }

    var sx, sy, sz, r, g, b;
    if (type === "coin") {
        sx = 0.3; sy = 0.3; sz = 0.3; r = 1.0; g = 0.85; b = 0.1;
    } else if (type === "low") {
        sx = 0.7; sy = 0.5; sz = 0.4; r = 1.0; g = 0.45; b = 0.2;
    } else { // barrier
        sx = 1.05; sy = 1.9; sz = 0.5; r = 0.9; g = 0.15; b = 0.15;
    }

    var y = type === "coin" ? COIN_Y : (sy * 0.5);
    var id = makeBox("Obs", LANES[lane], y, SPAWN_Z, sx, sy, sz, r, g, b);
    if (id === 0) return;
    obstacles.push({ id: id, lane: lane, z: SPAWN_Z, sy: sy, sx: sx, sz: sz, y0: y, type: type });
    lastSpawnLane = lane;
}

// 清空场上全部玩法实体（保留 .gesc 静态基架）
function clearItems() {
    for (var i = 0; i < obstacles.length; i++) {
        try { engine.entity.destroy(obstacles[i].id); } catch (e) {}
    }
    obstacles = [];
}

// 开始 / 重开一局
function startGame() {
    state = "playing";
    score = 0.0;
    elapsed = 0.0;
    speed = BASE_SPEED;
    spawnTimer = 1.0;
    playerLane = 1;
    playerY = PLAYER_GROUND_Y;
    playerVy = 0.0;

    clearItems();

    if (player !== 0) { try { engine.entity.destroy(player); } catch (e) {} }
    player = makeBox("Player", LANES[playerLane], playerY, 0.0, 0.5, 0.9, 0.5, 0.2, 0.85, 1.0);
    engine.state.set("gryce_state", HUD_PLAYING);
    engine.state.set("gryce_score", "0");
    engine.log.info("[parkour] 开始 (分数/时长将随游戏输出)");
}

// HUD 状态推送（menu | playing | gameover）
const HUD_MENU = "menu";
const HUD_PLAYING = "playing";
const HUD_OVER = "gameover";

// ---------------------------------------------------------------------------
// 脚本生命周期契约（ScriptSystem 驱动）
// ---------------------------------------------------------------------------

export function on_start() {
    engine.log.info("[parkour] Parkour Run script loaded.");
    engine.state.set("gryce_state", HUD_MENU);
    engine.state.set("gryce_score", "0");
    // 初始停在菜单态：等待空格 / 回车 / R 开局
}

export function on_update(dt) {
    // 菜单/结束态由按键开局
    if (state === "menu" || state === "gameover") {
        if (engine.input.key_down(KEY_SPACE) || engine.input.key_down(KEY_ENTER) ||
            engine.input.key_down(KEY_R)) {
            startGame();
        }
        return;
    }

    // ---- playing：难度与计时 ----
    if (dt > 0.05) dt = 0.05;
    elapsed += dt;
    speed = Math.min(BASE_SPEED + SPEED_GROW * elapsed, MAX_SPEED);

    // 玩家横向换道
    if (engine.input.key_down(KEY_LEFT) || engine.input.key_down(KEY_A)) {
        if (playerLane > 0) playerLane--;
    }
    if (engine.input.key_down(KEY_RIGHT) || engine.input.key_down(KEY_D)) {
        if (playerLane < 2) playerLane++;
    }
    // 跳跃
    if (engine.input.key_down(KEY_SPACE)) {
        if (playerY <= PLAYER_GROUND_Y + 0.01) playerVy = JUMP_SPEED;
    }
    playerVy -= GRAVITY * dt;
    playerY += playerVy * dt;
    if (playerY < PLAYER_GROUND_Y) { playerY = PLAYER_GROUND_Y; playerVy = 0.0; }
    move(player, LANES[playerLane], playerY, 0.0);

    // 累计分数（随距离增长）
    score += speed * dt * 4.0;

    // HUD：实时分数
    engine.state.set("gryce_state", HUD_PLAYING);
    engine.state.set("gryce_score", String(Math.floor(score)));

    // 生成
    spawnTimer -= dt;
    if (spawnTimer <= 0.0) {
        spawnItem();
        spawnTimer = 1.25 * (MAX_SPEED / speed);
    }

    // 移动 + 碰撞
    var px = LANES[playerLane];
    var dead = false;
    var collected = [];
    for (var i = 0; i < obstacles.length; i++) {
        var o = obstacles[i];
        o.z -= speed * dt;
        move(o.id, LANES[o.lane], o.y0, o.z);

        // 越过玩家 → 回收
        if (o.z < KILL_Z) { engine.entity.destroy(o.id); collected.push(i); continue; }

        // 同跑道判定
        var dx = abs(LANES[o.lane] - px);
        var dz = abs(o.z);
        if (o.type === "coin") {
            if (dx < 0.55 && dz < 0.7) {
                score += 50; engine.entity.destroy(o.id); collected.push(i);
            }
        } else {
            var hitX = dx < (o.sx / 2 + 0.3);
            var hitZ = dz < (o.sz / 2 + 0.45);
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
    for (var k = collected.length - 1; k >= 0; k--) obstacles.splice(collected[k], 1);

    if (dead) {
        state = "gameover";
        engine.state.set("gryce_state", HUD_OVER);
        engine.state.set("gryce_score", String(Math.floor(score)));
        engine.log.info("[parkour] 结束: 得分 " + Math.floor(score) +
                        " · 坚持 " + Math.floor(elapsed) + " 秒 · 按空格/R/回车重开");
    }
}

export function on_destroy() {
    if (player !== 0) { try { engine.entity.destroy(player); } catch (e) {} }
    clearItems();
}