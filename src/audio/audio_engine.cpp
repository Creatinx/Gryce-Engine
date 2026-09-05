#include "audio/audio_engine.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "miniaudio.h"
#include "audio/time_stretcher.h"
#include "utils/glog/glog_lib.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace gryce_engine::audio {

namespace {

#ifdef _WIN32
// miniaudio 的默认 VFS 用 CreateFileA 打开文件，窄路径按系统代码页（GBK）解释，
// UTF-8 编码的中文/特殊字符文件名会打不开（result=-7）。
// 显式按 UTF-8 转宽字符，改走 ma_sound_init_from_file_w。
std::wstring utf8_to_wide(const std::string& utf8) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring wide(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    return wide;
}
#endif

} // namespace

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

    // 短音效整体解码（低延迟、可被 ma_sound_init_copy 复制出多实例）；
    // 长音频（>4MB）用流式播放，避免阻塞与内存暴涨。
    // 注意：流式源不支持 ma_sound_init_copy，因此 AudioInstance 会直接从文件创建。
    constexpr std::uintmax_t k_stream_threshold = 4 * 1024 * 1024; // 4MB
    std::uintmax_t file_size = 0;

#ifdef _WIN32
    const std::wstring wide_path = utf8_to_wide(path);
    {
        std::error_code ec;
        const std::filesystem::path fs_path =
            wide_path.empty() ? std::filesystem::path(path) : std::filesystem::path(wide_path);
        file_size = std::filesystem::file_size(fs_path, ec);
        if (ec) file_size = 0;
    }
#else
    {
        std::error_code ec;
        file_size = std::filesystem::file_size(std::filesystem::path(path), ec);
        if (ec) file_size = 0;
    }
#endif

    is_streaming_ = file_size > k_stream_threshold;

    if (is_streaming_) {
        // 流式 clip 不预打开文件：只在播放时由 AudioInstance 创建流式 sound。
        // 这样停止播放后文件立即解锁，编辑器可正常删除/重命名。
        path_ = path;
        GLOG_INFO("AudioClip: loaded '{}' (stream, deferred)", path);
        return true;
    }

    sound_.reset(new ma_sound{});
    ma_result result;
#ifdef _WIN32
    result = wide_path.empty()
                 ? ma_sound_init_from_file(engine.engine(), path.c_str(),
                                           MA_SOUND_FLAG_DECODE, nullptr, nullptr, sound_.get())
                 : ma_sound_init_from_file_w(engine.engine(), wide_path.c_str(),
                                             MA_SOUND_FLAG_DECODE, nullptr, nullptr, sound_.get());
#else
    result = ma_sound_init_from_file(engine.engine(), path.c_str(),
                                     MA_SOUND_FLAG_DECODE, nullptr, nullptr, sound_.get());
#endif
    if (result != MA_SUCCESS) {
        GLOG_WARN("AudioClip: failed to load '{}' (result={})", path, static_cast<int>(result));
        sound_.reset();
        return false;
    }
    path_ = path;
    GLOG_INFO("AudioClip: loaded '{}' (decode)", path);
    return true;
}

bool AudioClip::decode_to_pcm(std::vector<float>& out_pcm, uint32_t& out_sample_rate,
                              uint32_t& out_channels) const {
    if (path_.empty() || is_streaming_) return false;

    // 保持文件原始声道数与采样率
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder{};
    ma_result result;

#ifdef _WIN32
    const std::wstring wide_path = utf8_to_wide(path_);
    result = wide_path.empty()
                 ? ma_decoder_init_file(path_.c_str(), &config, &decoder)
                 : ma_decoder_init_file_w(wide_path.c_str(), &config, &decoder);
#else
    result = ma_decoder_init_file(path_.c_str(), &config, &decoder);
#endif
    if (result != MA_SUCCESS) {
        GLOG_WARN("AudioClip::decode_to_pcm: failed to init decoder for '{}' (result={})",
                  path_, static_cast<int>(result));
        return false;
    }

    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);
    if (total_frames == 0) {
        ma_decoder_uninit(&decoder);
        return false;
    }

    out_channels = decoder.outputChannels;
    out_sample_rate = decoder.outputSampleRate;
    out_pcm.resize(static_cast<size_t>(total_frames * out_channels));

    ma_uint64 frames_read = 0;
    result = ma_decoder_read_pcm_frames(&decoder, out_pcm.data(), total_frames, &frames_read);
    ma_decoder_uninit(&decoder);

    if (result != MA_SUCCESS || frames_read == 0) {
        out_pcm.clear();
        out_sample_rate = 0;
        out_channels = 0;
        return false;
    }

    out_pcm.resize(static_cast<size_t>(frames_read * out_channels));
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

void AudioInstance::AudioBufferDeleter::operator()(void* b) const {
    if (b) {
        auto* buffer = static_cast<ma_audio_buffer*>(b);
        ma_audio_buffer_uninit(buffer);
        delete buffer;
    }
}

AudioInstance::AudioInstance() = default;
AudioInstance::~AudioInstance() = default;

bool AudioInstance::create_from_clip(std::shared_ptr<const AudioClip> clip, float speed) {
    if (!clip || !clip->valid()) return false;
    AudioEngine& engine = AudioEngine::instance();
    if (!engine.valid()) return false;

    // 先保存源，再清理旧资源
    source_clip_ = std::move(clip);
    speed_ = speed;
    sample_rate_ = 0;
    channels_ = 0;

    sound_.reset();
    audio_buffer_.reset();
    stretched_pcm_.clear();

    return recreate_from_source(speed_);
}

bool AudioInstance::recreate_from_source(float speed) {
    if (!source_clip_ || !source_clip_->valid()) return false;
    AudioEngine& engine = AudioEngine::instance();
    if (!engine.valid()) return false;

    sound_.reset();
    audio_buffer_.reset();
    stretched_pcm_.clear();

    const bool need_time_stretch = !source_clip_->is_streaming() && std::abs(speed - 1.0f) >= 0.005f;
    if (need_time_stretch) {
        std::vector<float> pcm;
        if (!source_clip_->decode_to_pcm(pcm, sample_rate_, channels_)) {
            GLOG_WARN("AudioInstance: decode_to_pcm failed for '{}', falling back to normal playback",
                      source_clip_->path());
        } else if (!TimeStretcher::process(pcm.data(), static_cast<int64_t>(pcm.size() / channels_),
                                           static_cast<int>(channels_), speed, stretched_pcm_)) {
            GLOG_WARN("AudioInstance: time stretch failed for '{}', falling back to normal playback",
                      source_clip_->path());
            stretched_pcm_.clear();
        }
    }

    ma_result result;
    sound_.reset(new ma_sound{});

    if (!stretched_pcm_.empty() && channels_ > 0 && sample_rate_ > 0) {
        // 从拉伸后的内存 PCM 创建 sound
        auto* buffer = new ma_audio_buffer{};
        ma_audio_buffer_config config = ma_audio_buffer_config_init(
            ma_format_f32,
            static_cast<ma_uint32>(channels_),
            static_cast<ma_uint64>(stretched_pcm_.size() / channels_),
            stretched_pcm_.data(),
            nullptr);
        config.sampleRate = sample_rate_;

        result = ma_audio_buffer_init(&config, buffer);
        if (result != MA_SUCCESS) {
            GLOG_WARN("AudioInstance: failed to init audio buffer (result={})", static_cast<int>(result));
            delete buffer;
            sound_.reset();
            audio_buffer_.reset();
            stretched_pcm_.clear();
            return false;
        }
        audio_buffer_.reset(buffer);

        result = ma_sound_init_from_data_source(engine.engine(), buffer,
                                                0, nullptr, sound_.get());
        if (result != MA_SUCCESS) {
            GLOG_WARN("AudioInstance: failed to init sound from data source (result={})",
                      static_cast<int>(result));
            sound_.reset();
            audio_buffer_.reset();
            stretched_pcm_.clear();
            return false;
        }
    } else if (source_clip_->is_streaming()) {
        // 流式源不支持 ma_sound_init_copy，直接从文件创建独立流式实例。
#ifdef _WIN32
        const std::wstring wide_path = utf8_to_wide(source_clip_->path());
        result = wide_path.empty()
                     ? ma_sound_init_from_file(engine.engine(), source_clip_->path().c_str(),
                                               MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound_.get())
                     : ma_sound_init_from_file_w(engine.engine(), wide_path.c_str(),
                                                 MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound_.get());
#else
        result = ma_sound_init_from_file(engine.engine(), source_clip_->path().c_str(),
                                         MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound_.get());
#endif
        if (result != MA_SUCCESS) {
            GLOG_WARN("AudioInstance: failed to create stream instance (result={})",
                      static_cast<int>(result));
            sound_.reset();
            return false;
        }
    } else {
        // flags 传 0：拷贝继承源 sound 的加载方式（解码源共享已解码数据）。
        result = ma_sound_init_copy(engine.engine(), source_clip_->handle(),
                                    0, nullptr, sound_.get());
        if (result != MA_SUCCESS) {
            GLOG_WARN("AudioInstance: failed to create copy instance (result={})",
                      static_cast<int>(result));
            sound_.reset();
            return false;
        }
    }

    GLOG_INFO("AudioInstance: created {} speed={} sound={} engine={}",
              source_clip_->is_streaming() ? "stream" : (stretched_pcm_.empty() ? "decode" : "time-stretch"),
              speed_,
              reinterpret_cast<void*>(sound_.get()),
              reinterpret_cast<void*>(ma_sound_get_engine(sound_.get())));
    return true;
}

void AudioInstance::set_speed(float speed) {
    if (!source_clip_ || source_clip_->is_streaming()) {
        // 流式音频回退到 pitch 控制（会改变音调，符合预期）
        set_pitch(speed);
        return;
    }
    if (std::abs(speed_ - speed) < 0.005f) return;

    const bool was_playing = is_playing();
    const float pos_sec = (sound_ && ma_sound_get_engine(sound_.get()))
                              ? static_cast<float>(ma_sound_get_time_in_milliseconds(sound_.get())) / 1000.0f
                              : 0.0f;

    speed_ = speed;
    if (!recreate_from_source(speed_)) {
        GLOG_WARN("AudioInstance: set_speed failed to recreate sound");
        return;
    }

    // 恢复播放状态与大致位置
    if (was_playing) {
        ma_sound_seek_to_pcm_frame(sound_.get(),
                                   static_cast<ma_uint64>(pos_sec * static_cast<float>(sample_rate_)));
        play();
    }
}

void AudioInstance::play() {
    if (!sound_ || !ma_sound_get_engine(sound_.get())) return;
    ma_sound_start(sound_.get());
}

void AudioInstance::stop() {
    if (!sound_ || !ma_sound_get_engine(sound_.get())) return;
    ma_sound_stop(sound_.get());
}

void AudioInstance::set_volume(float volume) {
    if (!sound_ || !ma_sound_get_engine(sound_.get())) return;
    ma_sound_set_volume(sound_.get(), volume);
}

void AudioInstance::set_pitch(float pitch) {
    if (!sound_ || !ma_sound_get_engine(sound_.get())) {
        GLOG_WARN("AudioInstance: set_pitch skipped, sound={} engine={}",
                  reinterpret_cast<void*>(sound_.get()),
                  reinterpret_cast<void*>(sound_ ? ma_sound_get_engine(sound_.get()) : nullptr));
        return;
    }
    ma_sound_set_pitch(sound_.get(), pitch);
}

void AudioInstance::set_loop(bool loop) {
    if (!sound_ || !ma_sound_get_engine(sound_.get())) return;
    ma_sound_set_looping(sound_.get(), loop ? MA_TRUE : MA_FALSE);
}

void AudioInstance::set_3d(bool enabled) {
    if (!sound_ || !ma_sound_get_engine(sound_.get())) return;
    if (enabled) {
        ma_sound_set_spatialization_enabled(sound_.get(), MA_TRUE);
    } else {
        ma_sound_set_spatialization_enabled(sound_.get(), MA_FALSE);
    }
}

void AudioInstance::set_position(const math::Vector3f& pos) {
    if (!sound_ || !ma_sound_get_engine(sound_.get())) return;
    ma_sound_set_position(sound_.get(), pos.x, pos.y, pos.z);
}

void AudioInstance::set_spatial_range(float min_dist, float max_dist) {
    if (!sound_ || !ma_sound_get_engine(sound_.get())) return;
    ma_sound_set_min_distance(sound_.get(), min_dist);
    ma_sound_set_max_distance(sound_.get(), max_dist);
}

bool AudioInstance::is_playing() const {
    return sound_ && ma_sound_get_engine(sound_.get()) &&
           ma_sound_is_playing(sound_.get()) == MA_TRUE;
}

} // namespace gryce_engine::audio
