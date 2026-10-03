#include "ecs/systems/audio_system.h"

#include <SparkyStudios/Audio/Amplitude/Amplitude.h>

#include "components/audio_3d.h"
#include "components/transform.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "scene/entity.h"
#include "scene/query.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace gryce_engine::ecs {

namespace am = SparkyStudios::Audio::Amplitude;

namespace {

// 保留给"世界坐标播放"的 Amplitude 实体 ID（0 是无效句柄，故从 1 开始）。
constexpr am::AmEntityID k_world_entity_id = 1;
// 监听者/发射器的 Amplitude ID 由引擎实体 ID 派生，避免与保留 ID 冲突。
constexpr am::AmEntityID k_id_offset = 2;

am::AmEntityID to_am_id(EntityID id) {
    return static_cast<am::AmEntityID>(id) + k_id_offset;
}

inline hmm_vec3 to_am(const math::Vector3f& v) {
    hmm_vec3 r;
    r.X = v.x;
    r.Y = v.y;
    r.Z = v.z;
    return r;
}

std::string resolve_path(const std::string& path) {
    if (resources::ResourcePath::is_resource_path(path)) {
        return resources::ResourcePath::resolve(path);
    }
    return path;
}

} // namespace

struct AudioSystem::Impl {
    bool initialized = false;
    bool silent_warned = false;
    float master_gain = 1.0f;

    // 每个发射器对应的活跃事件实例，用于停止/取消
    std::unordered_map<EntityID, am::EventCanceler> active_events;
    // 已注册到 Amplitude 的监听者/发射器 ID
    std::unordered_map<EntityID, am::AmEntityID> emitter_ids;
    std::unordered_map<EntityID, am::AmListenerID> listener_ids;

    am::Entity world_entity; // 世界坐标播放用的临时实体

    am::Engine* engine() const { return am::Engine::GetInstance(); }
};

AudioSystem::AudioSystem() : impl_(std::make_unique<Impl>()) {}
AudioSystem::~AudioSystem() = default;

bool AudioSystem::is_initialized() const { return impl_->initialized; }

void AudioSystem::on_init(scene::Scene& scene) {
    (void)scene;
    am::Engine* engine = impl_->engine();
    if (!engine) {
        GLOG_WARN("AudioSystem: Amplitude engine unavailable — audio disabled");
        return;
    }

    const std::string resolved = resolve_path(config_path);
    std::error_code ec;
    if (resolved.empty() || !std::filesystem::exists(resolved, ec)) {
        GLOG_WARN("AudioSystem: engine config not found ('{}') — running in silent mode", resolved);
        return;
    }

    const std::wstring wpath = am_string_widen(resolved);
    if (!engine->Initialize(wpath.c_str())) {
        GLOG_WARN("AudioSystem: Amplitude Initialize failed — running in silent mode");
        return;
    }

    impl_->initialized = true;
    engine->SetMasterGain(impl_->master_gain);

    // 世界坐标播放用的常驻实体
    impl_->world_entity = engine->AddEntity(k_world_entity_id);

    for (const std::string& bank : sound_banks) {
        load_sound_bank(bank);
    }

    GLOG_INFO("AudioSystem: Amplitude initialized (config='{}')", resolved);
}

void AudioSystem::on_shutdown(scene::Scene& scene) {
    (void)scene;
    if (!impl_->initialized) return;

    am::Engine* engine = impl_->engine();
    impl_->active_events.clear();
    impl_->emitter_ids.clear();
    impl_->listener_ids.clear();

    if (engine) {
        for (const auto& [entity, id] : impl_->emitter_ids) {
            engine->RemoveEntity(id);
        }
        engine->Deinitialize();
    }
    impl_->initialized = false;
}

bool AudioSystem::load_sound_bank(const std::string& path) {
    if (!impl_->initialized) return false;
    am::Engine* engine = impl_->engine();
    if (!engine) return false;

    const std::string resolved = resolve_path(path);
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec)) {
        GLOG_WARN("AudioSystem: sound bank not found '{}'", resolved);
        return false;
    }
    const std::wstring wpath = am_string_widen(resolved);
    if (!engine->LoadSoundBank(wpath.c_str())) {
        GLOG_WARN("AudioSystem: failed to load sound bank '{}'", resolved);
        return false;
    }
    return true;
}

void AudioSystem::set_master_gain(float gain) {
    impl_->master_gain = gain;
    if (impl_->initialized) {
        if (am::Engine* engine = impl_->engine()) {
            engine->SetMasterGain(gain);
        }
    }
}

bool AudioSystem::play_event(const std::string& event_name, const math::Vector3f& position) {
    if (!impl_->initialized || event_name.empty()) return false;
    am::Engine* engine = impl_->engine();
    if (!engine) return false;

    if (!impl_->world_entity.Valid()) {
        impl_->world_entity = engine->AddEntity(k_world_entity_id);
    }
    impl_->world_entity.SetLocation(to_am(position));
    const am::EventCanceler canceler = engine->Trigger(event_name, impl_->world_entity);
    return canceler.Valid();
}

void AudioSystem::stop_event(const std::string& event_name) {
    (void)event_name;
    if (!impl_->initialized) return;
    for (auto& [entity, canceler] : impl_->active_events) {
        if (canceler.Valid()) canceler.Cancel();
    }
    impl_->active_events.clear();
}

void AudioSystem::on_update(scene::Scene& scene, float dt) {
    if (!impl_->initialized) return;
    am::Engine* engine = impl_->engine();
    if (!engine) return;

    // --- 1. 监听者同步 ----------------------------------------------------
    foreach_with_component<components::AudioListener>(
        scene, [&](scene::Entity* e, components::AudioListener* listener) {
            const am::AmListenerID id = to_am_id(e->id());
            am::Listener handle = engine->GetListener(id);
            if (!handle.Valid()) {
                handle = engine->AddListener(id);
                impl_->listener_ids[e->id()] = id;
            }
            if (!handle.Valid()) return;

            const auto* tr = e->transform();
            if (tr) {
                handle.SetLocation(to_am(tr->position));
                // 朝向取实体本地 -Z（引擎前向），上方向取 +Y
                const math::Vector3f dir = tr->rotation.rotate_vector(math::Vector3f(0.0f, 0.0f, -1.0f));
                handle.SetOrientation(to_am(dir), to_am(math::Vector3f(0.0f, 1.0f, 0.0f)));
            }
            if (listener->is_default) {
                engine->SetDefaultListener(id);
            }
        });

    // --- 2. 发射器同步 ----------------------------------------------------
    auto emitter_pool = scene.component_store().pool<components::AudioEmitter>();
    for (components::AudioEmitter* emitter : emitter_pool) {
        if (!emitter || !emitter->enabled) continue;
        scene::Entity* e = emitter->owner();
        if (!e || !e->enabled) continue;

        const am::AmEntityID id = to_am_id(e->id());
        am::Entity handle = engine->GetEntity(id);
        if (!handle.Valid()) {
            handle = engine->AddEntity(id);
            impl_->emitter_ids[e->id()] = id;
        }
        if (!handle.Valid()) continue;
        emitter->runtime_id = id;

        if (const auto* tr = e->transform()) {
            handle.SetLocation(to_am(tr->position));
            const math::Vector3f dir = tr->rotation.rotate_vector(math::Vector3f(0.0f, 0.0f, -1.0f));
            handle.SetOrientation(to_am(dir), to_am(math::Vector3f(0.0f, 1.0f, 0.0f)));
        }

        // 首次播放：play_on_start 且尚未触发
        if (emitter->play_on_start && !emitter->start_requested && !emitter->event_name.empty()) {
            emitter->start_requested = true;
            const am::EventCanceler canceler = engine->Trigger(emitter->event_name, handle);
            if (canceler.Valid()) {
                impl_->active_events[e->id()] = canceler;
                emitter->playing = true;
            } else {
                if (!impl_->silent_warned) {
                    impl_->silent_warned = true;
                    GLOG_WARN("AudioSystem: event '{}' not found in loaded sound banks", emitter->event_name);
                }
                emitter->playing = false;
            }
        }
    }

    // --- 3. 清理失效发射器的活跃事件 --------------------------------------
    for (auto it = impl_->active_events.begin(); it != impl_->active_events.end();) {
        bool alive = false;
        for (components::AudioEmitter* emitter : emitter_pool) {
            if (emitter && emitter->owner() && emitter->owner()->id() == it->first) {
                alive = true;
                break;
            }
        }
        if (!alive) {
            if (it->second.Valid()) it->second.Cancel();
            it = impl_->active_events.erase(it);
        } else {
            ++it;
        }
    }

    // --- 4. 推进音频时钟 --------------------------------------------------
    engine->AdvanceFrame(static_cast<am::AmTime>(dt));
}

} // namespace gryce_engine::ecs
