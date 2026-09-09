#!/usr/bin/env python3
"""Gryce Engine -- assemble the build output into a standardized dist layout.

Both a direct `cmake --build .` and build.py end up running this script (via the
CMake `gryce_dist` ALL target). It reorganizes the flat single-config output
(`<build-dir>/bin/<Config>`) into self-contained, copy-and-publish folders:

    <build-dir>/bin/<Config>/dist/
        GryceGC/            GryceGC 自包含目录（GryceGC.exe + runtime/ 引擎 DLL）
        GryceGame/          GryceGame 自包含目录（GryceGame.exe + runtime/ 引擎 DLL）
        GryceEditor/        GryceEditor 自包含目录（若构建）
        GryceTests/         GryceTests 自包含目录（含 gtest DLL）
        GryceEngineUtils/   库目录（承接 Core + GryceEngineUtils）
            lib/            所有内核 DLL + 导入库 + 静态归档
            include/        全部公开头文件（include/ 树 + src/**/*.{h,hpp}）

每个可执行程序文件夹的 `runtime/` 与其根目录的 exe 一一对应：引擎 DLL 与
GryceEngineUtils/lib 同集，直接复制即可发布；GryceGC 打包游戏时就从自己的
同级 `runtime/` 或 GryceEngineUtils/lib 读取运行库。
"""

import argparse
import os
import re
import shutil
import sys
from pathlib import Path

# 单配置目录下的二进制位置（由 CMake 的 CMAKE_*_OUTPUT_DIRECTORY 保证）。
# build-dir 即 CMAKE_BINARY_DIR；flat 输出在 <build>/bin/<Config>，归档(导入库/静态库)在 <build>/lib/<Config>。
def flat_dir(build_dir, config):
    return Path(build_dir) / "bin" / config


def archive_dir(build_dir, config):
    return Path(build_dir) / "lib" / config


def cp(src, dst):
    """Copy a file, creating parent dirs and skipping missing sources."""
    src = Path(src)
    if not src.is_file():
        return False
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return True


def collect_flat(flat):
    """Classify every file in the flat output dir."""
    engine, gfx, toolchain, gtest, rest = [], [], [], [], []
    for f in flat.iterdir():
        if not f.is_file():
            continue
        n = f.name
        if n.startswith("libgtest") and n.endswith(".dll"):
            gtest.append(f)
        elif n.endswith(".dll"):
            if re.match(r"^(lib)?Gryce", n):
                engine.append(f)
            elif n.startswith("glfw") or n.startswith("glew"):
                gfx.append(f)
            elif n in ("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
                toolchain.append(f)
            else:
                rest.append(f)
    return {"engine": engine, "gfx": gfx, "toolchain": toolchain,
            "gtest": gtest, "rest": rest}


def assemble_runnable(dist_root, folder_name, exe_path, dll_files):
    """Create a self-contained folder: exe at the target root, and the engine
    runtime DLLs (same set as GryceEngineUtils/lib) under a `runtime/` subfolder —
    mirroring the packaged game layout (exe + runtime/).

    exe 统一重命名为与文件夹同名的 Pascal 形式（如 grycegc.exe -> GryceGC.exe），
    匹配"打包产物 Pascal 命名"约定；GryceGC 打包时按同级 `runtime/` 读取引擎 DLL。
    """
    out_dir = dist_root / folder_name
    runtime_dir = out_dir / "runtime"
    out_dir.mkdir(parents=True, exist_ok=True)
    suffix = Path(exe_path).suffix  # .exe (Windows) 或 '' (POSIX)
    if not cp(exe_path, out_dir / (folder_name + suffix)):
        return False
    # 引擎 DLL 同时放两处：根目录（保证 exe 启动时能找到依赖）与 runtime/
    # （GryceGC 打包游戏时读取的同级来源）。
    for dll in dll_files:
        cp(dll, out_dir / dll.name)
        cp(dll, runtime_dir / dll.name)
    return True


def assemble(sdk_lib, dll_files, import_libs, static_libs):
    for f in dll_files:
        cp(f, sdk_lib / f.name)
    for f in import_libs:
        cp(f, sdk_lib / f.name)
    for f in static_libs:
        cp(f, sdk_lib / f.name)


def assemble_include(sdk_include, source_dir):
    # 1) 整个 include/ 树（GryceEngineUtils 公开头等）
    include_src = source_dir / "include"
    if include_src.is_dir():
        shutil.copytree(include_src, sdk_include, dirs_exist_ok=True)
    # 2) src/**/*.{h,hpp} 头文件，保留相对路径（不含 .cpp/.cc/.hpp 之外）
    src_dir = source_dir / "src"
    if src_dir.is_dir():
        for h in src_dir.rglob("*"):
            if h.is_file() and h.suffix in (".h", ".hpp"):
                rel = h.relative_to(src_dir)
                dst = sdk_include / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(h, dst)


def main():
    parser = argparse.ArgumentParser(description="Assemble engine dist layout")
    parser.add_argument("--build-dir", required=True, help="CMake binary directory")
    parser.add_argument("--config", required=True, default="Debug")
    parser.add_argument("--source-dir", required=True, help="project root")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    source_dir = Path(args.source_dir)
    config = args.config

    flat = flat_dir(build_dir, config)
    libdir = archive_dir(build_dir, config)
    dist_root = flat / "dist"

    if not flat.is_dir():
        print(f"[assemble_dist] no flat output at {flat}; nothing to assemble")
        return 0

    # 每次重建 dist，确保与当前产物一致（不会保留已删除目标的旧文件夹）
    if dist_root.exists():
        shutil.rmtree(dist_root)
    dist_root.mkdir(parents=True, exist_ok=True)

    cat = collect_flat(flat)

    # --- 可执行程序文件夹：exe + runtime/DLL（与 GryceEngineUtils/lib 同集） ---
    all_dlls = cat["engine"] + cat["gfx"] + cat["toolchain"] + cat["gtest"] + cat["rest"]
    runnables = [
        # (文件夹, exe 源文件, runtime DLL 集合)
        ("GryceGC", flat / "grycegc.exe", all_dlls),
        ("GryceGame", flat / "GryceGame.exe", all_dlls),
        ("GryceEditor", flat / "GryceEditor.exe", all_dlls),
        ("GryceTests", flat / "gryce_tests.exe", all_dlls),
    ]
    built = []
    for folder, exe, dlls in runnables:
        if not Path(exe).is_file():
            continue
        if assemble_runnable(dist_root, folder, exe, dlls):
            built.append(folder)

    # --- Core 默认 shader 部署：每个可执行程序文件夹旁放一份 shaders/ 源目录 ---
    # 供未打包的运行路径（编辑器/原生 GryceGame）经 engine_shaders_dir() 定位，
    # 使 core 默认 shader 随构建产物自包含（打的是源文件，首跑才编译）。
    def deploy_core_shaders(target_root):
        src_shaders = source_dir / "src" / "render" / "shaders"
        if not src_shaders.is_dir():
            return
        shutil.copytree(src_shaders, target_root / "shaders", dirs_exist_ok=True)

    # --- shaderc 动态库部署：Vulkan 首编依赖 shaderc_shared.dll，随可执行程序
    # 一并放置（GL-only 运行时由 shaderc_available() 判断，不要求它存在）。---
    def find_shaderc_dll():
        env = os.environ.get("VULKAN_SDK") or os.environ.get("VULKAN_SDK_DIR") or ""
        cands = []
        if env:
            cands.append(Path(env) / "Bin" / "shaderc_shared.dll")
        # 兜底：默认 Vulkan SDK 安装根（若 VULKAN_SDK 未设置）
        cands.append(Path("C:/VulkanSDK") / "Bin" / "shaderc_shared.dll")
        for c in cands:
            try:
                if c.is_file():
                    return c
            except OSError:
                continue
        return None

    shaderc_dll = find_shaderc_dll()
    for folder in built:
        deploy_core_shaders(dist_root / folder)
        if shaderc_dll is not None:
            for sub in ("", "runtime"):
                dst = dist_root / folder / sub / shaderc_dll.name
                dst.parent.mkdir(parents=True, exist_ok=True)
                try:
                    shutil.copy2(shaderc_dll, dst)
                    print(f"[assemble_dist] deployed shaderc: {dst}")
                except OSError as e:
                    print(f"[assemble_dist] warn: skip shaderc copy to {dst}: {e}")

    # --- GryceEngineUtils 库目录 -- --
    sdk_dir = dist_root / "GryceEngineUtils"
    sdk_lib = sdk_dir / "lib"
    sdk_lib.mkdir(parents=True, exist_ok=True)

    # 依赖 DLL：全部 .dll（engine + gfx + toolchain + gtest + 其余）
    # 导入库（MinGW: *.dll.a；MSVC: *.lib）与引擎静态归档
    import_libs = []
    static_libs = []
    if libdir.is_dir():
        for f in libdir.iterdir():
            if not f.is_file():
                continue
            n = f.name
            if n.endswith(".dll.a") or re.match(r"^libGryce.*\.(lib|a)$", n):
                import_libs.append(f)
            elif re.match(r"^libgryce_.*static\.a$", n):
                static_libs.append(f)
    assemble(sdk_lib, all_dlls, import_libs, static_libs)

    # 头文件
    assemble_include(sdk_dir / "include", source_dir)

    print(f"[assemble_dist] dist ready: {dist_root}")
    print(f"[assemble_dist]   runnables: {', '.join(built) if built else '(none)'}")
    print(f"[assemble_dist]   libraries: {sdk_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())