#include "components/audio_source.h"

#include <algorithm>

#include "audio/audio_engine.h"
#include "assets/asset_manager.h"
#include "scene/entity.h"
#include "components/transform.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

// 实体世界坐标（取世界矩阵平移分量）。
// 3D 音源常作为子实体挂载（环境音），必须用世界坐标而非本地 position，
// 否则相对监听点的距离/方位错误，表现为音量忽大忽小、声像异常。
math::Vector3f world_position_of(const scene::Entity* entity) {
    if (!entity || !entity->transform()) return math::Vector3f::zero();
    const math::Matrix4f world = entity->world_transform();
    return math::Vector3f(world(0, 3), world(1, 3), world(2, 3));
}

} // namespace

void AudioSource::ensure_clip_loaded() {
    if (clip_path.empty()) return;
    if (clip_ && clip_->path() == clip_path) return;

    // 先查磁盘存在，不存在则从 gpkg 包提取到临时文件供读取
    std::string resolved = assets::AssetManager::instance().resolve_for_reading(clip_path);
    clip_ = std::make_shared<audio::AudioClip>();
    if (!clip_->load(resolved)) {
        clip_.reset();
    }
}

void AudioSource::sync_to_owner() {
    if (!owner() || !owner()->transform()) return;
    const math::Vector3f pos = world_position_of(owner());
    for (auto& inst : instances_) {
        if (inst && inst->valid()) {
            inst->set_position(pos);
        }
    }
}

void AudioSource::on_awake() {
    if (play_on_awake && !started_on_awake_) {
        started_on_awake_ = true;
        play();
    }
}

void AudioSource::on_init() {
    // 不再预加载 clip：避免编辑器中未播放的音频长期占用文件句柄，导致文件无法删除/重命名。
}

void AudioSource::on_start() {
    // 场景正式开始时可选触发（如延迟播放）
}

void AudioSource::on_enable() {
    // 组件被启用时可恢复播放（保留扩展点）
}

void AudioSource::on_disable() {
    stop();
}

void AudioSource::on_update(float dt) {
    (void)dt;
    if (is_3d) {
        sync_to_owner();
    }

    // 检测 is_3d 切换，热重载/Inspector 修改后立即重新应用空间化设置。
    const bool is_3d_changed = (is_3d != last_is_3d_);
    if (is_3d_changed) {
        GLOG_INFO("AudioSource: is_3d changed to {} for '{}'", is_3d, clip_path);
        last_is_3d_ = is_3d;
    }

    // 实时同步 Inspector/脚本修改的音量、音高、速度、循环、3D 属性到正在播放的实例
    for (auto& inst : instances_) {
        if (inst && inst->valid()) {
            inst->set_volume(volume);
            inst->set_pitch(pitch);
            inst->set_speed(speed);
            inst->set_loop(loop);
            if (is_3d_changed) {
                inst->set_3d(is_3d);
                inst->set_spatial_range(std::max(min_distance, 0.01f),
                                        std::max(max_distance, 0.02f));
            } else {
                inst->set_spatial_range(min_distance, max_distance);
            }
        }
    }

    // 清理已停止的一次性实例
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (*it && !(*it)->is_playing() && !loop) {
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }

    // 没有正在播放的实例时释放 clip，解除对音频文件的占用。
    if (instances_.empty()) {
        clip_.reset();
    }
}

void AudioSource::on_destroy() {
    stop();
}

void AudioSource::play() {
    ensure_clip_loaded();
    GLOG_INFO("AudioSource: play '{}' clip={} valid={} streaming={}", clip_path,
              static_cast<bool>(clip_), clip_ ? clip_->valid() : false,
              clip_ ? clip_->is_streaming() : false);
    if (!clip_ || !clip_->valid()) {
        GLOG_WARN("AudioSource: cannot play, clip not loaded '{}'", clip_path);
        return;
    }

    // 先停止已有实例，避免多次 play 叠加（相位/重音）。
    // 这里只清理实例而不调用 stop()，保留 clip_ 供新实例使用。
    instances_.clear();

    auto inst = std::make_unique<audio::AudioInstance>();
    if (!inst->create_from_clip(clip_, speed)) {
        return;
    }

    inst->set_volume(volume);
    inst->set_pitch(pitch);
    inst->set_speed(speed);
    inst->set_loop(loop);
    inst->set_3d(is_3d);
    // min_distance 为 0 时逆平方衰减在零距离处增益爆炸（削波），钳到安全下限
    inst->set_spatial_range(std::max(min_distance, 0.01f),
                            std::max(max_distance, 0.02f));

    if (owner() && owner()->transform()) {
        inst->set_position(world_position_of(owner()));
    }

    inst->play();
    instances_.push_back(std::move(inst));
    last_is_3d_ = is_3d;
}

void AudioSource::stop() {
    instances_.clear();
    clip_.reset();
}

bool AudioSource::is_playing() const {
    for (const auto& inst : instances_) {
        if (inst && inst->is_playing()) return true;
    }
    return false;
}

} // namespace gryce_engine::components
