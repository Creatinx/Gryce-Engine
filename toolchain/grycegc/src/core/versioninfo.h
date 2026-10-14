#pragma once

// ---------------------------------------------------------------------------
// GryceGC versioninfo：PE VERSIONINFO 资源写入（M5，Windows only）
//
// 用 BeginUpdateResourceW 更新现有 .exe/.dll 的 RT_VERSION 资源，
// 不重新签名/不改节表。POSIX 平台为 no-op。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include <filesystem>

namespace fs = std::filesystem;

namespace grycegc {

struct VersionInfoFields {
    std::string company;      // CompanyName
    std::string copyright;    // LegalCopyright
    std::string product;      // ProductName
    std::string version;      // ProductVersion（同时用于 FileVersion）
    std::string file_version;
};

// 写入/更新 PE 的 VERSIONINFO；成功 true。非 Windows 恒 true（no-op）。
bool pe_write_version_info(const fs::path& pe, const VersionInfoFields& f);

} // namespace grycegc
