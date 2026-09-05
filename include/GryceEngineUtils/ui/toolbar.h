#pragma once

#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/divider.h"
#include "GryceEngineUtils/ui/panel.h"

namespace GryceEngineUtils::ui {

// ToolBar — 工具栏（横向容器，按钮 + 分隔线）
class ToolBar : public Panel {
public:
    explicit ToolBar(const char* id = "toolbar");

    // 便捷创建：添加一个按钮并返回（用于 connect 信号/继续配置）
    Button* add_button(const char* id, const char* text);
    // 便捷创建：添加分隔线
    Divider* add_divider(const char* id = nullptr);

    const char* type_name() const override { return "ToolBar"; }
};

} // namespace GryceEngineUtils::ui
