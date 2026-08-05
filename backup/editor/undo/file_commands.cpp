#include "file_commands.h"

#include <fstream>
#include <random>

#include "utils/glog/glog_lib.h"
#include "../platform_utils.h"
#include "render/material.h"
#include "scene/scene_serializer.h"
#include <nlohmann/json.hpp>

namespace gryce_engine::editor {

namespace {

std::string random_suffix() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t v = dist(gen);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

bool copy_directory_recursive(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    if (ec) return false;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(src, ec)) {
        if (ec) return false;
        const auto relative = std::filesystem::relative(entry.path(), src, ec);
        if (ec) return false;
        const auto target = dst / relative;
        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(target, ec);
            if (ec) return false;
        } else if (entry.is_regular_file(ec)) {
            std::filesystem::copy_file(entry.path(), target,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return false;
        }
    }
    return true;
}

bool move_with_meta(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) return false;

    auto from_meta = editor::meta_path_of(from);
    if (!from_meta.empty()) {
        auto to_meta = std::filesystem::path(to.wstring() + L".meta");
        std::filesystem::rename(from_meta, to_meta, ec);
        // .meta 移动失败不视为整体失败（非致命）
        if (ec) {
            GLOG_WARN("FileCommand: failed to move meta '{}' -> '{}'", path_to_utf8(from_meta),
                      path_to_utf8(to_meta));
        }
    }
    return true;
}

} // namespace

std::filesystem::path meta_path_of(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path candidate(path.wstring() + L".meta");
    if (std::filesystem::exists(candidate, ec) && !ec) {
        return candidate;
    }
    return {};
}

// ---------------------------------------------------------------------------
// FileCreateFolderCommand
// ---------------------------------------------------------------------------
FileCreateFolderCommand::FileCreateFolderCommand(std::filesystem::path parent_dir,
                                                 std::string folder_name)
    : parent_dir_(std::move(parent_dir)), folder_name_(std::move(folder_name)) {
    created_path_ = parent_dir_ / utf8_path(folder_name_);
}

void FileCreateFolderCommand::execute() {
    std::error_code ec;
    std::filesystem::create_directories(created_path_, ec);
    if (ec) {
        GLOG_ERROR("FileCommand: failed to create folder '{}'", path_to_utf8(created_path_));
    }
}

void FileCreateFolderCommand::undo() {
    std::error_code ec;
    if (std::filesystem::exists(created_path_, ec)) {
        std::filesystem::remove_all(created_path_, ec);
    }
}

std::string FileCreateFolderCommand::description() const {
    return "Create folder";
}

// ---------------------------------------------------------------------------
// FileCreateSceneCommand
// ---------------------------------------------------------------------------
FileCreateSceneCommand::FileCreateSceneCommand(std::filesystem::path parent_dir,
                                               std::string file_name)
    : parent_dir_(std::move(parent_dir)), file_name_(std::move(file_name)) {
    created_path_ = parent_dir_ / utf8_path(file_name_);
}

void FileCreateSceneCommand::execute() {
    scene::Scene temp_scene;
    temp_scene.set_name(file_name_);
    if (!scene::SceneSerializer::save_to_file(temp_scene, path_to_utf8(created_path_))) {
        GLOG_ERROR("FileCommand: failed to create scene '{}'", path_to_utf8(created_path_));
    }
}

void FileCreateSceneCommand::undo() {
    std::error_code ec;
    if (std::filesystem::exists(created_path_, ec)) {
        std::filesystem::remove(created_path_, ec);
    }
    auto meta = meta_path_of(created_path_);
    if (!meta.empty()) {
        std::filesystem::remove(meta, ec);
    }
}

std::string FileCreateSceneCommand::description() const {
    return "Create scene";
}

// ---------------------------------------------------------------------------
// FileCreateMaterialCommand
// ---------------------------------------------------------------------------
FileCreateMaterialCommand::FileCreateMaterialCommand(std::filesystem::path parent_dir,
                                                     std::string file_name)
    : parent_dir_(std::move(parent_dir)), file_name_(std::move(file_name)) {
    created_path_ = parent_dir_ / utf8_path(file_name_);
}

void FileCreateMaterialCommand::execute() {
    render::Material material;
    nlohmann::json j;
    material.serialize(j);
    std::ofstream ofs(created_path_);
    if (ofs) {
        ofs << j.dump(2);
    } else {
        GLOG_ERROR("FileCommand: failed to create material '{}'", path_to_utf8(created_path_));
    }
}

void FileCreateMaterialCommand::undo() {
    std::error_code ec;
    if (std::filesystem::exists(created_path_, ec)) {
        std::filesystem::remove(created_path_, ec);
    }
    auto meta = meta_path_of(created_path_);
    if (!meta.empty()) {
        std::filesystem::remove(meta, ec);
    }
}

std::string FileCreateMaterialCommand::description() const {
    return "Create material";
}

// ---------------------------------------------------------------------------
// FileRenameCommand
// ---------------------------------------------------------------------------
FileRenameCommand::FileRenameCommand(std::filesystem::path old_path,
                                     std::filesystem::path new_path)
    : old_path_(std::move(old_path)), new_path_(std::move(new_path)) {}

bool FileRenameCommand::do_rename(const std::filesystem::path& from,
                                  const std::filesystem::path& to) {
    return move_with_meta(from, to);
}

void FileRenameCommand::execute() {
    if (!do_rename(old_path_, new_path_)) {
        GLOG_ERROR("FileCommand: rename failed '{}' -> '{}'", path_to_utf8(old_path_),
                   path_to_utf8(new_path_));
    }
}

void FileRenameCommand::undo() {
    if (!do_rename(new_path_, old_path_)) {
        GLOG_ERROR("FileCommand: rename undo failed '{}' -> '{}'", path_to_utf8(new_path_),
                   path_to_utf8(old_path_));
    }
}

std::string FileRenameCommand::description() const {
    return "Rename file";
}

// ---------------------------------------------------------------------------
// FileDeleteCommand
// ---------------------------------------------------------------------------
FileDeleteCommand::FileDeleteCommand(std::filesystem::path target_path,
                                     std::filesystem::path backup_dir)
    : target_path_(std::move(target_path)), backup_dir_(std::move(backup_dir)) {
    backup_subdir_ = backup_dir_ / ("del_" + random_suffix());
}

bool FileDeleteCommand::backup_to_temp() {
    std::error_code ec;
    std::filesystem::create_directories(backup_subdir_, ec);
    if (ec) {
        GLOG_ERROR("FileCommand: backup create_dirs failed '{}' ec={}", path_to_utf8(backup_subdir_), ec.value());
        return false;
    }

    const auto stem = target_path_.filename();
    const auto backup_target = backup_subdir_ / stem;

    if (std::filesystem::is_directory(target_path_, ec)) {
        if (!copy_directory_recursive(target_path_, backup_target)) {
            GLOG_ERROR("FileCommand: backup directory recursive failed '{}' -> '{}'", path_to_utf8(target_path_), path_to_utf8(backup_target));
            return false;
        }
    } else {
        std::filesystem::copy_file(target_path_, backup_target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            GLOG_ERROR("FileCommand: backup copy_file failed '{}' -> '{}' ec={}", path_to_utf8(target_path_), path_to_utf8(backup_target), ec.value());
            return false;
        }
    }

    auto meta = meta_path_of(target_path_);
    if (!meta.empty()) {
        std::filesystem::copy_file(meta, backup_subdir_ / meta.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            GLOG_WARN("FileCommand: backup .meta failed '{}' -> '{}' ec={}", path_to_utf8(meta), path_to_utf8(backup_subdir_ / meta.filename()), ec.value());
        }
    }
    return true;
}

bool FileDeleteCommand::restore_from_temp() {
    std::error_code ec;
    const auto stem = target_path_.filename();
    const auto backup_target = backup_subdir_ / stem;
    if (!std::filesystem::exists(backup_target, ec)) {
        GLOG_ERROR("FileCommand: restore backup missing '{}' ec={}", path_to_utf8(backup_target), ec.value());
        return false;
    }

    std::filesystem::path parent = target_path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            GLOG_ERROR("FileCommand: restore create_dirs failed '{}' ec={}", path_to_utf8(parent), ec.value());
        }
    }

    if (std::filesystem::is_directory(backup_target, ec)) {
        if (!copy_directory_recursive(backup_target, target_path_)) {
            GLOG_ERROR("FileCommand: restore directory recursive failed '{}' -> '{}'", path_to_utf8(backup_target), path_to_utf8(target_path_));
            return false;
        }
    } else {
        std::filesystem::copy_file(backup_target, target_path_,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            GLOG_ERROR("FileCommand: restore copy_file failed '{}' -> '{}' ec={}", path_to_utf8(backup_target), path_to_utf8(target_path_), ec.value());
            return false;
        }
    }

    auto backup_meta = backup_subdir_ / (stem.wstring() + L".meta");
    if (std::filesystem::exists(backup_meta, ec) && !ec) {
        std::filesystem::copy_file(backup_meta, std::filesystem::path(target_path_.wstring() + L".meta"),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            GLOG_WARN("FileCommand: restore .meta failed '{}' ec={}", path_to_utf8(backup_meta), ec.value());
        }
    }
    return true;
}

void FileDeleteCommand::remove_backup() {
    std::error_code ec;
    std::filesystem::remove_all(backup_subdir_, ec);
}

void FileDeleteCommand::execute() {
    std::error_code ec;
    GLOG_INFO("FileCommand: delete execute '{}' backup='{}'", path_to_utf8(target_path_), path_to_utf8(backup_subdir_));
    if (!std::filesystem::exists(target_path_, ec) || ec) {
        GLOG_WARN("FileCommand: delete target does not exist '{}' ec={}", path_to_utf8(target_path_), ec.value());
        return;
    }
    if (!backup_to_temp()) {
        GLOG_ERROR("FileCommand: failed to backup '{}'", path_to_utf8(target_path_));
        return;
    }
    if (std::filesystem::is_directory(target_path_, ec)) {
        std::filesystem::remove_all(target_path_, ec);
    } else {
        std::filesystem::remove(target_path_, ec);
    }
    if (ec) {
        GLOG_ERROR("FileCommand: delete failed '{}' ec={}", path_to_utf8(target_path_), ec.value());
        failed_ = true;
        return;
    }
    auto meta = meta_path_of(target_path_);
    if (!meta.empty()) {
        std::filesystem::remove(meta, ec);
        if (ec) {
            GLOG_WARN("FileCommand: delete .meta failed '{}' ec={}", path_to_utf8(meta), ec.value());
        }
    }
}

void FileDeleteCommand::undo() {
    GLOG_INFO("FileCommand: delete undo '{}' backup='{}'", path_to_utf8(target_path_), path_to_utf8(backup_subdir_));
    if (!restore_from_temp()) {
        GLOG_ERROR("FileCommand: restore failed '{}' backup='{}'", path_to_utf8(target_path_), path_to_utf8(backup_subdir_));
        return;
    }
    remove_backup();
    GLOG_INFO("FileCommand: delete undo completed '{}'", path_to_utf8(target_path_));
}

std::string FileDeleteCommand::description() const {
    return "Delete file";
}

// ---------------------------------------------------------------------------
// FileMoveCommand
// ---------------------------------------------------------------------------
FileMoveCommand::FileMoveCommand(std::filesystem::path old_path, std::filesystem::path new_path)
    : old_path_(std::move(old_path)), new_path_(std::move(new_path)) {}

bool FileMoveCommand::do_move(const std::filesystem::path& from, const std::filesystem::path& to) {
    return move_with_meta(from, to);
}

void FileMoveCommand::execute() {
    if (!do_move(old_path_, new_path_)) {
        GLOG_ERROR("FileCommand: move failed '{}' -> '{}'", path_to_utf8(old_path_),
                   path_to_utf8(new_path_));
    }
}

void FileMoveCommand::undo() {
    if (!do_move(new_path_, old_path_)) {
        GLOG_ERROR("FileCommand: move undo failed '{}' -> '{}'", path_to_utf8(new_path_),
                   path_to_utf8(old_path_));
    }
}

std::string FileMoveCommand::description() const {
    return "Move file";
}

// ---------------------------------------------------------------------------
// FileCopyCommand
// ---------------------------------------------------------------------------
FileCopyCommand::FileCopyCommand(std::filesystem::path src_path, std::filesystem::path dst_path)
    : src_path_(std::move(src_path)), dst_path_(std::move(dst_path)) {}

void FileCopyCommand::execute() {
    std::error_code ec;
    if (std::filesystem::is_directory(src_path_, ec)) {
        if (!copy_directory_recursive(src_path_, dst_path_)) {
            GLOG_ERROR("FileCommand: copy directory failed '{}' -> '{}'", path_to_utf8(src_path_),
                       path_to_utf8(dst_path_));
            return;
        }
    } else {
        std::filesystem::copy_file(src_path_, dst_path_, ec);
        if (ec) {
            GLOG_ERROR("FileCommand: copy file failed '{}' -> '{}'", path_to_utf8(src_path_),
                       path_to_utf8(dst_path_));
            return;
        }
    }
}

void FileCopyCommand::undo() {
    std::error_code ec;
    if (std::filesystem::is_directory(dst_path_, ec)) {
        std::filesystem::remove_all(dst_path_, ec);
    } else {
        std::filesystem::remove(dst_path_, ec);
        std::filesystem::path dst_meta(dst_path_.wstring() + L".meta");
        if (std::filesystem::exists(dst_meta, ec)) {
            std::filesystem::remove(dst_meta, ec);
        }
    }
}

std::string FileCopyCommand::description() const {
    return "Copy file";
}

} // namespace gryce_engine::editor
