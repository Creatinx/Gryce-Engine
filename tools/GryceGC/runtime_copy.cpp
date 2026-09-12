#include "runtime_copy.h"

#include <cstdio>
#include <filesystem>
#include <iostream>

#include "runtime_util.h"

namespace fs = std::filesystem;

namespace gryce_engine::gc {

namespace {

// Copy the MSVC CRT runtime DLLs into runtime/. When the VS redist folder is
// found, all of its DLLs are copied; debug builds additionally need the debug
// Universal CRT (ucrtbased.dll).
bool copy_msvc_runtime(const fs::path& build_dir, bool debug,
                       const fs::path& runtime_dir,
                       const fs::path& out_dir,
                       std::vector<std::string>& copied) {
    const fs::path src_dir = find_msvc_crt_dir(build_dir, debug);
    size_t found = 0;
    std::error_code ec;

    // The CRT is statically imported by the exe at process start, i.e. BEFORE
    // main() can run SetDllDirectoryW("runtime"). It must therefore sit next to
    // the exe (out_dir), not only in runtime/. Keep both copies so the runtime/
    // folder stays complete for DLLs resolved later (harmless duplication).
    auto copy_crt = [&](const fs::path& src, const std::string& dll_name) {
        std::error_code ec;
        fs::copy_file(src, runtime_dir / dll_name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to copy MSVC runtime " << src << "\n";
            return false;
        }
        fs::copy_file(src, out_dir / dll_name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to copy MSVC runtime to root " << src << "\n";
            return false;
        }
        copied.push_back(dll_name);
        ++found;
        return true;
    };

    // Prefer the VS redist folder: copy every DLL it contains.
    bool from_redist = src_dir.filename() != fs::path("System32");
    if (from_redist) {
        for (const auto& entry : fs::directory_iterator(src_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".dll") continue;
            if (!copy_crt(entry.path(), entry.path().filename().string())) return false;
        }
    } else {
        // System32 fallback: copy the common CRT names.
        static const std::vector<std::string> kRelease = {
            "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
            "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
            "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
            "concrt140.dll", "vccorlib140.dll", "vcomp140.dll",
        };
        static const std::vector<std::string> kDebug = {
            "vcruntime140d.dll", "vcruntime140_1d.dll", "vcruntime140_threadsd.dll",
            "msvcp140d.dll", "msvcp140_1d.dll", "msvcp140_2d.dll",
            "msvcp140d_atomic_wait.dll", "msvcp140d_codecvt_ids.dll",
            "concrt140d.dll", "vccorlib140d.dll", "vcomp140d.dll",
        };
        const std::vector<std::string>& names = debug ? kDebug : kRelease;
        for (const std::string& dll : names) {
            const fs::path src = src_dir / dll;
            if (!fs::is_regular_file(src, ec)) continue;
            if (!copy_crt(src, dll)) return false;
        }
    }

    // Debug builds also need the debug Universal CRT (ucrtbased.dll), which
    // lives in the Windows Kits (not the VC redist folder). System32 fallback.
    if (debug) {
        fs::path ucrt_src;
        const fs::path kits = "C:\\Program Files (x86)\\Windows Kits\\10\\bin";
        uint64_t best_version = 0;
        std::error_code ec;
        for (const auto& ver : fs::directory_iterator(kits, ec)) {
            if (!ver.is_directory()) continue;
            uint64_t vnum = 0;
            try {
                vnum = std::stoull(ver.path().filename().string());
            } catch (...) {
                continue;
            }
            const fs::path cand = ver.path() / "x64" / "ucrt" / "ucrtbased.dll";
            if (fs::is_regular_file(cand, ec) && vnum > best_version) {
                best_version = vnum;
                ucrt_src = cand;
            }
        }
        if (ucrt_src.empty()) ucrt_src = fs::path("C:\\Windows\\System32") / "ucrtbased.dll";
        if (fs::is_regular_file(ucrt_src, ec)) {
            copy_crt(ucrt_src, "ucrtbased.dll");
        } else {
            std::cerr << "[grycegc] warning: ucrtbased.dll not found (Debug UCRT missing)\n";
        }
    }

    std::printf("[grycegc] MSVC %s runtime: %zu DLL(s) from %s\n",
                debug ? "Debug" : "Release", found, src_dir.string().c_str());
    return true;
}

} // namespace

bool copy_file_if_exists(const fs::path& src_dir, const std::string& name,
                         const fs::path& dst_dir, std::vector<std::string>& copied) {
    std::error_code ec;
    const fs::path src = src_dir / name;
    if (!fs::is_regular_file(src, ec)) return false;
    fs::copy_file(src, dst_dir / name, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[grycegc] ERROR: failed to copy " << src << ": " << ec.message() << "\n";
        return false;
    }
    copied.push_back(name);
    return true;
}

bool copy_runtime(const fs::path& build_dir, const fs::path& bin_dir, bool debug,
                  const fs::path& out_dir, const fs::path& exe, const std::string& name,
                  std::vector<std::string>& copied) {
    const fs::path runtime_dir = out_dir / "runtime";
    std::error_code ec;
    fs::create_directories(runtime_dir, ec);

    // Detect MinGW by its own runtime DLLs (g++-specific, never produced by
    // MSVC). A MinGW build may still carry plain-named engine DLLs, so the
    // lib-prefix fallback below is not sufficient on its own.
    bool mingw = false;
    for (const char* rt : {"libgcc_s_seh-1.dll", "libstdc++-6.dll"}) {
        if (fs::is_regular_file(bin_dir / rt)) { mingw = true; break; }
    }
    // Engine core DLLs are discovered by prefix in bin_dir, not by a hard-coded
    // full name, so debug/release `d` suffix and `lib` prefix are picked up.
    const std::vector<std::string> core_prefixes = {
        "grycecore", "grycerenderer", "gryceplatform", "grycephysics",
    };
    for (const std::string& prefix : core_prefixes) {
        std::error_code ec2;
        fs::path src = find_sibling_dll(bin_dir, prefix);
        if (src.empty()) {
            std::cerr << "[grycegc] warning: " << prefix << "*.dll not found in " << bin_dir << "\n";
            continue;
        }
        if (src.filename().string().rfind("lib", 0) == 0) mingw = true;
        const std::string dst_name = src.filename().string();
        fs::copy_file(src, runtime_dir / dst_name, fs::copy_options::overwrite_existing, ec2);
        if (ec2) {
            std::cerr << "[grycegc] ERROR: failed to copy " << src << ": " << ec2.message() << "\n";
            return false;
        }
        copied.push_back(dst_name);
    }
    // GLFW: Debug builds link glfw3d.dll; Release links glfw3.dll.
    bool glfw_ok = false;
    {
        const char* want = debug ? "glfw3d" : "glfw3";
        std::error_code ge;
        fs::path glfw;
        if (!(fs::is_regular_file(bin_dir / (std::string(want) + ".dll"), ge)))
            glfw = find_sibling_dll(bin_dir, want);
        else
            glfw = bin_dir / (std::string(want) + ".dll");
        if (glfw.empty()) {
            for (const char* prefix : {"glfw3", "glfw"})
                if (!(glfw = find_sibling_dll(bin_dir, prefix)).empty()) break;
        }
        if (!glfw.empty()) {
            const std::string dst = glfw.filename().string();
            std::error_code g2;
            if (fs::copy_file(glfw, runtime_dir / dst, fs::copy_options::overwrite_existing, g2)) {
                copied.push_back(dst);
                glfw_ok = true;
            }
        }
    }
    if (!glfw_ok) {
        std::cerr << "[grycegc] warning: glfw3d.dll/glfw3.dll not found in " << bin_dir << "\n";
    }
    // GLEW: the renderer may link it dynamically (MinGW shared GLEW builds).
    copy_file_if_exists(bin_dir, "glew32.dll", runtime_dir, copied);
    // MinGW runtime DLLs (self-contained packages; harmless to include).
    for (const char* rt : {"libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"}) {
        copy_file_if_exists(bin_dir, rt, runtime_dir, copied);
    }
    // Vulkan shader compiler (shaderc_shared.dll): loaded at runtime, optional.
    const fs::path shaderc = find_shaderc_dll();
    if (!shaderc.empty()) {
        std::error_code se;
        fs::copy_file(shaderc, runtime_dir / "shaderc_shared.dll",
                      fs::copy_options::overwrite_existing, se);
        if (se) {
            std::cerr << "[grycegc] warning: failed to copy shaderc_shared.dll: "
                      << se.message() << "\n";
        } else {
            copied.push_back("shaderc_shared.dll");
        }
    }
    // MSVC CRT runtime (release/debug per config). MinGW carries its own above.
    if (!mingw) {
        if (!copy_msvc_runtime(build_dir, debug, runtime_dir, out_dir, copied)) {
            return false;
        }
    }

    // MinGW builds do not support /DELAYLOAD, so the exe imports the engine DLLs
    // at process start BEFORE SetDllDirectoryW("runtime") runs. Mirror the
    // entire staged runtime folder to the output root so the load-time closure
    // always resolves.
    if (mingw) {
        std::error_code ec2;
        for (const auto& entry : fs::directory_iterator(runtime_dir, ec2)) {
            if (!entry.is_regular_file(ec2)) continue;
            fs::copy_file(entry.path(), out_dir / entry.path().filename(),
                          fs::copy_options::overwrite_existing, ec2);
        }
    }

    fs::copy_file(exe, out_dir / (name + ".exe"), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[grycegc] ERROR: failed to copy " << exe << ": " << ec.message() << "\n";
        return false;
    }
    return true;
}

} // namespace gryce_engine::gc