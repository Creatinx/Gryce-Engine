---
engine: Gryce Engine
version: v0.1.0
author: Creatinx
template: default
output: gryce_trailer_v1.mp4
---

# Gryce Engine 技术展示

## 引擎介绍
type: title
duration: 4
record: scenes/title_showcase
camera: static
text: "Gryce Engine"
subtitle: "一个 C++23 游戏引擎原型项目"
bgm_intensity: 0.4

## 渲染架构
type: feature
duration: 4
record: scenes/pbr_vulkan_demo
camera: orbit
title: "双后端 RHI"
description: "从 OpenGL 到 Vulkan，一套接口两种实现"
bgm_intensity: 0.6

## ECS架构
type: feature
duration: 3.5
record: scenes/ecs_demo
camera: flythrough
title: "ECS + 场景树混合架构"
description: "Entity-Component-System，数据驱动的高性能更新"
bgm_intensity: 0.5

## 物理引擎
type: feature
duration: 4
record: scenes/physics_destruction
camera: demo
title: "接入成熟物理库"
description: "Jolt Physics v5.2 + Box2D v3.0，统一接口封装"
bgm_intensity: 0.7

## 2D功能
type: feature
duration: 4
record: scenes/gt2d_platformer
camera: demo
title: "已实现的 2D 功能"
description: "Sprite2D、TileMap、粒子系统、法线贴图、Bloom"
bgm_intensity: 0.6

## 编辑器
type: feature
duration: 3.5
record: scenes/editor_ui
camera: static
title: "基于 ImGui 的可视化编辑器"
description: "层级面板、Inspector、场景视图、Undo/Redo"
bgm_intensity: 0.5

## 结尾
type: ending
duration: 5
record: scenes/outro_logo
camera: static
text: "Gryce Engine"
subtitle: "原型阶段 · 持续开发中"
bgm_intensity: 0.8