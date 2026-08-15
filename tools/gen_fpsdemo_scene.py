#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gryce Engine FPSDemo 场景生成器。

生成 examples/FPSDemo/scenes/main.gesc（.gesc v2 编辑器格式）。
竞技场：40x40 地面 + 四面墙（±20）+ 掩体，玩家 FPS 角色 + 6 个 CesiumMan 敌人
+ HUD（ASCII）+ 音效/音乐实体 + GameManager。

用法：python tools/gen_fpsdemo_scene.py
"""

import json
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "examples", "FPSDemo", "scenes", "main.gesc")

# 确定性 UUID（顺序自增，与旧手写场景一致；编辑器仍可加载）
_uuid_counter = 0


def uuid():
    global _uuid_counter
    _uuid_counter += 1
    return "6f3f0a6c-1d30-4b2e-9a7e-%012x" % _uuid_counter


def entity(name, pos, components, scale=(1.0, 1.0, 1.0), rot=(0.0, 0.0, 0.0, 1.0)):
    return {
        "name": name,
        "uuid": uuid(),
        "parent": None,
        "enabled": True,
        "transform": {
            "position": list(pos),
            "rotation": list(rot),
            "scale": list(scale),
        },
        "components": components,
    }


def camera_component(fov=75.0):
    return {
        "background_color": [0.12, 0.16, 0.24, 1.0],
        "enabled": True,
        "far_plane": 500.0,
        "fov": fov,
        "is_main": True,
        "near_plane": 0.1,
        "type": "Camera",
    }


def rigid_body_component(mass=1.0, gravity=True):
    return {
        "acceleration": [0.0, 0.0, 0.0],
        "angular_damping": 0.5,
        "angular_velocity": [0.0, 0.0, 0.0],
        "enabled": True,
        "friction": 0.0,
        "is_kinematic": False,
        "linear_damping": 0.0,
        "mass": mass,
        "restitution": 0.0,
        "type": "RigidBody",
        "use_gravity": gravity,
        "velocity": [0.0, 0.0, 0.0],
    }


def box_collider_component(size):
    return {
        "center": [0.0, 0.0, 0.0],
        "enabled": True,
        "is_trigger": False,
        "size": list(size),
        "type": "BoxCollider",
    }


def static_body_component():
    return {"enabled": True, "kinematic": False, "type": "StaticBody"}


def script_component(path):
    return {"enabled": True, "script_path": "res:/scripts/" + path, "type": "Script"}


def material(albedo=(0.5, 0.5, 0.52, 1.0), metallic=0.0, roughness=0.85):
    return {
        "albedo_color": list(albedo),
        "albedo_map_path": "",
        "anisotropy": 0.0,
        "anisotropy_rotation": 0.0,
        "ao": 1.0,
        "ao_map_path": "",
        "blend_mode": 0,
        "clearcoat": 0.0,
        "clearcoat_roughness": 0.1,
        "density": 1.0,
        "drag_coefficient": 0.0,
        "emissive_color": [0.0, 0.0, 0.0],
        "emissive_map_path": "",
        "metallic": metallic,
        "metallic_map_path": "",
        "name": "Default",
        "normal_map_path": "",
        "opacity": 1.0,
        "preset_name": "",
        "roughness": roughness,
        "roughness_map_path": "",
        "sheen": 0.0,
        "sheen_tint": [1.0, 1.0, 1.0],
        "softness": 0.0,
        "two_sided": False,
        "use_albedo_map": False,
        "use_ao_map": False,
        "use_emissive_map": False,
        "use_metallic_map": False,
        "use_normal_map": False,
        "use_roughness_map": False,
        "uv_offset": [0.0, 0.0],
        "uv_scale": [1.0, 1.0],
    }


def mesh_renderer(path, albedo, metallic=0.0, roughness=0.85):
    return {
        "billboard": False,
        "enabled": True,
        "material": material(albedo, metallic, roughness),
        "mesh_path": "res:/models/" + path,
        "type": "MeshRenderer",
    }


def skinned_mesh_renderer(path):
    return {
        "clip_name": "",          # CesiumMan 的动画无名字 -> 回退第一个 clip
        "enabled": True,
        "loop": True,
        "model_path": "res:/models/" + path,
        "playing": True,
        "speed": 1.0,
        "type": "SkinnedMeshRenderer",
    }


def light_component(intensity=3.0, color=(1.0, 1.0, 1.0), direction=(0.4, -1.0, -0.3)):
    return {
        "color": list(color),
        "direction": list(direction),
        "enabled": True,
        "intensity": intensity,
        "light_type": 0,          # Directional
        "range": 200.0,
        "spot_angle": 45.0,
        "spot_softness": 0.2,
        "type": "Light",
    }


def label_component(text, size=22.0, color=(1.0, 1.0, 1.0, 1.0), layer=100, order=1000):
    return {
        "canvas_layer": layer,
        "color": list(color),
        "enabled": True,
        "font_size": size,
        "render_order": order,
        "text": text,
        "type": "Label",
    }


def audio_component(clip, volume=0.5, loop=False, play_on_awake=False):
    return {
        "clip_path": "res:/audio/" + clip,
        "enabled": True,
        "is_3d": False,
        "loop": loop,
        "max_distance": 100.0,
        "min_distance": 1.0,
        "pitch": 1.0,
        "play_on_awake": play_on_awake,
        "speed": 1.0,
        "type": "AudioSource",
        "volume": volume,
    }


def wall(name, pos, scale):
    return entity(name, pos,
                  [mesh_renderer("cube_pbr.obj", (0.32, 0.34, 0.38, 1.0), roughness=0.8),
                   static_body_component(), box_collider_component((1.0, 1.0, 1.0))],
                  scale=scale)


def cover(name, pos, scale):
    return entity(name, pos,
                  [mesh_renderer("cube_pbr.obj", (0.55, 0.45, 0.3, 1.0), roughness=0.7),
                   static_body_component(), box_collider_component((1.0, 1.0, 1.0))],
                  scale=scale)


def build():
    entities = []
    # --- 玩家 ---
    entities.append(entity("PlayerCamera", (0.0, 1.6, 0.0), [camera_component()]))
    entities.append(entity("Player", (0.0, 0.8, 0.0),
                           [rigid_body_component(mass=1.0),
                            box_collider_component((0.6, 1.6, 0.6)),
                            script_component("player.lua")]))
    entities.append(entity("SpawnPoint", (0.0, 0.8, 6.0), []))
    entities.append(entity("Weapon", (0.3, 1.2, 0.5),
                           [mesh_renderer("weapon.obj", (0.28, 0.29, 0.32, 1.0), metallic=0.5, roughness=0.3)],
                           scale=(1.4, 1.4, 1.4)))
    entities.append(entity("MainLight", (0.0, 8.0, 0.0), [light_component()]))

    # --- 竞技场（40x40，墙在 ±20）---
    entities.append(entity("Ground", (0.0, -0.5, 0.0),
                           [mesh_renderer("cube_pbr.obj", (0.42, 0.45, 0.48, 1.0), roughness=0.9),
                            static_body_component(), box_collider_component((1.0, 1.0, 1.0))],
                           scale=(40.0, 1.0, 40.0)))
    entities.append(wall("Wall_N", (0.0, 1.0, -20.0), (40.0, 2.0, 1.0)))
    entities.append(wall("Wall_S", (0.0, 1.0, 20.0), (40.0, 2.0, 1.0)))
    entities.append(wall("Wall_E", (20.0, 1.0, 0.0), (1.0, 2.0, 40.0)))
    entities.append(wall("Wall_W", (-20.0, 1.0, 0.0), (1.0, 2.0, 40.0)))
    entities.append(cover("Cover_1", (5.0, 0.5, 3.0), (3.0, 1.0, 1.6)))
    entities.append(cover("Cover_2", (-4.0, 0.5, -3.0), (2.5, 1.0, 1.2)))
    entities.append(cover("Cover_3", (3.0, 0.5, -7.0), (1.6, 1.0, 2.8)))
    entities.append(cover("Cover_4", (-6.0, 0.5, 9.0), (3.2, 1.0, 1.0)))

    # --- 敌人（6 个，CesiumMan 单动画；脚本控制 playing 做待机/追击）---
    enemy_positions = [
        (0.0, 0.75, -15.0),
        (13.0, 0.75, 12.0),
        (-14.0, 0.75, 9.0),
        (16.0, 0.75, -9.0),
        (-13.0, 0.75, -13.0),
        (7.0, 0.75, 15.0),
    ]
    for i, pos in enumerate(enemy_positions, 1):
        entities.append(entity("Enemy%d" % i, pos,
                               [skinned_mesh_renderer("enemy.glb"),
                                script_component("enemy.lua")]))

    # --- HUD（ASCII：Roboto 无 CJK 字形，fallback 只覆盖 ASCII）---
    hud = [
        ("HealthLabel", "HP: 100", (0.05, 0.82)),
        ("KillsLabel", "Kills: 0", (0.05, 0.76)),
        ("LivesLabel", "Lives: 5", (0.05, 0.70)),
        ("EnemiesLabel", "Enemies: 6", (0.05, 0.64)),
        ("Crosshair", "+", (0.0, 0.0)),
        ("MessageLabel", "WASD move, mouse aim, LMB shoot", (0.0, 0.35)),
    ]
    for name, text, _ in hud:
        entities.append(entity(name, (0.0, 0.0, 0.0), [label_component(text)]))

    # --- 音效 / 音乐 ---
    entities.append(entity("BGM", (0.0, 0.0, 0.0),
                           [audio_component("bgm.wav", volume=0.4, loop=True)]))
    for name, clip, vol in [
        ("SFX_Shoot", "shoot.wav", 0.5),
        ("SFX_Hit", "hit.ogg", 0.6),
        ("SFX_Hurt", "hurt.ogg", 0.6),
        ("SFX_EnemyDead", "enemy_dead.ogg", 0.6),
        ("SFX_Jump", "jump.wav", 0.4),
    ]:
        entities.append(entity(name, (0.0, 0.0, 0.0), [audio_component(clip, volume=vol)]))

    # --- GameManager ---
    entities.append(entity("GameManager", (0.0, 0.0, 0.0), [script_component("game_manager.lua")]))

    scene = {"version": 2, "name": "FPSDemo_main", "entities": entities}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(scene, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("wrote", OUT, "entities:", len(entities))


if __name__ == "__main__":
    build()
