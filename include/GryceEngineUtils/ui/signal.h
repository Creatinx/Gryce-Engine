#pragma once

// GryceEngineUtils::ui::signal.h — 信号系统
//
// 轻量信号槽：connect 注册回调，emit 派发。支持自由函数/lambda/
// 成员函数连接；成员函数连接在对象析构时由 connect_member 的
// 对象存活标记自动跳过（不会悬挂调用）。

#include <functional>
#include <memory>
#include <vector>

namespace GryceEngineUtils::ui {

// 主模板只声明；偏特化把签名解析成参数包：
//   Signal<void()>          → Args = {}
//   Signal<void(float)>     → Args = {float}
//   Signal<void(Widget*,bool)> → Args = {Widget*, bool}
template<typename Signature>
class Signal;

template<typename... Args>
class Signal<void(Args...)> {
public:
    using Callback = std::function<void(Args...)>;

    void connect(Callback cb) {
        if (cb) callbacks_.emplace_back(Entry{std::move(cb)});
    }

    void disconnect() {
        callbacks_.clear();
    }

    // 连接成员函数：内部持有对象的存活标记，对象析构后自动跳过
    template<typename T>
    void connect_member(T* obj, void (T::*method)(Args...)) {
        if (!obj || !method) return;
        std::weak_ptr<bool> weak = obj->signal_lifetime();
        callbacks_.emplace_back(Entry{
            [obj, method, weak](Args... args) {
                if (auto life = weak.lock()) {
                    (obj->*method)(std::forward<Args>(args)...);
                }
            }});
    }

    void emit(Args... args) const {
        for (const auto& entry : callbacks_) {
            entry.cb(std::forward<Args>(args)...);
        }
    }

    size_t callback_count() const { return callbacks_.size(); }

private:
    struct Entry {
        Callback cb;
    };
    std::vector<Entry> callbacks_;
};

} // namespace GryceEngineUtils::ui
