#include "i18n.h"

namespace gryce_engine::editor {

I18n& I18n::instance() {
    static I18n i18n;
    return i18n;
}

void I18n::set_language(const std::string& lang) {
    if (language_ == lang) return;
    language_ = lang;
    zh_table_.clear();
    if (lang == "zh") {
        load_zh_table();
    }
}

const char* I18n::tr(const char* key) const {
    if (!key) return key;
    if (!zh_table_.empty()) {
        const auto it = zh_table_.find(key);
        if (it != zh_table_.end()) return it->second.c_str();
    }
    return key;
}

void I18n::load_zh_table() {
    zh_table_ = {
        // ---- 菜单 ----
        {"File", "文件"},
        {"New Scene", "新建场景"},
        {"Open Scene", "打开场景"},
        {"Save Scene", "保存场景"},
        {"Save As...", "另存为..."},
        {"Close Scene", "关闭场景"},
        {"Exit", "退出"},
        {"Edit", "编辑"},
        {"Undo", "撤销"},
        {"Redo", "重做"},
        {"Editor Settings...", "编辑器设置..."},
        {"View", "视图"},
        {"Demo Window", "演示窗口"},
        {"Metrics", "性能指标"},
        {"Default Layout", "默认布局"},
        {"Render Settings", "渲染设置"},
        {"Help", "帮助"},
        {"About Gryce Engine", "关于 Gryce Engine"},

        // ---- 工具栏 ----
        {"Play", "播放"},
        {"Pause", "暂停"},
        {"Stop", "停止"},
        {"Resume", "继续"},
        {"Step", "步进"},
        {"Loading...", "正在加载..."},
        {"Play mode active", "播放模式运行中"},

        // ---- 状态栏 ----
        {"Frame", "帧"},
        {"Entities", "实体"},

        // ---- 渲染设置 ----
        {"Shadow", "阴影"},
        {"Point Shadow", "点阴影"},
        {"Shadow Mode", "阴影模式"},
        {"Shadow Atlas", "阴影图集"},
        {"Post-Processing", "后处理"},
        {"SSR", "屏幕空间反射"},
        {"DOF", "景深"},
        {"Focus Distance", "对焦距离"},
        {"Focus Range", "对焦范围"},
        {"Blur Amount", "模糊程度"},
        {"Motion Blur", "动态模糊"},
        {"Environment", "环境"},
        {"Volumetric Fog", "体积雾"},
        {"Fog Color", "雾颜色"},
        {"Density", "密度"},
        {"Height", "高度"},
        {"Water", "水面"},
        {"Global Illumination", "全局光照"},
        {"GI Mode", "GI 模式"},
        {"Indirect Intensity", "间接光强度"},

        // ---- 编辑器设置 ----
        {"Editor Settings", "编辑器设置"},
        {"Appearance", "外观"},
        {"Theme", "主题"},
        {"Language", "语言"},
        {"Width", "宽度"},
        {"Height", "高度"},
        {"Apply Size", "应用尺寸"},
        {"Maximize state is saved automatically on close", "最大化状态会在关闭编辑器时自动保存"},

        // ---- 面板标题 ----
        {"Scene", "场景"},
        {"FileSystem", "文件系统"},
        {"Inspector", "检查器"},
        {"Viewport", "视口"},
        {"Output", "输出"},
        {"Project", "项目"},

        // ---- 层级面板 ----
        {"No active scene", "无活动场景"},
        {"Create Entity", "创建实体"},
        {"Create Child", "创建子实体"},
        {"Delete", "删除"},
        {"Entity", "实体"},

        // ---- 检查器 ----
        {"Select an entity to inspect", "选择一个实体以查看"},
        {"Enabled", "启用"},
        {"Add Component", "添加组件"},
        {"Search...", "搜索..."},
        {"No reflection metadata for %s", "没有 %s 的反射元数据"},

        // ---- 控制台 ----
        {"Error", "错误"},
        {"Warn", "警告"},
        {"Info", "信息"},
        {"Debug", "调试"},
        {"Level:", "级别:"},
        {"Auto-scroll", "自动滚动"},
        {"Filter...", "过滤..."},
        {"Clear", "清除"},
        {"Memory log sink is not installed", "未安装内存日志接收器"},

        // ---- 文件系统面板 ----
        {"History", "历史记录"},
        {"History tab is not yet implemented", "历史记录尚未实现"},
        {"Project root is not configured", "未配置项目根目录"},
        {"(empty)", "(空)"},
        {"Directory not found: %s", "目录不存在: %s"},
    };
}

} // namespace gryce_engine::editor