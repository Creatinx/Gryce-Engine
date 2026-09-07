#pragma once

// ScriptContext — 脚本运行时上下文
//
// 管理当前实体、场景、UIManager 引用等运行时状态，
// 供 engine.* 绑定在回调时使用。
//
// 用法：
//   ScriptContext::instance().set_current_entity(handle);
//   ScriptContext::instance().current_entity();

#include <string>
#include <unordered_map>
#include <mutex>

#include <quickjs/quickjs.h>

#include "export.h"

namespace GryceEngineUtils::script {

class GRYCE_API ScriptContext {
public:
    static ScriptContext& instance();

    // ---- 当前实体/场景上下文 ----
    void set_current_entity(int handle);
    int current_entity() const;

    void set_current_scene(void* scene);
    void* current_scene() const;

    void set_ui_manager(void* ui_mgr);
    void* ui_manager() const;

    // ---- 跨场景状态 ----
    // 存储 JS 值的字符串表示（简单类型：数字、布尔、字符串）
    void set_state(const std::string& key, const std::string& value);
    std::string get_state(const std::string& key) const;
    bool has_state(const std::string& key) const;

    // ---- 输入缓存 ----
    void set_key_state(int key, bool down);
    bool is_key_down(int key) const;

    // 以"当前按下"的按键集合整体刷新状态；不在集合中的键视为抬起。
    // 接受任意可迭代的整型容器（如核心 InputState::keys_down 的 unordered_set）。
    template <class KeySet>
    void set_held_keys(const KeySet& keys) {
        std::lock_guard<std::mutex> lock(input_mutex_);
        for (int i = 0; i < 512; ++i) key_states_[i] = false;
        for (int k : keys) {
            if (k >= 0 && k < 512) key_states_[k] = true;
        }
    }

    void set_mouse_pos(float x, float y);
    void get_mouse_pos(float* out_x, float* out_y) const;

    void set_mouse_button(int btn, bool down);
    bool is_mouse_down(int btn) const;

    // ---- 时间 ----
    void set_delta_time(float dt);
    float delta_time() const;

    void set_elapsed_time(float t);
    float elapsed_time() const;

    // ---- 重置（每帧开始前调用） ----
    void reset_frame();

private:
    ScriptContext() = default;

    int current_entity_ = 0;
    void* current_scene_ = nullptr;
    void* ui_manager_ = nullptr;

    std::unordered_map<std::string, std::string> state_;
    mutable std::mutex state_mutex_;

    // 输入状态
    bool key_states_[512] = {false};
    mutable std::mutex input_mutex_;

    float mouse_x_ = 0.0f;
    float mouse_y_ = 0.0f;
    bool mouse_buttons_[8] = {false};

    // 时间
    float delta_time_ = 0.0f;
    float elapsed_time_ = 0.0f;
};

} // namespace GryceEngineUtils::script