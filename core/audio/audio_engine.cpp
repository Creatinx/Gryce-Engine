#include "audio/audio_engine.h"

#include "miniaudio.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::audio {

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------
void AudioEngine::EngineDeleter::operator()(ma_engine* e) const {
    if (e) {
        ma_engine_uninit(e);
        delete e;
    }
}

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
    if (engine_) return true;
    engine_.reset(new ma_engine{});
    ma_result result = ma_engine_init(nullptr, engine_.get());
    if (result != MA_SUCCESS) {
        GLOG_ERROR("AudioEngine: failed to initialize miniaudio engine (result={})", static_cast<int>(result));
        engine_.reset();
        return false;
    }
    GLOG_INFO("AudioEngine: initialized successfully");
    return true;
}

void AudioEngine::shutdown() {
    engine_.reset();
}

bool AudioEngine::valid() const {
    return engine_ != nullptr;
}

void AudioEngine::set_listener_position(const math::Vector3f& pos) {
    if (!engine_) return;
    ma_engine_listener_set_position(engine_.get(), 0, pos.x, pos.y, pos.z);
}

math::Vector3f AudioEngine::listener_position() const {
    if (!engine_) return math::Vector3f::zero();
    ma_vec3f v = ma_engine_listener_get_position(engine_.get(), 0);
    return math::Vector3f(v.x, v.y, v.z);
}

AudioEngine& AudioEngine::instance() {
    static AudioEngine engine;
    return engine;
}

// ---------------------------------------------------------------------------
// AudioClip
// ---------------------------------------------------------------------------
void AudioClip::SoundDeleter::operator()(ma_sound* s) const {
    if (s) {
        ma_sound_uninit(s);
        delete s;
    }
}

AudioClip::AudioClip() = default;
AudioClip::~AudioClip() = default;

bool AudioClip::load(const std::string& path) {
    if (path.empty()) return false;
    AudioEngine& engine = AudioEngine::instance();
    if (!engine.valid()) {
        if (!engine.init()) return false;
    }

    sound_.reset(new ma_sound{});
    ma_result result = ma_sound_init_from_file(engine.engine(), path.c_str(),
                                               MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC,
                                               nullptr, nullptr, sound_.get());
    if (result != MA_SUCCESS) {
        GLOG_WARN("AudioClip: failed to load '{}' (result={})", path, static_cast<int>(result));
        sound_.reset();
        return false;
    }
    path_ = path;
    GLOG_INFO("AudioClip: loaded '{}'", path);
    return true;
}

// ---------------------------------------------------------------------------
// AudioInstance
// ---------------------------------------------------------------------------
void AudioInstance::SoundDeleter::operator()(ma_sound* s) const {
    if (s) {
        ma_sound_uninit(s);
        delete s;
    }
}

AudioInstance::AudioInstance() = default;
AudioInstance::~AudioInstance() = default;

bool AudioInstance::create_from_clip(const AudioClip& clip) {
    if (!clip.valid()) return false;
    AudioEngine& engine = AudioEngine::instance();
    if (!engine.valid()) return false;

    sound_.reset(new ma_sound{});
    ma_result result = ma_sound_init_copy(engine.engine(), clip.handle(),
                                          MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC,
                                          nullptr, sound_.get());
    if (result != MA_SUCCESS) {
        GLOG_WARN("AudioInstance: failed to create instance (result={})", static_cast<int>(result));
        sound_.reset();
        return false;
    }
    return true;
}

void AudioInstance::play() {
    if (sound_) ma_sound_start(sound_.get());
}

void AudioInstance::stop() {
    if (sound_) ma_sound_stop(sound_.get());
}

void AudioInstance::set_volume(float volume) {
    if (sound_) ma_sound_set_volume(sound_.get(), volume);
}

void AudioInstance::set_pitch(float pitch) {
    if (sound_) ma_sound_set_pitch(sound_.get(), pitch);
}

void AudioInstance::set_loop(bool loop) {
    if (sound_) ma_sound_set_looping(sound_.get(), loop ? MA_TRUE : MA_FALSE);
}

void AudioInstance::set_3d(bool enabled) {
    if (!sound_) return;
    if (enabled) {
        ma_sound_set_spatialization_enabled(sound_.get(), MA_TRUE);
    } else {
        ma_sound_set_spatialization_enabled(sound_.get(), MA_FALSE);
    }
}

void AudioInstance::set_position(const math::Vector3f& pos) {
    if (sound_) ma_sound_set_position(sound_.get(), pos.x, pos.y, pos.z);
}

void AudioInstance::set_spatial_range(float min_dist, float max_dist) {
    if (sound_) {
        ma_sound_set_min_distance(sound_.get(), min_dist);
        ma_sound_set_max_distance(sound_.get(), max_dist);
    }
}

bool AudioInstance::is_playing() const {
    return sound_ && ma_sound_is_playing(sound_.get()) == MA_TRUE;
}

} // namespace gryce_engine::audio
