#pragma once

#include "export.h"
#include "ecs/system.h"
#include "math/math.h"

#include <memory>
#include <string>
#include <vector>

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// AudioSystem — 音频系统（Amplitude Audio SDK）
//
// 生命周期：
//   on_init     -> 从 config_path 初始化 Amplitude 引擎；失败则进入静音模式
//   on_update   -> AdvanceFrame(dt)；同步监听者/发射器位姿；处理播放请求
//   on_shutdown -> Deinitialize
//
// 静音模式：未提供配置文件或初始化失败时，系统仍正常运行，所有播放调用
// 变为安全空操作并只记录一次警告。这样即使项目尚未制作音频资源，引擎也
// 不会崩溃，接入音效库后自动生效。
//
// 配置：
//   config_path   Amplitude 引擎配置（flatbuffer 二进制，.amconfig）
//   sound_banks   启动时加载的音效库（.ambank），可多个
//   master_gain   总线主音量（0~1）
// ---------------------------------------------------------------------------
class GRYCE_API AudioSystem : public ISystem {
public:
    AudioSystem();
    ~AudioSystem() override;

    const char* name() const override { return "AudioSystem"; }
    // 音频在逻辑更新前推进一帧，保证本帧触发的播放立即生效
    Phase phase() const override { return Phase::PreUpdate; }
    int priority() const override { return -100; }

    void on_init(scene::Scene& scene) override;
    void on_shutdown(scene::Scene& scene) override;
    void on_update(scene::Scene& scene, float dt) override;

    // 引擎是否成功初始化（false = 静音模式）
    bool is_initialized() const;

    // 加载一个音效库；未初始化时返回 false
    bool load_sound_bank(const std::string& path);

    // 在世界坐标处播放事件；返回是否有活跃实例
    bool play_event(const std::string& event_name, const math::Vector3f& position);

    // 停止某个事件名对应的所有实例
    void stop_event(const std::string& event_name);

    void set_master_gain(float gain);

    // 配置（可在 on_init 之前修改）
    std::string config_path = "res:/audio/engine_config.amconfig";
    std::vector<std::string> sound_banks;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gryce_engine::ecs
