#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <random>
#include <algorithm>
#include <format>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include "platform/window.h"
#include "platform/input.h"
#include "render/render_context.h"
#include "render/render2d.h"
#include "render/opengl/imgui_renderer.h"
#include "resources/project.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/2d/component_2d.h"
#include "components/2d/label.h"
#include "components/2d/sprite_2d.h"
#include "components/2d/light_2d.h"
#include "components/2d/camera_2d.h"
#include "components/2d/parallax_background.h"
#include "components/2d/particle_emitter.h"
#include "components/2d/tilemap.h"
#include "components/audio_source.h"
#include "components/physical_material.h"
#include "components/rigid_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
#include "components/component_factory.h"
#include "ecs/world.h"
#include "ecs/systems/physics_system_2d.h"
#include "ecs/systems/render_system_2d.h"
#include "ecs/query.h"
#include "ui/debug_panel.h"
#include "utils/glog/glog_lib.h"
#include "utils/frame_limiter.h"

using namespace gryce_engine;

namespace {

constexpr float k_pi = 3.14159265358979323846f;
constexpr float k_tile_size = 32.0f;
constexpr int k_map_w = 140;
constexpr int k_map_h = 24;
constexpr float k_world_w = k_map_w * k_tile_size;
constexpr float k_world_h = k_map_h * k_tile_size;

constexpr float k_player_speed = 240.0f;
constexpr float k_jump_height_tiles = 6.0f; // 目标弹跳高度（瓦片数），会根据当前重力自动调整起跳速度
constexpr float k_enemy_speed = 90.0f;
constexpr float k_stomp_bounce = -320.0f;
constexpr float k_hurt_knockback_x = 220.0f;
constexpr float k_hurt_knockback_y = -280.0f;
constexpr float k_invulnerable_time = 1.2f;

// ---------------------------------------------------------------------------
// 重力预设：太阳系 8 大行星（以地球游戏重力 1150 为基准按比例缩放）
// ---------------------------------------------------------------------------
struct GravityPreset {
    const char* name;
    float gravity; // 世界单位/秒²，Y 轴向下为正
};

constexpr GravityPreset k_gravity_presets[] = {
    {"Mercury",  434.0f},
    {"Venus",   1040.0f},
    {"Earth",   1150.0f},
    {"Mars",     435.0f},
    {"Jupiter", 2906.0f},
    {"Saturn",  1224.0f},
    {"Uranus",  1040.0f},
    {"Neptune", 1307.0f},
};
constexpr int k_gravity_preset_count = static_cast<int>(sizeof(k_gravity_presets) / sizeof(k_gravity_presets[0]));

// ---------------------------------------------------------------------------
// 运行时组件
// ---------------------------------------------------------------------------
struct Coin : public components::Component {
    int value = 10;
    float radius = 10.0f;
    bool collected = false;

    const char* type() const override { return "Coin"; }
    void serialize(nlohmann::json&) const override {}
    void deserialize(const nlohmann::json&) override {}
};

struct Enemy : public components::Component {
    float speed = k_enemy_speed;
    int direction = 1; // +1 right, -1 left
    float min_x = 0.0f;
    float max_x = 0.0f;

    const char* type() const override { return "Enemy"; }
    void serialize(nlohmann::json&) const override {}
    void deserialize(const nlohmann::json&) override {}
};

struct PlayerTag : public components::Component {
    const char* type() const override { return "PlayerTag"; }
    void serialize(nlohmann::json&) const override {}
    void deserialize(const nlohmann::json&) override {}
};

struct Bullet : public components::Component {
    float lifetime = 2.0f;
    float speed = 720.0f;
    math::Vector2f velocity;

    const char* type() const override { return "Bullet"; }
    void serialize(nlohmann::json&) const override {}
    void deserialize(const nlohmann::json&) override {}
};

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
struct AABB {
    float min_x, min_y, max_x, max_y;
};

AABB get_aabb(scene::Entity* e) {
    math::Vector2f pos(e->transform()->position.x, e->transform()->position.y);
    math::Vector2f size(28.0f, 28.0f);
    if (auto* col = e->get_component<components::BoxCollider2D>()) {
        size = col->size;
    }
    return { pos.x - size.x * 0.5f, pos.y - size.y * 0.5f,
             pos.x + size.x * 0.5f, pos.y + size.y * 0.5f };
}

bool aabb_overlap(const AABB& a, const AABB& b) {
    return a.min_x < b.max_x && a.max_x > b.min_x &&
           a.min_y < b.max_y && a.max_y > b.min_y;
}

// 平台角色踩踏判定：玩家从上方落到敌人头顶时触发
bool is_stomp(scene::Entity* player, components::RigidBody2D* player_rb, scene::Entity* enemy) {
    if (!player || !player_rb || !enemy) return false;
    AABB pa = get_aabb(player);
    AABB ea = get_aabb(enemy);
    if (!aabb_overlap(pa, ea)) return false;

    math::Vector2f pp(player->transform()->position.x, player->transform()->position.y);
    math::Vector2f ep(enemy->transform()->position.x, enemy->transform()->position.y);

    // 玩家中心必须在敌人中心上方
    if (pp.y >= ep.y) return false;
    // 玩家必须正在下落（Y 轴向下为正）
    if (player_rb->velocity.y <= 0.0f) return false;

    // 玩家底边应落在敌人顶边附近
    float player_bottom = pa.max_y;
    float enemy_top = ea.min_y;
    float vertical_penetration = player_bottom - enemy_top;
    // 横向重叠必须明显大于纵向穿透，确保是“站在头顶”而不是侧面擦碰
    float overlap_left = std::max(pa.min_x, ea.min_x);
    float overlap_right = std::min(pa.max_x, ea.max_x);
    float horizontal_overlap = overlap_right - overlap_left;

    return vertical_penetration >= -2.0f && vertical_penetration < 12.0f &&
           horizontal_overlap > 4.0f;
}

void spawn_bullet(scene::Scene* scene, const math::Vector3f& from, const math::Vector2f& target) {
    if (!scene) return;
    scene::Entity* e = scene->create_entity("Bullet");
    e->transform()->position = from;

    auto* sprite = e->add_component<components::d2::sprite::Sprite2D>("res:/textures/bullet.png", 12.0f, 12.0f);
    sprite->color = render::Color(1.0f, 0.95f, 0.40f, 1.0f);
    sprite->lit = true;
    sprite->render_order = 20;

    // 子弹发光，用于测试 2D 光照
    auto* light = e->add_component<components::d2::light::Light2D>(
        render::Color(1.0f, 0.85f, 0.25f, 1.0f), 2.5f, 90.0f);
    light->render_order = 100;

    auto* rb = e->add_component<components::RigidBody2D>();
    rb->mass = 0.05f;
    rb->use_gravity = false;
    rb->is_kinematic = false;
    rb->restitution = 0.0f;
    rb->friction = 0.0f;

    auto* col = e->add_component<components::CircleCollider2D>();
    col->radius = 5.0f;

    auto* bullet = e->add_component<Bullet>();
    math::Vector2f dir = target - math::Vector2f(from.x, from.y);
    if (dir.length_sq() < 1e-6f) {
        dir = math::Vector2f(1.0f, 0.0f);
    } else {
        dir = dir.normalized();
    }
    bullet->velocity = dir * bullet->speed;
    rb->velocity = bullet->velocity;
}

void update_bullets(scene::Scene* scene, float dt, int& score,
                    components::AudioSource* sfx_stomp,
                    components::d2::ParticleEmitter2D* hit_fx) {
    if (!scene) return;

    std::vector<scene::Entity*> bullets_to_remove;
    std::vector<scene::Entity*> enemies_to_destroy;

    ecs::foreach_with_component<Bullet>(*scene, [&](scene::Entity* e, Bullet* bullet) {
        bullet->lifetime -= dt;
        if (bullet->lifetime <= 0.0f) {
            bullets_to_remove.push_back(e);
            return;
        }

        // 同步刚体速度（运动学子弹）
        if (auto* rb = e->get_component<components::RigidBody2D>()) {
            rb->velocity = bullet->velocity;
        }

        AABB ba = get_aabb(e);
        // 子弹与敌人 AABB 碰撞
        ecs::foreach_with_component<Enemy>(*scene, [&](scene::Entity* enemy, Enemy*) {
            if (aabb_overlap(ba, get_aabb(enemy))) {
                enemies_to_destroy.push_back(enemy);
                bullets_to_remove.push_back(e);
                score += 25;
                if (sfx_stomp) sfx_stomp->play();
                if (hit_fx && hit_fx->owner()) {
                    hit_fx->owner()->transform()->position = enemy->transform()->position;
                    hit_fx->burst();
                }
            }
        });
    });

    for (auto* e : enemies_to_destroy) {
        scene->destroy_entity(e);
    }
    for (auto* e : bullets_to_remove) {
        scene->destroy_entity(e);
    }
}

std::mt19937& rng() {
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

float randf(float min, float max) {
    if (min >= max) return min;
    std::uniform_real_distribution<float> dist(min, max);
    return dist(rng());
}

int randi(int min, int max) {
    if (min >= max) return min;
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng());
}

std::filesystem::path find_project_root() {
    std::filesystem::path exe_path;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        exe_path = std::filesystem::path(buffer);
    }
#else
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, PATH_MAX - 1);
    if (len > 0) {
        buffer[len] = '\0';
        exe_path = std::filesystem::path(buffer);
    }
#endif
    std::filesystem::path dir = exe_path.parent_path();
    // 先向上查找引擎仓库根（包含 CMakeLists.txt 与 core/ 目录）
    std::filesystem::path engine_root;
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "CMakeLists.txt") &&
            std::filesystem::is_directory(dir / "core")) {
            engine_root = dir;
            break;
        }
        dir = dir.parent_path();
    }
    if (!engine_root.empty()) {
        // 再进入 examples/<exe_name>/ 作为项目根
        std::string exe_name = exe_path.stem().string();
        std::filesystem::path candidate = engine_root / "examples" / exe_name;
        if (std::filesystem::exists(candidate / "project.gryce")) {
            return candidate;
        }
    }
    return std::filesystem::current_path();
}

// 瓦片索引：0=草皮 1=泥土 2=砖块 3=石块 4=砖墙 5=星星 6+=灰块
void build_level_tilemap(components::d2::tilemap::Tilemap* tm) {
    // 地面：草皮 + 两层泥土
    int surface_y = k_map_h - 3;
    for (int x = 0; x < k_map_w; ++x) {
        tm->set_tile(x, surface_y, 0);     // grass
        tm->set_tile(x, surface_y + 1, 1); // dirt
        tm->set_tile(x, surface_y + 2, 1); // dirt
    }

    // 一些坑洞（小心跳过去）
    for (int x = 22; x <= 26; ++x) {
        tm->set_tile(x, surface_y, -1);
        tm->set_tile(x, surface_y + 1, -1);
        tm->set_tile(x, surface_y + 2, -1);
    }
    for (int x = 58; x <= 63; ++x) {
        tm->set_tile(x, surface_y, -1);
        tm->set_tile(x, surface_y + 1, -1);
        tm->set_tile(x, surface_y + 2, -1);
    }
    for (int x = 105; x <= 110; ++x) {
        tm->set_tile(x, surface_y, -1);
        tm->set_tile(x, surface_y + 1, -1);
        tm->set_tile(x, surface_y + 2, -1);
    }

    auto platform = [&](int x1, int x2, int y, int tile) {
        for (int x = x1; x <= x2; ++x) tm->set_tile(x, y, tile);
    };

    // 平台
    platform(10, 16, surface_y - 4, 2);
    platform(34, 42, surface_y - 3, 2);
    platform(48, 54, surface_y - 6, 2);
    platform(72, 80, surface_y - 4, 2);
    platform(86, 94, surface_y - 7, 2);
    platform(118, 126, surface_y - 5, 2);
    platform(130, 137, surface_y - 3, 2);

    // 高处砖块障碍
    platform(28, 30, surface_y - 2, 2);
    platform(65, 68, surface_y - 5, 2);
    platform(98, 101, surface_y - 3, 2);
}

scene::Scene* create_platformer_scene() {
    auto scene = std::make_unique<scene::Scene>("Platformer");

    // 星空背景
    {
        scene::Entity* e = scene->create_entity("Starfield");
        auto* bg = e->add_component<components::d2::parallax::ParallaxBackground>();
        bg->layers.push_back({"res:/textures/parallax_stars.png", 0.05f, 1.4f,
                              render::Color(0.25f, 0.25f, 0.40f, 1.0f)});
        bg->layers.push_back({"res:/textures/parallax_stars.png", 0.15f, 1.0f,
                              render::Color(0.18f, 0.18f, 0.30f, 1.0f)});
        bg->layers.push_back({"res:/textures/parallax_stars.png", 0.35f, 0.7f,
                              render::Color(0.10f, 0.10f, 0.18f, 1.0f)});
    }

    // 瓦片地图（受光照）
    {
        scene::Entity* e = scene->create_entity("Level");
        auto* tm = e->add_component<components::d2::tilemap::Tilemap>(k_map_w, k_map_h, k_tile_size, k_tile_size);
        tm->tileset_path = "res:/tilesets/default.json";
        tm->generate_colliders = true;
        tm->use_tileset_texture = true;
        tm->lit = true;
        tm->render_order = -100;
        build_level_tilemap(tm);
    }

    // 玩家
    {
        scene::Entity* e = scene->create_entity("Player");
        e->transform()->position = math::Vector3f(3.0f * k_tile_size, (k_map_h - 6) * k_tile_size, 0.0f);
        e->add_component<PlayerTag>();

        auto* sprite = e->add_component<components::d2::sprite::Sprite2D>("res:/textures/player.png", 28.0f, 28.0f);
        sprite->color = render::Color::white();
        sprite->lit = true;
        sprite->render_order = 10;

        auto* rb = e->add_component<components::RigidBody2D>();
        rb->mass = 1.0f;
        rb->use_gravity = true;
        rb->restitution = 0.0f;
        rb->friction = 0.2f;
        rb->linear_damping = 0.02f;

        auto* pm = e->add_component<components::PhysicalMaterial>();
        if (pm) {
            pm->apply_preset("Rubber");
        }

        auto* col = e->add_component<components::BoxCollider2D>();
        col->size = math::Vector2f(28.0f, 28.0f);

        // 手电筒
        auto* light = e->add_component<components::d2::light::Light2D>(
            render::Color(1.0f, 0.95f, 0.70f, 1.0f), 1.7f, 260.0f);
        light->render_order = 100;
    }

    // 摄像机
    {
        scene::Entity* e = scene->create_entity("MainCamera");
        e->transform()->position = math::Vector3f(640.0f, 360.0f, 0.0f);
        auto* cam = e->add_component<components::d2::camera::Camera2D>();
        cam->is_active = true;
        cam->zoom = 1.0f;
    }

    // 音效
    auto make_sfx = [&](const std::string& name, const std::string& path, float volume) {
        scene::Entity* e = scene->create_entity(name);
        auto* audio = e->add_component<components::AudioSource>();
        audio->clip_path = path;
        audio->volume = volume;
        audio->is_3d = false;
        return audio;
    };
    make_sfx("SFX_Jump", "res:/audio/jump.wav", 0.45f);
    make_sfx("SFX_Coin", "res:/audio/coin.wav", 0.40f);
    make_sfx("SFX_Stomp", "res:/audio/stomp.wav", 0.50f);
    make_sfx("SFX_Hurt", "res:/audio/hurt.wav", 0.55f);

    // 粒子：跳跃尘土 + 命中爆炸
    {
        scene::Entity* e = scene->create_entity("JumpDust");
        auto* pe = e->add_component<components::d2::ParticleEmitter2D>();
        pe->emission_offset = math::Vector2f(0.0f, 14.0f);
        pe->direction_min = k_pi * 0.40f;
        pe->direction_max = k_pi * 0.60f;
        pe->velocity_min = 40.0f;
        pe->velocity_max = 110.0f;
        pe->acceleration = math::Vector2f(0.0f, 200.0f);
        pe->lifetime_min = 0.2f;
        pe->lifetime_max = 0.45f;
        pe->start_color = render::Color(0.75f, 0.55f, 0.30f, 1.0f);
        pe->end_color = render::Color(0.75f, 0.55f, 0.30f, 0.0f);
        pe->start_size = 5.0f;
        pe->end_size = 1.0f;
        pe->angular_velocity_min = -120.0f;
        pe->angular_velocity_max = 120.0f;
        pe->render_order = 15;
    }
    {
        scene::Entity* e = scene->create_entity("HitFx");
        auto* pe = e->add_component<components::d2::ParticleEmitter2D>();
        pe->burst_min = 14;
        pe->burst_max = 22;
        pe->velocity_min = 50.0f;
        pe->velocity_max = 180.0f;
        pe->direction_min = 0.0f;
        pe->direction_max = 2.0f * k_pi;
        pe->acceleration = math::Vector2f(0.0f, 120.0f);
        pe->lifetime_min = 0.25f;
        pe->lifetime_max = 0.55f;
        pe->start_color = render::Color(1.0f, 0.35f, 0.15f, 1.0f);
        pe->end_color = render::Color(0.4f, 0.05f, 0.05f, 0.0f);
        pe->start_size = 6.0f;
        pe->end_size = 1.0f;
        pe->rotation_min = 0.0f;
        pe->rotation_max = k_pi;
        pe->angular_velocity_min = -270.0f;
        pe->angular_velocity_max = 270.0f;
        pe->render_order = 50;
    }

    // UI
    auto make_label = [&](const std::string& name, const std::string& text, float x, float y, const render::Color& c) {
        scene::Entity* e = scene->create_entity(name);
        e->transform()->position = math::Vector3f(x, y, 0.0f);
        auto* lbl = e->add_component<components::d2::text::Label>(text, 20.0f, c);
        lbl->render_order = 1100;
        return lbl;
    };
    make_label("ScoreLabel", "Score: 0", 12.0f, 28.0f, render::Color::white());
    make_label("CoinsLabel", "Coins: 0", 12.0f, 54.0f, render::Color::yellow());
    make_label("LivesLabel", "Lives: 3", 12.0f, 80.0f, render::Color::white());
    make_label("HintLabel", "A/D move | Space/W jump | Mouse aim/shoot | R restart", 12.0f, 106.0f, render::Color::gray(0.75f));
    make_label("GameOverLabel", "", 12.0f, 140.0f, render::Color::red());
    make_label("FPSLabel", "Render FPS: --", 12.0f, 174.0f, render::Color::green());

    return scene.release();
}

void spawn_coin(scene::Scene* scene, float x, float y) {
    if (!scene) return;
    scene::Entity* e = scene->create_entity("Coin");
    e->transform()->position = math::Vector3f(x, y, 0.0f);

    auto* sprite = e->add_component<components::d2::sprite::Sprite2D>("res:/textures/coin.png", 18.0f, 18.0f);
    sprite->color = render::Color::white();
    sprite->lit = true;
    sprite->render_order = 5;

    auto* light = e->add_component<components::d2::light::Light2D>(
        render::Color(1.0f, 0.8f, 0.1f, 1.0f), 2.0f, 55.0f);
    light->render_order = 100;

    e->add_component<Coin>();
}

void spawn_enemy(scene::Scene* scene, float x, float y, float patrol_w) {
    if (!scene) return;
    scene::Entity* e = scene->create_entity("Enemy");
    e->transform()->position = math::Vector3f(x, y, 0.0f);

    auto* sprite = e->add_component<components::d2::sprite::Sprite2D>("res:/textures/enemy.png", 28.0f, 28.0f);
    sprite->color = render::Color::white();
    sprite->lit = true;
    sprite->render_order = 8;

    auto* rb = e->add_component<components::RigidBody2D>();
    rb->mass = 1.0f;
    rb->use_gravity = true;
    rb->restitution = 0.1f;
    rb->friction = 0.3f;
    rb->linear_damping = 0.0f;
    rb->velocity.x = k_enemy_speed;

    auto* col = e->add_component<components::BoxCollider2D>();
    col->size = math::Vector2f(28.0f, 28.0f);

    auto* enemy = e->add_component<Enemy>();
    enemy->min_x = x - patrol_w * 0.5f;
    enemy->max_x = x + patrol_w * 0.5f;
}

void populate_level(scene::Scene* scene) {
    if (!scene) return;

    int surface_y = k_map_h - 3;

    // 金币：地面、平台上
    auto place_coins = [&](int tx1, int tx2, int ty, int spacing) {
        for (int x = tx1; x <= tx2; x += spacing) {
            spawn_coin(scene, x * k_tile_size, ty * k_tile_size);
        }
    };

    place_coins(7, 9, surface_y - 2, 1);
    place_coins(12, 16, surface_y - 6, 1);
    place_coins(36, 42, surface_y - 5, 1);
    place_coins(50, 54, surface_y - 8, 1);
    place_coins(74, 80, surface_y - 6, 1);
    place_coins(88, 94, surface_y - 9, 1);
    place_coins(120, 126, surface_y - 7, 1);
    place_coins(132, 137, surface_y - 5, 1);

    // 敌人巡逻
    spawn_enemy(scene, 19.0f * k_tile_size, (surface_y - 1) * k_tile_size, 5.0f * k_tile_size);
    spawn_enemy(scene, 38.0f * k_tile_size, (surface_y - 5) * k_tile_size, 4.0f * k_tile_size);
    spawn_enemy(scene, 76.0f * k_tile_size, (surface_y - 6) * k_tile_size, 4.0f * k_tile_size);
    spawn_enemy(scene, 90.0f * k_tile_size, (surface_y - 9) * k_tile_size, 4.0f * k_tile_size);
    spawn_enemy(scene, 122.0f * k_tile_size, (surface_y - 7) * k_tile_size, 4.0f * k_tile_size);
}

void reset_game(scene::Scene* scene, int& score, int& coins, int& lives, bool& game_over) {
    score = 0;
    coins = 0;
    lives = 3;
    game_over = false;

    // 销毁所有金币、敌人和子弹
    std::vector<scene::Entity*> to_destroy;
    ecs::foreach_with_component<Coin>(*scene, [&](scene::Entity* e, Coin*) {
        to_destroy.push_back(e);
    });
    ecs::foreach_with_component<Enemy>(*scene, [&](scene::Entity* e, Enemy*) {
        to_destroy.push_back(e);
    });
    ecs::foreach_with_component<Bullet>(*scene, [&](scene::Entity* e, Bullet*) {
        to_destroy.push_back(e);
    });
    for (auto* e : to_destroy) {
        scene->destroy_entity(e);
    }

    populate_level(scene);

    if (scene::Entity* player = scene->find_entity_by_name("Player")) {
        player->transform()->position = math::Vector3f(3.0f * k_tile_size, (k_map_h - 6) * k_tile_size, 0.0f);
        if (auto* rb = player->get_component<components::RigidBody2D>()) {
            rb->velocity = math::Vector2f::zero();
            rb->acceleration = math::Vector2f::zero();
        }
    }

    if (scene::Entity* go = scene->find_entity_by_name("GameOverLabel")) {
        if (auto* lbl = go->get_component<components::d2::text::Label>()) {
            lbl->text = "";
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    float auto_close_seconds = 0.0f;
    std::string screenshot_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--auto-close") == 0 && i + 1 < argc) {
            auto_close_seconds = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        }
    }

    std::cout << "Gryce Engine - 2D Platformer Demo" << std::endl;

    utils::glog_initialize();
    utils::GLog::instance().set_min_level(utils::LogLevel::Info);

    resources::Project::instance().set_root(find_project_root().string());
    components::register_builtin_components();

    if (!platform::Window::init_sdk()) {
        GLOG_ERROR("Failed to initialize GLFW");
        return -1;
    }

    platform::Window window("Gryce Engine - 2D Platformer", 1280, 720,
                            platform::WindowMode::Windowed,
                            platform::WindowContextType::OpenGL);
    if (!window.is_valid()) {
        GLOG_ERROR("Failed to create window");
        platform::Window::shutdown_sdk();
        return -1;
    }

    render::RenderContext render_ctx;
    if (!render_ctx.init(window.native_handle(), render::RenderAPI::OpenGL)) {
        GLOG_ERROR("Failed to initialize render context");
        platform::Window::shutdown_sdk();
        return -1;
    }

    auto renderer2d = render_ctx.create_renderer2d();
    if (renderer2d) {
        renderer2d->init(&render_ctx);
    }

    render::ImGuiRenderer imgui;
    auto imgui_backend = render_ctx.create_imgui_backend();
    imgui.init(window.native_handle(), std::move(imgui_backend));

    ecs::World world;
    auto scene = std::unique_ptr<scene::Scene>(create_platformer_scene());
    populate_level(scene.get());
    world.attach_scene(std::move(scene));
    world.add_system<ecs::PhysicsSystem2D>();
    if (renderer2d) {
        world.add_system<ecs::RenderSystem2D>(renderer2d.get());
    }
    world.init();

    int gravity_index = 2; // 默认地球
    float current_gravity = k_gravity_presets[gravity_index].gravity;

    ecs::PhysicsSystem2D* phys = world.get_system<ecs::PhysicsSystem2D>();
    if (phys) {
        phys->gravity = math::Vector2f(0.0f, current_gravity);
    }

    platform::InputManager input;
    input.update(&window);

    // 查找对象
    scene::Entity* player_entity = nullptr;
    components::RigidBody2D* player_rb = nullptr;
    components::d2::camera::Camera2D* main_camera = nullptr;
    components::AudioSource* sfx_jump = nullptr;
    components::AudioSource* sfx_coin = nullptr;
    components::AudioSource* sfx_stomp = nullptr;
    components::AudioSource* sfx_hurt = nullptr;
    components::d2::ParticleEmitter2D* jump_dust = nullptr;
    components::d2::ParticleEmitter2D* hit_fx = nullptr;
    components::d2::text::Label* score_label = nullptr;
    components::d2::text::Label* coins_label = nullptr;
    components::d2::text::Label* lives_label = nullptr;
    components::d2::text::Label* game_over_label = nullptr;
    components::d2::text::Label* fps_label = nullptr;
    components::PhysicalMaterial* player_pm = nullptr;
    ecs::RenderSystem2D* render_sys = world.get_system<ecs::RenderSystem2D>();

    auto refresh_pointers = [&]() {
        if (!world.scene()) return;
        player_entity = world.scene()->find_entity_by_name("Player");
        if (player_entity) {
            player_rb = player_entity->get_component<components::RigidBody2D>();
            player_pm = player_entity->get_component<components::PhysicalMaterial>();
        } else {
            player_pm = nullptr;
        }
        if (scene::Entity* cam = world.scene()->find_entity_by_name("MainCamera")) {
            main_camera = cam->get_component<components::d2::camera::Camera2D>();
        }
        auto find_audio = [&](const std::string& name) -> components::AudioSource* {
            if (scene::Entity* e = world.scene()->find_entity_by_name(name)) {
                return e->get_component<components::AudioSource>();
            }
            return nullptr;
        };
        sfx_jump = find_audio("SFX_Jump");
        sfx_coin = find_audio("SFX_Coin");
        sfx_stomp = find_audio("SFX_Stomp");
        sfx_hurt = find_audio("SFX_Hurt");
        auto find_fx = [&](const std::string& name) -> components::d2::ParticleEmitter2D* {
            if (scene::Entity* e = world.scene()->find_entity_by_name(name)) {
                return e->get_component<components::d2::ParticleEmitter2D>();
            }
            return nullptr;
        };
        jump_dust = find_fx("JumpDust");
        hit_fx = find_fx("HitFx");
        auto find_lbl = [&](const std::string& name) -> components::d2::text::Label* {
            if (scene::Entity* e = world.scene()->find_entity_by_name(name)) {
                return e->get_component<components::d2::text::Label>();
            }
            return nullptr;
        };
        score_label = find_lbl("ScoreLabel");
        coins_label = find_lbl("CoinsLabel");
        lives_label = find_lbl("LivesLabel");
        game_over_label = find_lbl("GameOverLabel");
        fps_label = find_lbl("FPSLabel");
    };
    refresh_pointers();

    int score = 0;
    int coins = 0;
    int lives = 3;
    bool game_over = false;

    float invulnerable_timer = 0.0f;
    float jump_cooldown = 0.0f;
    bool jump_pressed_last = false;

    int rendered_frames = 0;
    float fps_update_timer = 0.0f;
    math::Vector2f mouse_world;

    render_ctx.start();
    utils::FrameLimiter frame_limiter;
    frame_limiter.set_target_fps(0);

    double auto_close_timer = 0.0;

    while (!window.should_close()) {
        frame_limiter.begin_frame();
        window.update_frame_stats();

        if (auto_close_seconds > 0.0f) {
            auto_close_timer += window.delta_time();
            if (auto_close_timer >= static_cast<double>(auto_close_seconds)) {
                GLOG_INFO("Auto-close after {} seconds", auto_close_seconds);
                window.request_close();
                break;
            }
        }

        window.poll_events();
        input.update(&window);

        int w = 0, h = 0;
        window.get_size(w, h);
        render_ctx.set_viewport(0, 0, w, h);

        // 计算鼠标世界坐标（用于射击瞄准）
        {
            math::Vector2f screen_center(w * 0.5f, h * 0.5f);
            math::Vector2f mouse_screen(static_cast<float>(input.mouse_x()), static_cast<float>(input.mouse_y()));
            float zoom = (main_camera && main_camera->owner()) ? main_camera->zoom : 1.0f;
            math::Vector2f cam_center = (main_camera && main_camera->owner()) ? main_camera->center() : screen_center;
            mouse_world = cam_center + (mouse_screen - screen_center) / zoom;
        }

        float dt = static_cast<float>(window.delta_time());
        if (dt > 0.1f) dt = 0.1f;

        if (input.is_key_pressed(GLFW_KEY_R)) {
            reset_game(world.scene(), score, coins, lives, game_over);
            refresh_pointers();
            invulnerable_timer = 0.0f;
            jump_cooldown = 0.0f;
        }

        if (invulnerable_timer > 0.0f) invulnerable_timer -= dt;
        if (jump_cooldown > 0.0f) jump_cooldown -= dt;

        // 玩家移动、跳跃
        if (!game_over && player_entity && player_rb) {
            math::Vector3f& pos = player_entity->transform()->position;

            bool left = input.is_key_held(GLFW_KEY_A) || input.is_key_held(GLFW_KEY_LEFT);
            bool right = input.is_key_held(GLFW_KEY_D) || input.is_key_held(GLFW_KEY_RIGHT);
            bool jump = input.is_key_held(GLFW_KEY_SPACE) || input.is_key_held(GLFW_KEY_W) ||
                        input.is_key_held(GLFW_KEY_UP);

            float target_vx = 0.0f;
            if (left) target_vx = -k_player_speed;
            if (right) target_vx = k_player_speed;
            player_rb->velocity.x = target_vx;

            // 跳跃：竖直速度接近 0 视为着地（简化）
            bool grounded = std::abs(player_rb->velocity.y) < 18.0f;
            if (jump && !jump_pressed_last && grounded && jump_cooldown <= 0.0f) {
                // 根据当前重力自动计算起跳速度，保证不同星球弹跳高度一致
                float jump_speed = std::sqrt(2.0f * current_gravity * k_jump_height_tiles * k_tile_size);
                player_rb->velocity.y = -jump_speed;
                jump_cooldown = 0.18f;
                if (sfx_jump) sfx_jump->play();
                if (jump_dust && jump_dust->owner()) {
                    jump_dust->owner()->transform()->position = pos;
                    jump_dust->burst();
                }
            }
            jump_pressed_last = jump;

            // 边界：不能退到摄像机左侧外，右侧到关卡末尾
            if (pos.x < k_tile_size) pos.x = k_tile_size;
            if (pos.x > k_world_w - k_tile_size) pos.x = k_world_w - k_tile_size;

            // 掉出地图
            if (pos.y > k_world_h + 100.0f) {
                lives--;
                if (lives <= 0) {
                    lives = 0;
                    game_over = true;
                } else {
                    pos = math::Vector3f(3.0f * k_tile_size, (k_map_h - 6) * k_tile_size, 0.0f);
                    player_rb->velocity = math::Vector2f::zero();
                    invulnerable_timer = k_invulnerable_time;
                    if (sfx_hurt) sfx_hurt->play();
                }
            }
        }

        // 摄像机跟随（带边界限制）
        if (main_camera && main_camera->owner() && player_entity) {
            float half_screen_w = w * 0.5f;
            float half_screen_h = h * 0.5f;
            math::Vector2f target(player_entity->transform()->position.x,
                                  player_entity->transform()->position.y - 60.0f);
            math::Vector2f current = main_camera->center();
            math::Vector2f next(
                current.x + (target.x - current.x) * 0.12f,
                current.y + (target.y - current.y) * 0.08f
            );
            next.x = std::clamp(next.x, half_screen_w, k_world_w - half_screen_w);
            next.y = std::clamp(next.y, half_screen_h, k_world_h - half_screen_h);
            main_camera->owner()->transform()->position = math::Vector3f(next.x, next.y, 0.0f);
        }

        // 物理更新
        world.update(dt);

        // 敌人巡逻
        if (!game_over) {
            ecs::foreach_with_component<Enemy>(*world.scene(), [&](scene::Entity* e, Enemy* enemy) {
                auto* rb = e->get_component<components::RigidBody2D>();
                if (!rb) return;
                float x = e->transform()->position.x;
                if (x <= enemy->min_x) enemy->direction = 1;
                if (x >= enemy->max_x) enemy->direction = -1;
                rb->velocity.x = enemy->speed * static_cast<float>(enemy->direction);
            });
        }

        // 金币收集
        std::vector<scene::Entity*> coins_to_collect;
        if (!game_over && player_entity) {
            math::Vector2f pp(player_entity->transform()->position.x,
                              player_entity->transform()->position.y);
            ecs::foreach_with_component<Coin>(*world.scene(), [&](scene::Entity* e, Coin* coin) {
                if (coin->collected) return;
                math::Vector2f cp(e->transform()->position.x,
                                  e->transform()->position.y);
                float dist_sq = (cp - pp).length_sq();
                float radius_sum = coin->radius + 14.0f;
                if (dist_sq <= radius_sum * radius_sum) {
                    coins_to_collect.push_back(e);
                }
            });
        }
        for (auto* e : coins_to_collect) {
            score += 10;
            coins++;
            if (sfx_coin) sfx_coin->play();
            world.scene()->destroy_entity(e);
        }

        // 玩家射击（鼠标左键）
        bool shoot = input.is_mouse_button_pressed(0);
        if (!game_over && shoot && player_entity) {
            spawn_bullet(world.scene(), player_entity->transform()->position, mouse_world);
        }

        // 子弹更新与碰撞
        update_bullets(world.scene(), dt, score, sfx_stomp, hit_fx);

        // 敌人交互：使用 AABB + 穿透方向判定踩踏/受伤
        std::vector<scene::Entity*> enemies_to_destroy;
        bool player_hurt_this_frame = false;
        if (!game_over && player_entity && player_rb) {
            ecs::foreach_with_component<Enemy>(*world.scene(), [&](scene::Entity* e, Enemy*) {
                if (is_stomp(player_entity, player_rb, e)) {
                    enemies_to_destroy.push_back(e);
                    player_rb->velocity.y = k_stomp_bounce;
                    score += 50;
                    if (sfx_stomp) sfx_stomp->play();
                    if (hit_fx && hit_fx->owner()) {
                        hit_fx->owner()->transform()->position = e->transform()->position;
                        hit_fx->burst();
                    }
                } else if (aabb_overlap(get_aabb(player_entity), get_aabb(e))) {
                    if (invulnerable_timer <= 0.0f) {
                        player_hurt_this_frame = true;
                    }
                }
            });
        }

        if (player_hurt_this_frame) {
            lives--;
            invulnerable_timer = k_invulnerable_time;
            if (player_rb) {
                int dir = (player_entity->transform()->position.x > k_world_w * 0.5f) ? -1 : 1;
                player_rb->velocity.x = static_cast<float>(dir) * k_hurt_knockback_x;
                player_rb->velocity.y = k_hurt_knockback_y;
            }
            if (sfx_hurt) sfx_hurt->play();
            if (hit_fx && hit_fx->owner()) {
                hit_fx->owner()->transform()->position = player_entity->transform()->position;
                hit_fx->burst();
            }
            if (lives <= 0) {
                lives = 0;
                game_over = true;
            }
        }

        for (auto* e : enemies_to_destroy) {
            world.scene()->destroy_entity(e);
        }

        // 玩家无敌闪烁：简单隐藏/显示精灵
        if (player_entity) {
            if (auto* sprite = player_entity->get_component<components::d2::sprite::Sprite2D>()) {
                sprite->enabled = (invulnerable_timer <= 0.0f) ||
                                  (static_cast<int>(invulnerable_timer * 10.0f) % 2 == 0);
            }
        }

        // UI 更新
        if (score_label) score_label->text = std::format("Score: {}", score);
        if (coins_label) coins_label->text = std::format("Coins: {}", coins);
        if (lives_label) lives_label->text = std::format("Lives: {}", lives);
        if (game_over_label) {
            if (game_over) {
                game_over_label->text = std::format("GAME OVER - Score: {} (R to restart)", score);
            } else {
                game_over_label->text = "";
            }
        }
        fps_update_timer += dt;
        if (fps_update_timer >= 0.5f) {
            float render_fps = (fps_update_timer > 0.0f) ? static_cast<float>(rendered_frames) / fps_update_timer : 0.0f;
            if (fps_label) fps_label->text = std::format("Render FPS: {:.1f}", render_fps);
            rendered_frames = 0;
            fps_update_timer = 0.0f;
        }

        // 渲染
        if (renderer2d) {
            renderer2d->begin_frame(static_cast<float>(w), static_cast<float>(h));
            renderer2d->set_ambient_light(render::Color(0.08f, 0.08f, 0.14f, 1.0f));
        }

        world.render(render_ctx);
        bool rendered = !render_sys || render_sys->rendered_last_frame();
        if (rendered) {
            rendered_frames++;
        }

        if (renderer2d) {
            renderer2d->end_frame();
        }

        imgui.begin_frame();
        ImGui::Begin("2D Platformer");
        ImGui::Text("A/D move, Space/W jump, Mouse aim/shoot, R restart");
        ImGui::Text("Score: %d  Coins: %d  Lives: %d", score, coins, lives);
        if (game_over) ImGui::TextColored(ImVec4(1, 0, 0, 1), "GAME OVER - Press R to restart");

        ImGui::Separator();
        if (player_pm) {
            ImGui::Text("Player material: %s", player_pm->preset_name.c_str());
            ImGui::Text("  Softness:     %.2f", player_pm->softness);
            ImGui::Text("  Drag coeff.:  %.2f", player_pm->drag_coefficient);
            ImGui::Text("  Density:      %.2f", player_pm->density);
            ImGui::Text("  Restitution:  %.2f", player_pm->restitution());
            ImGui::Text("  Eff. gravity: %.2f", player_pm->effective_gravity(current_gravity));
            ImGui::Separator();
        }

        int prev_gravity = gravity_index;
        auto gravity_name_getter = [](void* data, int idx, const char** out_text) -> bool {
            auto* presets = static_cast<const GravityPreset*>(data);
            *out_text = presets[idx].name;
            return true;
        };
        ImGui::Combo("Gravity", &gravity_index, gravity_name_getter,
                     const_cast<GravityPreset*>(k_gravity_presets), k_gravity_preset_count);
        if (gravity_index != prev_gravity) {
            current_gravity = k_gravity_presets[gravity_index].gravity;
            if (phys) {
                phys->gravity = math::Vector2f(0.0f, current_gravity);
            }
        }
        ImGui::Text("Planet: %s (g=%.0f)", k_gravity_presets[gravity_index].name, current_gravity);
        ImGui::End();

        if (rendered) {
            imgui.end_frame([&](ImDrawData* draw_data, std::shared_ptr<std::promise<void>> sync_promise) {
                auto owned = imgui.clone_draw_data(draw_data);
                render_ctx.push_command([owned, &imgui, sync_promise](render::IRenderBackend*) {
                    imgui.render_draw_data(owned.get());
                    sync_promise->set_value();
                });
            });
            render_ctx.present();
            if (!screenshot_path.empty()) {
                render_ctx.request_screenshot(screenshot_path);
                screenshot_path.clear(); // 只截一次
            }
        } else {
            // 画面未变化：不提交 ImGui 渲染命令，也不 present，保持 ImGui 帧状态正确
            imgui.end_frame([](ImDrawData*, std::shared_ptr<std::promise<void>> sync_promise) {
                sync_promise->set_value();
            });
        }
        frame_limiter.end_frame();
    }

    render_ctx.pause_render_thread_keep_cmdbuffer();
    world.shutdown();
    imgui.shutdown();
    if (renderer2d) {
        renderer2d->shutdown();
    }
    render_ctx.shutdown();
    window.destroy();
    platform::Window::shutdown_sdk();
    utils::glog_shutdown();

    std::cout << "2D Platformer demo exited." << std::endl;
    return 0;
}
