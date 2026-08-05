#include "create_entity_dialog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "resources/project.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"
#include "imgui.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

namespace {

// 类型分类（与筛选下拉框一一对应；0 保留给「全部」）
enum class TypeCategory : int { ThreeD = 1, TwoD = 2, Light = 3, Camera = 4, Other = 5, Physics = 6, Audio = 7 };

} // namespace

namespace {

// 可创建类型注册表条目
struct TypeEntry {
    const char* id;            // 稳定 id，用于持久化与回调
    const char* icon_label;    // 图块文字
    ImU32 icon_bg;
    ImU32 icon_text;
    const char* name_key;      // 名称本地化 key
    const char* desc_key;      // 描述本地化 key
    TypeCategory category;
};

// 全部可创建类型（图标配色沿用 Hierarchy 面板的 Godot 风格字母图块）
const TypeEntry k_type_entries[] = {
    {"node3d",      "3D",  IM_COL32(230, 140, 70, 255),  IM_COL32(255, 255, 255, 255),
     "create_entity.name.node3d", "create_entity.desc.node3d", TypeCategory::ThreeD},
    {"cube",        "M",   IM_COL32(60, 180, 220, 255),  IM_COL32(0, 40, 60, 255),
     "create_entity.name.cube", "create_entity.desc.cube", TypeCategory::ThreeD},
    {"node2d",      "2D",  IM_COL32(80, 150, 230, 255),  IM_COL32(255, 255, 255, 255),
     "create_entity.name.node2d", "create_entity.desc.node2d", TypeCategory::TwoD},
    {"sprite2d",    "UI",  IM_COL32(90, 190, 130, 255),  IM_COL32(255, 255, 255, 255),
     "create_entity.name.sprite2d", "create_entity.desc.sprite2d", TypeCategory::TwoD},
    {"label",       "UI",  IM_COL32(90, 190, 130, 255),  IM_COL32(255, 255, 255, 255),
     "create_entity.name.label", "create_entity.desc.label", TypeCategory::TwoD},
    {"color_rect",  "UI",  IM_COL32(90, 190, 130, 255),  IM_COL32(255, 255, 255, 255),
     "create_entity.name.color_rect", "create_entity.desc.color_rect", TypeCategory::TwoD},
    {"dir_light",   "L",   IM_COL32(240, 200, 60, 255),  IM_COL32(40, 30, 0, 255),
     "create_entity.name.dir_light", "create_entity.desc.dir_light", TypeCategory::Light},
    {"point_light", "L",   IM_COL32(240, 200, 60, 255),  IM_COL32(40, 30, 0, 255),
     "create_entity.name.point_light", "create_entity.desc.point_light", TypeCategory::Light},
    {"spot_light",  "L",   IM_COL32(240, 200, 60, 255),  IM_COL32(40, 30, 0, 255),
     "create_entity.name.spot_light", "create_entity.desc.spot_light", TypeCategory::Light},
    {"camera",      "C",   IM_COL32(160, 110, 220, 255), IM_COL32(255, 255, 255, 255),
     "create_entity.name.camera", "create_entity.desc.camera", TypeCategory::Camera},
    {"circle",      "UI",  IM_COL32(90, 190, 130, 255),  IM_COL32(255, 255, 255, 255),
     "create_entity.name.circle", "create_entity.desc.circle", TypeCategory::TwoD},
    {"polygon",     "UI",  IM_COL32(90, 190, 130, 255),  IM_COL32(255, 255, 255, 255),
     "create_entity.name.polygon", "create_entity.desc.polygon", TypeCategory::TwoD},
    {"camera2d",    "C",   IM_COL32(160, 110, 220, 255), IM_COL32(255, 255, 255, 255),
     "create_entity.name.camera2d", "create_entity.desc.camera2d", TypeCategory::Camera},
    {"light2d",     "L",   IM_COL32(240, 200, 60, 255),  IM_COL32(40, 30, 0, 255),
     "create_entity.name.light2d", "create_entity.desc.light2d", TypeCategory::Light},
    {"ambient_light2d", "L", IM_COL32(240, 200, 60, 255), IM_COL32(40, 30, 0, 255),
     "create_entity.name.ambient_light2d", "create_entity.desc.ambient_light2d", TypeCategory::Light},
    {"static_body2d", "S", IM_COL32(100, 190, 120, 255), IM_COL32(255, 255, 255, 255),
     "create_entity.name.static_body2d", "create_entity.desc.static_body2d", TypeCategory::Physics},
    {"rigid_body2d", "R",  IM_COL32(220, 80, 80, 255),   IM_COL32(255, 255, 255, 255),
     "create_entity.name.rigid_body2d", "create_entity.desc.rigid_body2d", TypeCategory::Physics},
    {"static_body3d", "S", IM_COL32(100, 190, 120, 255), IM_COL32(255, 255, 255, 255),
     "create_entity.name.static_body3d", "create_entity.desc.static_body3d", TypeCategory::Physics},
    {"rigid_body3d", "R",  IM_COL32(220, 80, 80, 255),   IM_COL32(255, 255, 255, 255),
     "create_entity.name.rigid_body3d", "create_entity.desc.rigid_body3d", TypeCategory::Physics},
    {"audio_source", "A",  IM_COL32(230, 100, 160, 255), IM_COL32(255, 255, 255, 255),
     "create_entity.name.audio_source", "create_entity.desc.audio_source", TypeCategory::Audio},
    {"audio_listener", "A", IM_COL32(230, 100, 160, 255), IM_COL32(255, 255, 255, 255),
     "create_entity.name.audio_listener", "create_entity.desc.audio_listener", TypeCategory::Audio},
    {"empty",       "N",   IM_COL32(128, 128, 128, 255), IM_COL32(255, 255, 255, 255),
     "create_entity.name.empty", "create_entity.desc.empty", TypeCategory::Other},
};

const TypeEntry* find_entry(const std::string& id) {
    for (const auto& e : k_type_entries) {
        if (id == e.id) return &e;
    }
    return nullptr;
}

// 大小写不敏感子串匹配
bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

const char* category_header_key(TypeCategory cat) {
    switch (cat) {
        case TypeCategory::ThreeD: return "create_entity.filter.3d";
        case TypeCategory::TwoD:   return "create_entity.filter.2d";
        case TypeCategory::Light:  return "create_entity.filter.light";
        case TypeCategory::Camera: return "create_entity.filter.camera";
        case TypeCategory::Other:  return "create_entity.filter.other";
        case TypeCategory::Physics: return "create_entity.filter.physics";
        case TypeCategory::Audio:  return "create_entity.filter.audio";
    }
    return "create_entity.filter.other";
}

// 绘制一个类型行（图块 + 名称），返回是否被点击 / 双击
void draw_type_row(const TypeEntry& entry, bool selected,
                   bool& out_clicked, bool& out_double_clicked, float row_width = 0.0f) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float font_size = ImGui::GetFontSize();
    const float row_height = ImGui::GetFrameHeight() + 4.0f;
    const float icon_size = std::max(14.0f, font_size * 0.95f);

    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    out_clicked = ImGui::Selectable("##row", selected,
                                    ImGuiSelectableFlags_AllowDoubleClick,
                                    ImVec2(row_width, row_height));
    out_double_clicked = out_clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    // 在 Selectable 之上叠加图块与名称
    ImGui::SetCursorScreenPos(row_min);
    const float icon_y = row_min.y + (row_height - icon_size) * 0.5f;
    const ImVec2 icon_min(row_min.x + 2.0f, icon_y);
    const ImVec2 icon_max(icon_min.x + icon_size, icon_min.y + icon_size);
    draw_list->AddRectFilled(icon_min, icon_max, entry.icon_bg, 3.0f);

    const ImVec2 icon_text_size = ImGui::CalcTextSize(entry.icon_label);
    const ImVec2 icon_text_pos(icon_min.x + (icon_size - icon_text_size.x) * 0.5f,
                               icon_min.y + (icon_size - icon_text_size.y) * 0.5f);
    draw_list->AddText(icon_text_pos, entry.icon_text, entry.icon_label);

    const ImVec2 name_pos(icon_max.x + ImGui::GetStyle().ItemInnerSpacing.x + 4.0f,
                          row_min.y + (row_height - font_size) * 0.5f);
    draw_list->AddText(name_pos, ImGui::GetColorU32(ImGuiCol_Text), tr(entry.name_key));

    // 手动推进光标到下一行
    ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + row_height));
}

} // namespace

void CreateEntityDialog::open(scene::Entity* parent) {
    parent_uuid_ = parent ? parent->uuid() : scene::UUID::nil();
    open_ = true;
    pending_open_ = true; // 在 draw() 中再 OpenPopup：上下文菜单的 ID 栈与面板窗口不同，菜单内直接 OpenPopup 会导致 ID 不匹配、弹窗永不显示
    first_frame_ = true;
    search_[0] = '\0';
    filter_category_ = 0;
    selected_id_.clear();
    if (!persistent_loaded_) {
        load_persistent_state();
        persistent_loaded_ = true;
    }
}

void CreateEntityDialog::load_persistent_state() {
    favorites_.clear();
    recent_.clear();

    std::filesystem::path path =
        std::filesystem::path(resources::Project::instance().root()) / "create_entity_dialog.json";
    std::ifstream file(path);
    if (!file) return; // 首次使用尚无文件

    try {
        nlohmann::json j = nlohmann::json::parse(file);
        if (j.contains("favorites") && j["favorites"].is_array()) {
            for (const auto& v : j["favorites"]) {
                if (v.is_string() && find_entry(v.get<std::string>())) {
                    favorites_.push_back(v.get<std::string>());
                }
            }
        }
        if (j.contains("recent") && j["recent"].is_array()) {
            for (const auto& v : j["recent"]) {
                if (v.is_string() && find_entry(v.get<std::string>())) {
                    recent_.push_back(v.get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        GLOG_WARN("CreateEntityDialog: failed to parse '{}': {}", path.string(), e.what());
    }
}

void CreateEntityDialog::save_persistent_state() const {
    std::filesystem::path path =
        std::filesystem::path(resources::Project::instance().root()) / "create_entity_dialog.json";

    nlohmann::json j;
    j["favorites"] = favorites_;
    j["recent"] = recent_;

    std::ofstream file(path);
    if (!file) {
        GLOG_WARN("CreateEntityDialog: cannot write '{}'", path.string());
        return;
    }
    file << j.dump(2);
}

void CreateEntityDialog::push_recent(const std::string& type_id) {
    recent_.erase(std::remove(recent_.begin(), recent_.end(), type_id), recent_.end());
    recent_.insert(recent_.begin(), type_id);
    if (recent_.size() > 10) recent_.resize(10);
    save_persistent_state();
}

bool CreateEntityDialog::is_favorite(const std::string& type_id) const {
    return std::find(favorites_.begin(), favorites_.end(), type_id) != favorites_.end();
}

void CreateEntityDialog::toggle_favorite(const std::string& type_id) {
    auto it = std::find(favorites_.begin(), favorites_.end(), type_id);
    if (it != favorites_.end()) {
        favorites_.erase(it);
    } else {
        favorites_.push_back(type_id);
    }
    save_persistent_state();
}

void CreateEntityDialog::create_selected(scene::Scene* scene, const std::string& type_id) {
    if (!create_handler_ || !find_entry(type_id)) return;
    scene::Entity* parent = scene ? scene->find_entity_by_uuid(parent_uuid_) : nullptr;
    create_handler_(type_id, parent);
    push_recent(type_id);
    ImGui::CloseCurrentPopup();
    open_ = false;
}

void CreateEntityDialog::draw(scene::Scene* scene) {
    if (!open_) return;

    // 使用 ###CreateEntity 作为稳定 ID，标题可翻译且不影响 popup 匹配
    const std::string title_str = std::format("{}###CreateEntity", tr("create_entity.title"));
    if (pending_open_) {
        ImGui::OpenPopup(title_str.c_str());
        pending_open_ = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700.0f, 560.0f), ImGuiCond_FirstUseEver);

    bool open = true;
    if (!ImGui::BeginPopupModal(title_str.c_str(), &open, ImGuiWindowFlags_NoScrollbar)) {
        open_ = false;
        return;
    }
    if (!open) {
        // 用户通过 Esc / 关闭按钮关闭
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        open_ = false;
        return;
    }

    const float footer_h = ImGui::GetFrameHeightWithSpacing();
    const float desc_h = ImGui::GetTextLineHeightWithSpacing() * 4.0f + ImGui::GetFrameHeightWithSpacing();
    const float left_w = 180.0f;

    // 主区域（左右两列）
    if (ImGui::BeginChild("##main_cols", ImVec2(0, -desc_h - footer_h))) {
        // ---- 左列：收藏 / 最近使用 ----
        if (ImGui::BeginChild("##left_col", ImVec2(left_w, 0), ImGuiChildFlags_Borders)) {
            ImGui::TextUnformatted(tr("create_entity.favorites"));
            ImGui::Separator();
            const float fav_h = ImGui::GetContentRegionAvail().y * 0.5f;
            if (ImGui::BeginChild("##fav_list", ImVec2(0, fav_h))) {
                if (favorites_.empty()) {
                    ImGui::TextDisabled("(%s)", tr("create_entity.empty_list"));
                }
                for (const auto& id : favorites_) {
                    const TypeEntry* entry = find_entry(id);
                    if (!entry) continue;
                    ImGui::PushID(("fav_" + id).c_str());
                    bool clicked, dbl;
                    draw_type_row(*entry, selected_id_ == id, clicked, dbl);
                    if (clicked) selected_id_ = id;
                    if (dbl) create_selected(scene, id);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::TextUnformatted(tr("create_entity.recent"));
            ImGui::Separator();
            if (ImGui::BeginChild("##recent_list", ImVec2(0, 0))) {
                if (recent_.empty()) {
                    ImGui::TextDisabled("(%s)", tr("create_entity.empty_list"));
                }
                for (const auto& id : recent_) {
                    const TypeEntry* entry = find_entry(id);
                    if (!entry) continue;
                    ImGui::PushID(("recent_" + id).c_str());
                    bool clicked, dbl;
                    draw_type_row(*entry, selected_id_ == id, clicked, dbl);
                    if (clicked) selected_id_ = id;
                    if (dbl) create_selected(scene, id);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // ---- 右列：搜索 / 筛选 / 匹配项 ----
        if (ImGui::BeginChild("##right_col", ImVec2(0, 0))) {
            // 搜索框（打开时自动聚焦；回车创建最佳匹配）
            if (first_frame_) {
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-1.0f);
            const bool enter_pressed = ImGui::InputTextWithHint(
                "##search", tr("create_entity.search"), search_, sizeof(search_),
                ImGuiInputTextFlags_EnterReturnsTrue);

            // 分类筛选
            static const char* k_filter_keys[] = {
                "create_entity.filter.all", "create_entity.filter.3d", "create_entity.filter.2d",
                "create_entity.filter.light", "create_entity.filter.camera", "create_entity.filter.other",
                "create_entity.filter.physics", "create_entity.filter.audio",
            };
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo("##filter", tr(k_filter_keys[filter_category_]))) {
                for (int i = 0; i < 8; ++i) {
                    if (ImGui::Selectable(tr(k_filter_keys[i]), filter_category_ == i)) {
                        filter_category_ = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(tr("create_entity.matches"));
            ImGui::Separator();

            // 收集匹配项
            const std::string filter = search_;
            std::vector<const TypeEntry*> matches;
            for (const auto& e : k_type_entries) {
                if (filter_category_ != 0 && static_cast<int>(e.category) != filter_category_) continue;
                if (!filter.empty() &&
                    !contains_case_insensitive(tr(e.name_key), filter) &&
                    !contains_case_insensitive(e.id, filter)) {
                    continue;
                }
                matches.push_back(&e);
            }

            if (enter_pressed && !matches.empty()) {
                // 回车：创建当前最佳匹配（选中项优先，否则第一个匹配）
                const std::string best =
                    (selected_id_.empty() || std::none_of(matches.begin(), matches.end(),
                        [&](const TypeEntry* e) { return selected_id_ == e->id; }))
                        ? matches.front()->id
                        : selected_id_;
                create_selected(scene, best);
            }

            if (ImGui::BeginChild("##match_list", ImVec2(0, 0))) {
                TypeCategory last_cat = static_cast<TypeCategory>(-1);
                bool any = false;
                for (const TypeEntry* entry : matches) {
                    // 分类标题（仅在「全部」筛选下分组显示）
                    if (filter_category_ == 0 && entry->category != last_cat) {
                        if (any) ImGui::Spacing();
                        ImGui::TextDisabled("%s", tr(category_header_key(entry->category)));
                        last_cat = entry->category;
                    }
                    any = true;

                    ImGui::PushID(entry->id);
                    // 收藏星标开关
                    const bool fav = is_favorite(entry->id);
                    if (ImGui::SmallButton(fav ? "*" : "-")) {
                        toggle_favorite(entry->id);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", tr("create_entity.favorites"));
                    }
                    ImGui::SameLine();

                    bool clicked, dbl;
                    draw_type_row(*entry, selected_id_ == entry->id, clicked, dbl);
                    if (clicked) selected_id_ = entry->id;
                    if (dbl) create_selected(scene, entry->id);
                    ImGui::PopID();
                }
                if (!any) {
                    ImGui::TextDisabled("%s", tr("create_entity.no_result"));
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    // ---- 描述面板 ----
    if (ImGui::BeginChild("##desc", ImVec2(0, desc_h), ImGuiChildFlags_Borders)) {
        ImGui::TextDisabled("%s", tr("create_entity.description"));
        ImGui::Separator();
        const TypeEntry* entry = selected_id_.empty() ? nullptr : find_entry(selected_id_);
        if (entry) {
            ImGui::PushTextWrapPos();
            ImGui::TextUnformatted(tr(entry->desc_key));
            ImGui::PopTextWrapPos();
        } else {
            ImGui::TextDisabled("%s", tr("create_entity.no_selection"));
        }
    }
    ImGui::EndChild();

    // ---- 底部按钮 ----
    const float btn_w = 100.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - btn_w * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().WindowPadding.x);
    if (selected_id_.empty()) ImGui::BeginDisabled();
    if (ImGui::Button(tr("create_entity.create"), ImVec2(btn_w, 0))) {
        create_selected(scene, selected_id_);
    }
    if (selected_id_.empty()) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("create_entity.cancel"), ImVec2(btn_w, 0))) {
        ImGui::CloseCurrentPopup();
        open_ = false;
    }

    first_frame_ = false;
    ImGui::EndPopup();
}

} // namespace gryce_engine::editor
