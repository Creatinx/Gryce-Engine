// file_watcher_test.cpp — FileWatcher 文件监听系统单元测试
//
// 测试 FileWatcher 的以下功能：
// 1. 注册/取消监听文件
// 2. 监听文件变化回调
// 3. 多次监听同一文件
// 4. 清空所有监听

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>

#include "GryceEngineUtils/ui/file_watcher.h"

using namespace GryceEngineUtils::ui;

namespace {

// 创建临时文件
std::string create_temp_file(const std::string& name, const std::string& content = "hello") {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto path = (temp_dir / name).string();
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
    ofs.close();
    return path;
}

// 修改文件内容（触发修改时间更新）
void touch_file(const std::string& path, const std::string& content = "updated") {
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 确保时间戳变化
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
    ofs.close();
}

// 删除文件
void remove_temp_file(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

// ============================================================================
// 基础烟雾测试
// ============================================================================

TEST(FileWatcherSmoke, CreateDestroy) {
    FileWatcher watcher;
    EXPECT_EQ(watcher.watched_count(), 0);
}

// ============================================================================
// 注册 / 取消监听测试
// ============================================================================

class FileWatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = create_temp_file("file_watcher_test_" + std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->name()[0]) + ".tmp");
    }

    void TearDown() override {
        remove_temp_file(test_file_);
    }

    std::string test_file_;
};

// 注册监听文件
TEST_F(FileWatcherTest, WatchFile) {
    FileWatcher watcher;
    bool called = false;
    watcher.watch(test_file_, [&called](const std::string&) { called = true; });
    EXPECT_EQ(watcher.watched_count(), 1);
}

// 取消监听
TEST_F(FileWatcherTest, UnwatchFile) {
    FileWatcher watcher;
    watcher.watch(test_file_, [](const std::string&) {});
    EXPECT_EQ(watcher.watched_count(), 1);
    watcher.unwatch(test_file_);
    EXPECT_EQ(watcher.watched_count(), 0);
}

// 清空所有监听
TEST_F(FileWatcherTest, ClearAll) {
    FileWatcher watcher;
    watcher.watch(test_file_, [](const std::string&) {});
    watcher.watch(create_temp_file("fw_test_other.tmp"), [](const std::string&) {});
    EXPECT_EQ(watcher.watched_count(), 2);
    watcher.clear();
    EXPECT_EQ(watcher.watched_count(), 0);
    remove_temp_file("fw_test_other.tmp");
}

// 多次监听同一文件应更新回调
TEST_F(FileWatcherTest, ReWatchSameFile) {
    FileWatcher watcher;
    int call_count = 0;
    watcher.watch(test_file_, [&call_count](const std::string&) { call_count++; });
    EXPECT_EQ(watcher.watched_count(), 1);

    // 再次监听同一文件
    watcher.watch(test_file_, [&call_count](const std::string&) { call_count++; });
    EXPECT_EQ(watcher.watched_count(), 1); // 不增加计数
}

// ============================================================================
// 变化检测测试
// ============================================================================

// 检测文件变化
TEST_F(FileWatcherTest, DetectFileChange) {
    FileWatcher watcher;
    bool called = false;
    std::string changed_path;
    watcher.watch(test_file_, [&called, &changed_path](const std::string& path) {
        called = true;
        changed_path = path;
    }, 100);

    // 初始 poll 应记录时间戳，不触发回调
    watcher.poll();

    // 修改文件
    touch_file(test_file_);

    // 轮询直到检测到变化或超时（文件系统时间戳分辨率有限）
    // 使用 retry 避免 NTFS 时间戳精度问题
    bool detected = false;
    for (int retry = 0; retry < 10; ++retry) {
        if (watcher.poll() > 0) {
            detected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(detected);
    EXPECT_TRUE(called);
    EXPECT_EQ(changed_path, test_file_);
}

// 未修改的文件不应触发回调
TEST_F(FileWatcherTest, UnchangedFileNoCallback) {
    FileWatcher watcher;
    bool called = false;
    watcher.watch(test_file_, [&called](const std::string&) {
        called = true;
    }, 100);

    // 多次 poll 不应触发
    watcher.poll();
    watcher.poll();
    watcher.poll();

    EXPECT_FALSE(called);
}

// 取消监听后不再检测变化
TEST_F(FileWatcherTest, UnwatchStopsDetection) {
    FileWatcher watcher;
    bool called = false;
    watcher.watch(test_file_, [&called](const std::string&) {
        called = true;
    }, 100);

    watcher.poll(); // 初始化
    watcher.unwatch(test_file_);

    touch_file(test_file_);

    // 多次 poll 确保不触发
    for (int retry = 0; retry < 5; ++retry) {
        watcher.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_FALSE(called);
}

// 监听不存在的文件不应崩溃
TEST_F(FileWatcherTest, WatchNonExistentFile) {
    FileWatcher watcher;
    bool called = false;
    watcher.watch("nonexistent_file_xyz.tmp", [&called](const std::string&) {
        called = true;
    }, 100);

    watcher.poll();
    EXPECT_FALSE(called);
}

// 空路径不应注册
TEST_F(FileWatcherTest, EmptyPathIgnored) {
    FileWatcher watcher;
    watcher.watch("", [](const std::string&) {});
    EXPECT_EQ(watcher.watched_count(), 0);
}

// 空回调不应注册
TEST_F(FileWatcherTest, EmptyCallbackIgnored) {
    FileWatcher watcher;
    watcher.watch(test_file_, FileWatcher::FileChangeCallback{});
    EXPECT_EQ(watcher.watched_count(), 0);
}