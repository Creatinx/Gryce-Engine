#pragma once

#include "components/node_ui.h"
#include "ui/ui_runtime.h"

namespace gryce_engine::components {

enum class UIRootSpace : uint8_t {
    Screen = 0,
    World = 1,
};

// 一个 Entity 上的 UI 根。组件树中的 NodeUI 子节点构成完整界面。
// UIRuntime 是运行时缓存，不进入场景序列化。
class UIRoot : public NodeUI {
public:
    bool active = true;
    bool accepts_input = true;
    UIRootSpace space = UIRootSpace::Screen;
    float world_scale = 1.0f;

    UIRoot() {
        size = math::Vector2f(0.0f, 0.0f);
        runtime_.set_root(this);
    }

    NodeDomain node_domain() const noexcept override { return NodeDomain::UI; }
    const char* type() const override { return "UIRoot"; }

    ui::UIRuntime& runtime() { return runtime_; }
    const ui::UIRuntime& runtime() const { return runtime_; }

    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIRoot)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }

    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["active"] = active;
        out["accepts_input"] = accepts_input;
        out["space"] = static_cast<int>(space);
        out["world_scale"] = world_scale;
    }

    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        active = in.value("active", true);
        accepts_input = in.value("accepts_input", true);
        space = static_cast<UIRootSpace>(
            in.value("space", static_cast<int>(UIRootSpace::Screen)));
        world_scale = in.value("world_scale", 1.0f);
    }

private:
    ui::UIRuntime runtime_;
};

} // namespace gryce_engine::components
