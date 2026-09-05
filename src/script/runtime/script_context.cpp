#include "script_context.h"

namespace GryceEngineUtils::script {

// ============================================================================
// 单例
// ============================================================================

ScriptContext& ScriptContext::instance() {
    static ScriptContext ctx;
    return ctx;
}

// ============================================================================
// 当前实体/场景上下文
// ============================================================================

void ScriptContext::set_current_entity(int handle) {
    current_entity_ = handle;
}

int ScriptContext::current_entity() const {
    return current_entity_;
}

void ScriptContext::set_current_scene(void* scene) {
    current_scene_ = scene;
}

void* ScriptContext::current_scene() const {
    return current_scene_;
}

void ScriptContext::set_ui_manager(void* ui_mgr) {
    ui_manager_ = ui_mgr;
}

void* ScriptContext::ui_manager() const {
    return ui_manager_;
}

// ============================================================================
// 跨场景状态
// ============================================================================

void ScriptContext::set_state(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_[key] = value;
}

std::string ScriptContext::get_state(const std::string& key) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = state_.find(key);
    if (it != state_.end()) {
        return it->second;
    }
    return "";
}

bool ScriptContext::has_state(const std::string& key) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_.find(key) != state_.end();
}

// ============================================================================
// 输入缓存
// ============================================================================

void ScriptContext::set_key_state(int key, bool down) {
    if (key < 0 || key >= 512) return;
    std::lock_guard<std::mutex> lock(input_mutex_);
    key_states_[key] = down;
}

bool ScriptContext::is_key_down(int key) const {
    if (key < 0 || key >= 512) return false;
    std::lock_guard<std::mutex> lock(input_mutex_);
    return key_states_[key];
}

void ScriptContext::set_mouse_pos(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
}

void ScriptContext::get_mouse_pos(float* out_x, float* out_y) const {
    if (out_x) *out_x = mouse_x_;
    if (out_y) *out_y = mouse_y_;
}

void ScriptContext::set_mouse_button(int btn, bool down) {
    if (btn < 0 || btn >= 8) return;
    mouse_buttons_[btn] = down;
}

bool ScriptContext::is_mouse_down(int btn) const {
    if (btn < 0 || btn >= 8) return false;
    return mouse_buttons_[btn];
}

// ============================================================================
// 时间
// ============================================================================

void ScriptContext::set_delta_time(float dt) {
    delta_time_ = dt;
}

float ScriptContext::delta_time() const {
    return delta_time_;
}

void ScriptContext::set_elapsed_time(float t) {
    elapsed_time_ = t;
}

float ScriptContext::elapsed_time() const {
    return elapsed_time_;
}

// ============================================================================
// 重置
// ============================================================================

void ScriptContext::reset_frame() {
    // 每帧清除按键状态（按键只保留一帧）
    // 鼠标位置和按钮状态保留
    delta_time_ = 0.0f;
}

} // namespace GryceEngineUtils::script