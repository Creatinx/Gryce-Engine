#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""2dDemo 关卡生成器（Gryce Engine）。

生成 examples/2dDemo/scenes/level_*.gesc（编辑器可加载的关卡场景）与
examples/2dDemo/levels.json（关卡顺序/昼夜起始/重力/敌速）。
运行：python tools/gen_2dDemo_levels.py
"""

import json

K = 32  # 瓦片世界尺寸
SURFACE = 21  # 地面行 y


# ---------------- component builders ----------------
def c2d(t, render_order=0, **fields):
    c = {"canvas_layer": 0, "enabled": True, "render_order": render_order}
    c.update(fields)
    c["type"] = t
    return c


def node2d():
    return {"type": "Node2D", "enabled": True, "z_index": 0, "top_level": False}


def sprite(tex, w, h, lit=True, ro=10, color=(1, 1, 1, 1), shadow=False):
    return c2d("Sprite2D", ro,
               texture_path=tex, normal_map_path="", color=list(color),
               width=w, height=h, lit=lit, cast_shadow=shadow)


def rigidbody(mass=1.0, gravity=True, restitution=0.0, friction=0.2,
              damping=0.02, vel=(0, 0)):
    return {"type": "RigidBody2D", "enabled": True, "mass": mass,
            "use_gravity": gravity, "is_kinematic": False, "fixed_rotation": False,
            "velocity": list(vel), "acceleration": [0.0, 0.0],
            "restitution": restitution, "friction": friction, "linear_damping": damping}


def boxcollider(size=(28, 28), center=(0, 0), trigger=False):
    return {"type": "BoxCollider2D", "enabled": True, "size": list(size),
            "center": list(center), "is_trigger": trigger}


def circlecollider(radius=5.0, center=(0, 0), trigger=False):
    return {"type": "CircleCollider2D", "enabled": True, "radius": radius,
            "center": list(center), "is_trigger": trigger}


def light2d(lt="point", color=(1, 1, 1, 1), intensity=1.0, radius=200.0, range=1000.0,
            direction=(0, -1), spot_angle=45.0, spot_softness=0.2, ro=0):
    return c2d("Light2D", ro, light_type=lt, color=list(color), intensity=intensity,
               radius=radius, range=range, direction=list(direction),
               spot_angle=spot_angle, spot_softness=spot_softness)


def ambient(color=(0.85, 0.85, 0.9, 1), intensity=0.85, ro=-2000):
    return c2d("AmbientLight2D", ro, color=list(color), intensity=intensity)


def skybox(tex, color=(1, 1, 1, 1), scroll=0.02, tile=True, ro=-1500):
    return c2d("Skybox2D", ro, texture_path=tex, color=list(color),
               scroll_factor=scroll, tile=tile)


def parallax(layers, ro=-1000):
    return c2d("ParallaxBackground", ro, layers=layers)


def camera2d(ro=0, active=True, zoom=1.0, pos_off=(0, 0)):
    return c2d("Camera2D", ro, is_active=active, zoom=zoom, rotation=0.0,
               offset=list(pos_off), limit_enabled=False, limit_left=-1000000.0,
               limit_top=-1000000.0, limit_right=1000000.0, limit_bottom=1000000.0)


def label(text, font_size=18.0, color=(1, 1, 1, 1), ro=1100):
    return c2d("Label", ro, text=text, font_size=font_size, color=list(color))


def audio(clip, volume=0.5, pitch=1.0):
    return {"type": "AudioSource", "enabled": True, "clip_path": clip, "volume": volume,
            "pitch": pitch, "speed": 1.0, "loop": False, "play_on_awake": False,
            "is_3d": False, "min_distance": 1.0, "max_distance": 100.0}


def particle(ro, **kw):
    base = {"emission_rate": 0.0, "max_particles": 256, "burst_min": 8, "burst_max": 16,
            "lifetime_min": 0.4, "lifetime_max": 0.8, "velocity_min": 50.0, "velocity_max": 150.0,
            "direction_min": 0.0, "direction_max": 6.283185307179586,
            "acceleration": [0.0, 0.0], "start_color": [1, 1, 1, 1], "end_color": [1, 1, 1, 0],
            "start_size": 8.0, "end_size": 2.0, "rotation_min": 0.0, "rotation_max": 6.283185307179586,
            "angular_velocity_min": 0.0, "angular_velocity_max": 0.0, "texture_path": "",
            "additive": False, "emission_offset": [0.0, 0.0]}
    base.update(kw)
    return c2d("ParticleEmitter2D", ro, **base)


def script(path):
    return {"type": "Script", "enabled": True, "script_path": path}


def tilemap(tiles, w, h, ro=-100):
    return c2d("Tilemap", ro, tileset_path="res:/tilesets/default.json",
               map_width=w, map_height=h, cell_width=32.0, cell_height=32.0,
               tiles=tiles, generate_colliders=True, debug_draw_colliders=False,
               use_tileset_texture=True, lit=True, cast_shadow=True)


def polygon(points, color, ro=0):
    return c2d("Polygon", ro, points=[list(p) for p in points], color=list(color))


def colorrect(w, h, color, ro=0):
    return c2d("ColorRect", ro, width=w, height=h, color=list(color))


# ---------------- entity builders ----------------
_counter = [0]


def ent(name, components, pos=(0, 0, 0)):
    _counter[0] += 1
    uid = "%012d-0000-0000-0000-000000000000" % (_counter[0] * 7 + 3)
    return {"name": name, "uuid": uid, "parent": None, "enabled": True,
            "transform": {"position": list(pos), "rotation": [0.0, 0.0, 0.0, 1.0],
                          "scale": [1.0, 1.0, 1.0]}, "components": components}


def scene_common(map_w, map_h, tiles, player_spawn):
    es = []
    es.append(ent("Starfield", [parallax([
        {"texture_path": "res:/textures/parallax_stars.png", "scroll_factor": 0.05, "scale": 1.4,
         "tint": [1.0, 1.0, 1.0, 1.0]},
        {"texture_path": "res:/textures/parallax_stars.png", "scroll_factor": 0.15, "scale": 1.0,
         "tint": [1.0, 1.0, 1.0, 1.0]},
        {"texture_path": "res:/textures/parallax_stars.png", "scroll_factor": 0.35, "scale": 0.7,
         "tint": [1.0, 1.0, 1.0, 1.0]},
    ])]))
    es.append(ent("AmbientLight", [ambient()]))
    es.append(ent("Skybox", [skybox("res:/textures/sky.png")]))
    es.append(ent("Level", [tilemap(tiles, map_w, map_h)]))
    es.append(ent("DirectionalLight", [light2d(
        "directional", (0.78, 0.82, 0.95, 1), 0.55,
        direction=(0.2873478855663454, -0.9578262852211514), ro=-500)]))
    # 玩家：精灵 + 刚体 + 碰撞盒 + 手电筒聚光
    es.append(ent("Player", [
        sprite("res:/textures/player.png", 28, 28, ro=10),
        rigidbody(mass=1.0, restitution=0.0, friction=0.2, damping=0.02),
        boxcollider((28, 28)),
        light2d("spot", (1.0, 0.95, 0.7, 1), 1.7, 260.0, 420.0, (1.0, 0.0), 35.0, 0.25, ro=100),
        script("res:/scripts/player.lua"),
    ], pos=(player_spawn[0], player_spawn[1], 0)))
    es.append(ent("SpawnPoint", [node2d()], pos=(player_spawn[0], player_spawn[1], 0)))
    es.append(ent("MainCamera", [camera2d(),
                                 script("res:/scripts/camera.lua")], pos=(640.0, 360.0, 0.0)))
    es.append(ent("GameManager", [script("res:/scripts/game_manager.lua")]))
    # HUD
    es.append(ent("ScoreLabel", [label("Score: 0", 18, (1, 1, 1, 1))], pos=(12, 28, 0)))
    es.append(ent("CoinsLabel", [label("Coins: 0", 18, (1, 0.9, 0.2, 1))], pos=(12, 52, 0)))
    es.append(ent("LivesLabel", [label("Lives: 3", 18, (1, 1, 1, 1))], pos=(12, 76, 0)))
    es.append(ent("GunLabel", [label("Gun: Lv.1", 18, (0.5, 1, 0.7, 1))], pos=(12, 100, 0)))
    es.append(ent("LevelLabel", [label("Level", 18, (0.6, 0.8, 1, 1))], pos=(12, 124, 0)))
    es.append(ent("TimeLabel", [label("Time: Day", 18, (0.9, 0.85, 0.7, 1))], pos=(12, 148, 0)))
    es.append(ent("MessageLabel", [label("", 20, (1, 0.5, 0.3, 1))], pos=(12, 176, 0)))
    es.append(ent("FPSLabel", [label("Render FPS: --", 15, (0.4, 1, 0.5, 1))], pos=(12, 202, 0)))
    es.append(ent("HintLabel", [label(
        "A/D 移动 | Space/W 跳跃 | 鼠标/Z 射击 | R 重开", 15, (0.75, 0.75, 0.75, 1))],
        pos=(12, 226, 0)))
    # 音效
    es.append(ent("SFX_Jump", [audio("res:/audio/jump.wav", 0.45)]))
    es.append(ent("SFX_Coin", [audio("res:/audio/coin.wav", 0.40)]))
    es.append(ent("SFX_Stomp", [audio("res:/audio/stomp.wav", 0.50)]))
    es.append(ent("SFX_Hurt", [audio("res:/audio/hurt.wav", 0.55)]))
    es.append(ent("SFX_Shoot", [audio("res:/audio/shoot.wav", 0.35)]))
    es.append(ent("SFX_Powerup", [audio("res:/audio/coin.wav", 0.45, pitch=1.6)]))
    es.append(ent("SFX_Goal", [audio("res:/audio/explosion.wav", 0.50)]))
    # 粒子特效：跳跃尘土 / 命中爆炸 / 枪口火光
    es.append(ent("JumpDust", [particle(15,
        emission_offset=[0.0, 14.0], direction_min=1.2566370614359172,
        direction_max=1.8849555921538759, velocity_min=40.0, velocity_max=110.0,
        acceleration=[0.0, 200.0], lifetime_min=0.2, lifetime_max=0.45,
        start_color=[0.75, 0.55, 0.30, 1], end_color=[0.75, 0.55, 0.30, 0],
        start_size=5.0, end_size=1.0, angular_velocity_min=-120.0,
        angular_velocity_max=120.0)]))
    es.append(ent("HitFx", [particle(50,
        burst_min=14, burst_max=22, velocity_min=50.0, velocity_max=180.0,
        direction_min=0.0, direction_max=6.283185307179586, acceleration=[0.0, 120.0],
        lifetime_min=0.25, lifetime_max=0.55,
        start_color=[1, 0.35, 0.15, 1], end_color=[0.4, 0.05, 0.05, 0],
        start_size=6.0, end_size=1.0, rotation_min=0.0, rotation_max=3.141592653589793,
        angular_velocity_min=-270.0, angular_velocity_max=270.0)]))
    es.append(ent("MuzzleFlash", [particle(60,
        burst_min=6, burst_max=10, velocity_min=60.0, velocity_max=160.0,
        direction_min=-0.5, direction_max=0.5, acceleration=[0.0, 0.0],
        lifetime_min=0.08, lifetime_max=0.18,
        start_color=[1.0, 0.85, 0.3, 1], end_color=[1.0, 0.5, 0.1, 0],
        start_size=5.0, end_size=1.0, additive=True, emission_offset=[10.0, 0.0])]))
    return es


def coin(name, x, y, spin=False):
    comps = [sprite("res:/textures/coin.png", 20, 20, lit=True, ro=5),
             light2d("point", (1, 0.8, 0.1, 1), 2.0, 55.0, 1000.0, ro=100)]
    comps.append(script("res:/scripts/coin.lua"))
    return ent(name, comps, pos=(x, y, 0))


def enemy(name, x, y, patrol_left_x, patrol_right_x):
    es = [ent(name, [sprite("res:/textures/enemy.png", 28, 28, lit=True, ro=8),
                     rigidbody(mass=1.0, restitution=0.1, friction=0.3, damping=0.0,
                               vel=(90, 0)),
                     boxcollider((28, 28)),
                     script("res:/scripts/enemy.lua")], pos=(x, y, 0))]
    es.append(ent("PatrolLeft_" + name, [node2d()], pos=(patrol_left_x, y, 0)))
    es.append(ent("PatrolRight_" + name, [node2d()], pos=(patrol_right_x, y, 0)))
    return es


def spike(name, x, y):
    return ent(name, [polygon([(0, -14), (16, 10), (-16, 10)], (1, 0.25, 0.2, 1), ro=6),
                      boxcollider((26, 16), (0, 2), trigger=True)], pos=(x, y, 0))


def powerup(name, x, y):
    return ent(name, [sprite("res:/textures/bullet.png", 24, 24, lit=True, ro=12,
                             color=(0.3, 1, 0.8, 1)),
                      light2d("point", (0.3, 1, 0.8, 1), 2.4, 70.0, 1000.0, ro=100),
                      boxcollider((24, 24)),
                      script("res:/scripts/powerup.lua")], pos=(x, y, 0))


def goal(x, y):
    return ent("Goal", [colorrect(6, 46, (0.75, 0.75, 0.8, 1), ro=6),
                        polygon([(3, -42), (3, -14), (26, -28)], (1, 0.4, 0.3, 1), ro=7),
                        light2d("point", (1, 0.9, 0.5, 1), 2.2, 90.0, 1000.0, ro=100),
                        boxcollider((40, 48), (0, -10)),
                        script("res:/scripts/goal.lua")], pos=(x, y, 0))


# ---------------- tilemap builder ----------------
def build_tiles(map_w, map_h, pits, platforms):
    t = [[-1] * map_w for _ in range(map_h)]
    for x in range(map_w):
        t[SURFACE][x] = 0
        t[SURFACE + 1][x] = 1
        t[SURFACE + 2][x] = 1
    for (x1, x2) in pits:
        for x in range(x1, x2 + 1):
            for y in (SURFACE, SURFACE + 1, SURFACE + 2):
                t[y][x] = -1
    for (x1, x2, y) in platforms:
        for x in range(x1, x2 + 1):
            t[y][x] = 2
    return [t[y][x] for y in range(map_h) for x in range(map_w)]


# ---------------- levels ----------------
def make_level(file_name, scene_name, map_w, map_h, pits, platforms, player_spawn,
               coins, enemies, spikes, powerups, goal_x):
    tiles = build_tiles(map_w, map_h, pits, platforms)
    es = scene_common(map_w, map_h, tiles, player_spawn)
    for (name, x, y, spin) in coins:
        es.append(coin(name, x, y, spin))
    for (name, x, y, lx, rx) in enemies:
        es.extend(enemy(name, x, y, lx, rx))
    for (name, x, y) in spikes:
        es.append(spike(name, x, y))
    for (name, x, y) in powerups:
        es.append(powerup(name, x, y))
    es.append(goal(goal_x, SURFACE * K - 10))
    scene = {"version": 2, "name": scene_name, "entities": es}
    with open("examples/2dDemo/scenes/" + file_name, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("wrote", file_name, "entities:", len(es), "tiles:", len(tiles))


SY = SURFACE * K
GY = SY - 18  # 地面上的实体站立 y

# Level 1 - 晨曦草原（白天，教学关）
make_level("level_1.gesc", "Level1_DawnPlains", 110, 24,
           pits=[(40, 44)],
           platforms=[(15, 20, 17), (55, 62, 17), (80, 86, 15)],
           player_spawn=(3 * K, (24 - 6) * K),
           coins=[
               ("Coin1", 8 * K, (SURFACE - 1) * K, True),
               ("Coin2", 10 * K, (SURFACE - 1) * K, False),
               ("Coin3", 12 * K, (SURFACE - 1) * K, False),
               ("Coin4", 17 * K, 16 * K, True),
               ("Coin5", 18 * K, 16 * K, False),
               ("Coin6", 57 * K, 16 * K, False),
               ("Coin7", 60 * K, 16 * K, True),
               ("Coin8", 83 * K, 14 * K, False),
               ("Coin9", 85 * K, 14 * K, False),
               ("Coin10", 100 * K, (SURFACE - 1) * K, False),
           ],
           enemies=[
               ("Enemy1", 28 * K, GY, 24 * K, 34 * K),
               ("Enemy2", 58 * K, 16 * K, 55 * K, 62 * K),
               ("Enemy3", 72 * K, GY, 67 * K, 78 * K),
           ],
           spikes=[],
           powerups=[],
           goal_x=104 * K)

# Level 2 - 黄昏峡谷（黄昏，出现尖刺与枪械升级）
make_level("level_2.gesc", "Level2_DuskCanyon", 140, 24,
           pits=[(22, 26), (58, 63), (105, 110)],
           platforms=[(10, 16, 17), (34, 42, 18), (48, 54, 15),
                      (72, 80, 17), (86, 94, 14), (118, 126, 16)],
           player_spawn=(3 * K, (24 - 6) * K),
           coins=[
               ("Coin1", 7 * K, (SURFACE - 1) * K, True),
               ("Coin2", 13 * K, 16 * K, True),
               ("Coin3", 15 * K, 16 * K, False),
               ("Coin4", 36 * K, 17 * K, False),
               ("Coin5", 40 * K, 17 * K, True),
               ("Coin6", 50 * K, 14 * K, False),
               ("Coin7", 52 * K, 14 * K, False),
               ("Coin8", 74 * K, 16 * K, True),
               ("Coin9", 78 * K, 16 * K, False),
               ("Coin10", 88 * K, 13 * K, False),
               ("Coin11", 91 * K, 13 * K, True),
               ("Coin12", 120 * K, 15 * K, False),
               ("Coin13", 124 * K, 15 * K, False),
               ("Coin14", 130 * K, (SURFACE - 1) * K, False),
           ],
           enemies=[
               ("Enemy1", 19 * K, GY, 16 * K, 21 * K),
               ("Enemy2", 38 * K, 17 * K, 34 * K, 42 * K),
               ("Enemy3", 76 * K, 16 * K, 72 * K, 80 * K),
               ("Enemy4", 90 * K, 13 * K, 86 * K, 94 * K),
           ],
           spikes=[
               ("Spike1", 30 * K, SY - 10),
               ("Spike2", 31 * K, SY - 10),
               ("Spike3", 95 * K, SY - 10),
               ("Spike4", 96 * K, SY - 10),
           ],
           powerups=[("PowerUp1", 52 * K, 14 * K)],
           goal_x=134 * K)

# Level 3 - 午夜要塞（夜晚，最多敌人/尖刺/坑洞）
make_level("level_3.gesc", "Level3_MidnightFortress", 160, 24,
           pits=[(18, 22), (55, 60), (95, 100), (135, 140)],
           platforms=[(10, 16, 17), (30, 36, 18), (46, 52, 15), (66, 72, 17),
                      (82, 90, 14), (110, 116, 16), (126, 132, 18), (145, 150, 15)],
           player_spawn=(3 * K, (24 - 6) * K),
           coins=[
               ("Coin1", 7 * K, (SURFACE - 1) * K, True),
               ("Coin2", 13 * K, 16 * K, True),
               ("Coin3", 15 * K, 16 * K, False),
               ("Coin4", 33 * K, 17 * K, False),
               ("Coin5", 35 * K, 17 * K, True),
               ("Coin6", 48 * K, 14 * K, False),
               ("Coin7", 50 * K, 14 * K, True),
               ("Coin8", 68 * K, 16 * K, False),
               ("Coin9", 70 * K, 16 * K, False),
               ("Coin10", 84 * K, 13 * K, True),
               ("Coin11", 87 * K, 13 * K, False),
               ("Coin12", 112 * K, 15 * K, False),
               ("Coin13", 114 * K, 15 * K, True),
               ("Coin14", 128 * K, 17 * K, False),
               ("Coin15", 130 * K, 17 * K, False),
               ("Coin16", 147 * K, 14 * K, True),
               ("Coin17", 149 * K, 14 * K, False),
               ("Coin18", 152 * K, (SURFACE - 1) * K, False),
           ],
           enemies=[
               ("Enemy1", 12 * K, GY, 9 * K, 15 * K),
               ("Enemy2", 48 * K, 14 * K, 46 * K, 52 * K),
               ("Enemy3", 70 * K, 16 * K, 66 * K, 72 * K),
               ("Enemy4", 86 * K, 13 * K, 82 * K, 90 * K),
               ("Enemy5", 120 * K, GY, 116 * K, 124 * K),
               ("Enemy6", 147 * K, 14 * K, 145 * K, 150 * K),
           ],
           spikes=[
               ("Spike1", 26 * K, SY - 10),
               ("Spike2", 27 * K, SY - 10),
               ("Spike3", 75 * K, SY - 10),
               ("Spike4", 76 * K, SY - 10),
               ("Spike5", 108 * K, SY - 10),
               ("Spike6", 109 * K, SY - 10),
           ],
           powerups=[("PowerUp1", 88 * K, 13 * K), ("PowerUp2", 128 * K, 17 * K)],
           goal_x=154 * K)

# levels.json：关卡顺序 + 昼夜起始相位 + 重力 + 敌速
levels = {
    "cycle_seconds": 120,
    "levels": [
        {"scene": "res:/scenes/level_1.gesc", "name": "Level 1 - 晨曦草原",
         "start_time": 0.0, "gravity": 1150, "enemy_speed": 80},
        {"scene": "res:/scenes/level_2.gesc", "name": "Level 2 - 黄昏峡谷",
         "start_time": 0.28, "gravity": 1050, "enemy_speed": 100},
        {"scene": "res:/scenes/level_3.gesc", "name": "Level 3 - 午夜要塞",
         "start_time": 0.52, "gravity": 1150, "enemy_speed": 115},
    ],
}
with open("examples/2dDemo/levels.json", "w", encoding="utf-8") as f:
    json.dump(levels, f, ensure_ascii=False, indent=2)
    f.write("\n")
print("wrote levels.json")
