#include "GryceEngineUtils/audio.h"

#include "audio/audio_engine.h"

namespace GryceEngineUtils {

AudioEngine::AudioEngine() {
    gryce_engine::audio::AudioEngine::instance().init();
}

AudioEngine::~AudioEngine() {
    gryce_engine::audio::AudioEngine::instance().shutdown();
}

AudioEngine* AudioEngine::create() {
    return new AudioEngine();
}

void AudioEngine::destroy() {
    delete this;
}

void AudioEngine::set_listener_position(const math::Vector3f& pos) {
    gryce_engine::audio::AudioEngine::instance().set_listener_position(pos);
}

} // namespace GryceEngineUtils
