#pragma once

#include "command.h"

#include <filesystem>
#include <string>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// 文件操作撤销命令集合。
// 所有命令都会同步处理同名的 .meta 文件（存在时一起移动/删除/恢复）。
// Delete 的撤销依赖临时备份目录；backup_dir 在命令构造时指定。
// ---------------------------------------------------------------------------

// 创建空文件夹 —— execute 创建，undo 删除。
class FileCreateFolderCommand : public EditorCommand {
public:
    FileCreateFolderCommand(std::filesystem::path parent_dir, std::string folder_name);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    std::filesystem::path parent_dir_;
    std::string folder_name_;
    std::filesystem::path created_path_;
};

// 创建空场景文件 —— execute 创建，undo 删除。
class FileCreateSceneCommand : public EditorCommand {
public:
    FileCreateSceneCommand(std::filesystem::path parent_dir, std::string file_name);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    std::filesystem::path parent_dir_;
    std::string file_name_;
    std::filesystem::path created_path_;
};

// 创建材质文件 —— execute 创建，undo 删除。
class FileCreateMaterialCommand : public EditorCommand {
public:
    FileCreateMaterialCommand(std::filesystem::path parent_dir, std::string file_name);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    std::filesystem::path parent_dir_;
    std::string file_name_;
    std::filesystem::path created_path_;
};

// 重命名文件/文件夹 —— execute 执行重命名，undo 恢复旧名，同时处理 .meta。
class FileRenameCommand : public EditorCommand {
public:
    FileRenameCommand(std::filesystem::path old_path, std::filesystem::path new_path);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    std::filesystem::path old_path_;
    std::filesystem::path new_path_;

    bool do_rename(const std::filesystem::path& from, const std::filesystem::path& to);
};

// 删除文件/文件夹 —— execute 备份并删除，undo 从临时目录恢复。
class FileDeleteCommand : public EditorCommand {
public:
    FileDeleteCommand(std::filesystem::path target_path, std::filesystem::path backup_dir);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    std::filesystem::path target_path_;
    std::filesystem::path backup_dir_;
    std::filesystem::path backup_subdir_;

    bool backup_to_temp();
    bool restore_from_temp();
    void remove_backup();
};

// 移动/剪切文件/文件夹 —— execute 移动，undo 移回旧路径，同时处理 .meta。
class FileMoveCommand : public EditorCommand {
public:
    FileMoveCommand(std::filesystem::path old_path, std::filesystem::path new_path);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    std::filesystem::path old_path_;
    std::filesystem::path new_path_;

    bool do_move(const std::filesystem::path& from, const std::filesystem::path& to);
};

// 复制文件/文件夹 —— execute 复制，undo 删除生成的副本，同时处理 .meta。
class FileCopyCommand : public EditorCommand {
public:
    FileCopyCommand(std::filesystem::path src_path, std::filesystem::path dst_path);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    std::filesystem::path src_path_;
    std::filesystem::path dst_path_;
};

// 工具函数：获取对应 .meta 路径（如果不存在则返回空）。
std::filesystem::path meta_path_of(const std::filesystem::path& path);

} // namespace gryce_engine::editor
