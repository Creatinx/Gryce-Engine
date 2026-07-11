#include "components/audio_source.h"

#include "audio/audio_engine.h"
#include "scene/entity.h"
#include "components/transform.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

void AudioSource::ensure_clip_loaded() {
    if (clip_path.empty()) return;
    if (clip_ && clip_->path() == clip_path) return;

    std::string resolved = resources::ResourcePath::resolve(clip_path);
    clip_ = std::make_shared<audio::AudioClip>();
    if (!clip_->load(resolved)) {
        clip_.reset();
    }
}

void AudioSource::sync_to_owner() {
    if (!owner() || !owner()->transform()) return;
    math::Vector3f pos = owner()->transform()->position;
    for (auto& inst : instances_) {
        if (inst && inst->valid()) {
            inst->set_position(pos);
        }
    }
}

void AudioSource::on_init() {
    ensure_clip_loaded();
    if (play_on_awake && !started_on_awake_) {
        started_on_awake_ = true;
        play();
    }
}

void AudioSource::on_update(float dt) {
    (void)dt;
    if (is_3d) {
        sync_to_owner();
    }

    // 清理已停止的一次性实例
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (*it && !(*it)->is_playing() && !loop) {
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSource::on_destroy() {
    stop();
}

void AudioSource::play() {
    ensure_clip_loaded();
    if (!clip_ || !clip_->valid()) {
        GLOG_WARN("AudioSource: cannot play, clip not loaded '{}'", clip_path);
        return;
    }

    auto inst = std::make_unique<audio::AudioInstance>();
    if (!inst->create_from_clip(*clip_)) {
        return;
    }

    inst->set_volume(volume);
    inst->set_pitch(pitch);
    inst->set_loop(loop);
    inst->set_3d(is_3d);
    inst->set_spatial_range(min_distance, max_distance);

    if (owner() && owner()->transform()) {
        inst->set_position(owner()->transform()->position);
    }

    inst->play();
    instances_.push_back(std::move(inst));
}

void AudioSource::stop() {
    instances_.clear();
}

bool AudioSource::is_playing() const {
    for (const auto& inst : instances_) {
        if (inst && inst->is_playing()) return true;
    }
    return false;
}

} // namespace gryce_engine::components
