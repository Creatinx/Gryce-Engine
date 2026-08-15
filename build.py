#!/usr/bin/env python3
"""Gryce Engine -- one-click build script (Windows MinGW-w64/MSVC, Linux GCC/Clang)

用法:
    python build.py [config] [选项]

示例:
    python build.py                    # 编译 Debug
    python build.py Release            # 编译 Release
    python build.py --setup-deps       # 仅下载依赖
    python build.py --clean            # 清理构建产物（保留 deps）
    python build.py --clean-all        # 完全清理（包括 deps）
    python build.py --editor           # 预留：同时启用 Editor(C#) 编译开关

Linux 编译前置条件（安装 X11/GL 开发头文件，供 GLFW/GLEW 从源码构建）:
    sudo apt install build-essential cmake ninja-build python3 \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
        libxi-dev libxkbcommon-dev mesa-common-dev libgl-dev \
        libglew-dev libglfw3-dev  # 可选 libvulkan-dev / lld
"""

import argparse
import re
import shutil
import subprocess
import sys
import os
from pathlib import Path

# 平台常量：Windows 之外（Linux/macOS）走 POSIX 路径。
IS_WINDOWS = (os.name == 'nt')

# 编译器输出按 UTF-8 解码（见 run()）；打印到 GBK 控制台时替换不可编码字符，避免崩溃。
if os.name == 'nt':
    try:
        sys.stdout.reconfigure(errors='replace')
        sys.stderr.reconfigure(errors='replace')
    except Exception:
        pass

# ---------------------------------------------------------------------------
# Colors (disabled on Windows without ANSI support)
# ---------------------------------------------------------------------------
def supports_color():
    if os.name == 'nt' and 'ANSICON' not in os.environ:
        return False
    return True


if supports_color():
    C_OK = '\033[92m'
    C_WARN = '\033[93m'
    C_ERR = '\033[91m'
    C_INFO = '\033[96m'
    C_RESET = '\033[0m'
else:
    C_OK = C_WARN = C_ERR = C_INFO = C_RESET = ''


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def run(cmd, cwd=None, check=False, stream=True):
    """Run a command. If stream=True, prints output in real-time.
    Returns (ok, collected_output)."""
    try:
        if not stream:
            result = subprocess.run(
                cmd, cwd=cwd, capture_output=True, text=True,
                encoding='utf-8', errors='replace',
                shell=(os.name == 'nt')
            )
            output = (result.stdout or '') + (result.stderr or '')
            if check and result.returncode != 0:
                print(f"{C_ERR}[ERROR] Command failed:{C_RESET}")
                print(f"  {' '.join(cmd)}")
                print(output)
                sys.exit(1)
            return result.returncode == 0, output

        proc = subprocess.Popen(
            cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding='utf-8', errors='replace',
            shell=(os.name == 'nt')
        )
        lines = []
        for line in proc.stdout:
            stripped = line.rstrip('\n')
            if stripped:
                lines.append(stripped)
                print(stripped)
        proc.stdout.close()
        return_code = proc.wait()
        if check and return_code != 0:
            sys.exit(1)
        return return_code == 0, '\n'.join(lines)
    except FileNotFoundError as e:
        return False, str(e)


def find_in_path(name):
    """Cross-platform which."""
    return shutil.which(name)


def find_msys2_mingw():
    """Search known MSYS2 MinGW paths for gcc/g++."""
    candidates = [
        Path("C:/msys64/ucrt64/bin"),
        Path("C:/msys64/mingw64/bin"),
        Path("C:/msys64/clang64/bin"),
    ]
    if os.environ.get("MSYS2_PREFIX"):
        candidates.insert(0, Path(os.environ["MSYS2_PREFIX"]) / "bin")

    for base in candidates:
        gcc = base / "gcc.exe"
        gxx = base / "g++.exe"
        if gcc.exists() and gxx.exists():
            return str(gcc), str(gxx), str(base)
    return None, None, None


def clean_build_artifacts(build_dir, keep_deps=True):
    """Remove build artifacts.

    注意：依赖实际存放在 {args.build_dir}/deps/（源码根下，独立于各 config 子目录），
    删除某个 config 目录不会影响它，因此这里的 keep_deps 参数已不再需要挪移依赖目录。
    """
    bd = Path(build_dir)
    if not bd.exists():
        return

    if not keep_deps:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Cleaning {bd} (including deps) ...")
    else:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Cleaning {bd} (preserving deps cache) ...")
    shutil.rmtree(bd)


def ensure_deps(offline: bool = False):
    """Ensure all dependencies are downloaded via deps_manager.py."""
    deps_script = Path(__file__).parent / "tools" / "deps_manager.py"
    if not deps_script.exists():
        print(f"{C_ERR}[ERROR]{C_RESET} deps_manager.py not found at {deps_script}")
        sys.exit(1)

    mode = " (offline mode — no network)" if offline else ""
    print(f"{C_INFO}[Gryce Engine]{C_RESET} Checking dependencies{mode} ...")
    cmd = [sys.executable, str(deps_script), "download"]
    if offline:
        cmd.append("--offline")
    ok, output = run(cmd, check=False)
    if not ok:
        print(f"{C_ERR}[ERROR]{C_RESET} Dependency check failed:")
        print(output)
        sys.exit(1)


# ---------------------------------------------------------------------------
# Solution sync
# ---------------------------------------------------------------------------
def _fix_project_path(path, project_root):
    """把解决方案中的项目路径修正为相对于项目根目录的路径。

    CMake 生成的 .slnx 中，所有相对路径都是相对于 build/（二进制目录）的。
    将 .slnx 从 build/ 移到项目根目录时，所有相对路径都需要加上 build/ 前缀。

    规则：
      - 绝对路径（且位于项目根下）-> 相对路径
      - 所有相对路径 -> 加上 build/ 前缀
    """
    path = path.replace('\\', '/')
    p = Path(path)
    if p.is_absolute():
        try:
            rel = p.relative_to(project_root)
            return str(rel.as_posix())
        except ValueError:
            return path
    return 'build/' + path


def _sync_sln_text(project_root, build_dir, build_sln, root_sln):
    """同步旧版文本格式 .sln。"""
    content = build_sln.read_text(encoding='utf-8-sig')

    # 修正所有 .vcxproj 路径：添加 build\ 前缀
    def fix_vcxproj_path(match):
        prefix = match.group(1)  # ="Name", "
        path = match.group(2)    # 文件路径
        suffix = match.group(3)  # ", {GUID}
        if ':' in path:  # 绝对路径，跳过
            return match.group(0)
        if path.startswith('build\\') or path.startswith('build/'):
            return match.group(0)
        return f'{prefix}build\\{path}{suffix}'

    content = re.sub(
        r'(= "[^"]+", ")([^"]+\.vcxproj)(",)',
        fix_vcxproj_path,
        content
    )

    # 修正 GryceEngine.Editor 路径：绝对路径 -> 相对路径
    content = re.sub(
        r'"' + re.escape(str(project_root)) + r'[\\/]editor[\\/]GryceEngine\.Editor\.csproj"',
        '"editor\\GryceEngine.Editor.csproj"',
        content
    )
    # 也处理正斜杠版本
    content = re.sub(
        r'"' + re.escape(project_root.as_posix()) + r'/editor/GryceEngine\.Editor\.csproj"',
        '"editor\\GryceEngine.Editor.csproj"',
        content
    )

    root_sln.write_text(content, encoding='utf-8-sig')
    print(f"{C_OK}[Gryce Engine]{C_RESET} Synced root solution: {root_sln}")

    # 修正 .sln 文件头版本号为 VS2026
    for sln_file in [root_sln, build_sln]:
        if sln_file.exists():
            sln_content = sln_file.read_text(encoding='utf-8-sig')
            if '# Visual Studio Version 17' in sln_content:
                sln_content = sln_content.replace(
                    '# Visual Studio Version 17',
                    '# Visual Studio Version 18'
                )
                sln_file.write_text(sln_content, encoding='utf-8-sig')


def _sync_slnx_xml(project_root, build_dir, build_slnx, root_slnx):
    """同步新版 XML 格式 .slnx（VS2026 默认生成）。"""
    import xml.etree.ElementTree as ET

    tree = ET.parse(build_slnx)
    root = tree.getroot()

    # .slnx 没有命名空间时 ElementTree 直接解析标签；有则保留前缀。
    # 只修正 <Project Path="..."> 和 <BuildDependency Project="...">，
    # 不要碰 <Platform Project="x64"> 或 <Build Project="false">。
    def fix_elem(elem):
        tag = elem.tag.split('}')[-1] if '}' in elem.tag else elem.tag
        if tag == 'Project' and 'Path' in elem.attrib:
            elem.set('Path', _fix_project_path(elem.attrib['Path'], project_root))
        if tag == 'BuildDependency' and 'Project' in elem.attrib:
            elem.set('Project', _fix_project_path(elem.attrib['Project'], project_root))
        for child in elem:
            fix_elem(child)

    fix_elem(root)

    # 保持 XML 声明和缩进
    root_slnx.write_bytes(b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding='UTF-8'))
    print(f"{C_OK}[Gryce Engine]{C_RESET} Synced root solution: {root_slnx}")


def sync_solution_to_root(project_root, build_dir):
    """将 CMake 生成的解决方案文件同步到项目根目录，修正路径使其可从根目录打开。

    支持旧版 .sln 与 VS2026 默认生成的 .slnx。
    """
    build_sln = build_dir / "GryceEngine.sln"
    build_slnx = build_dir / "GryceEngine.slnx"
    root_sln = project_root / "GryceEngine.sln"
    root_slnx = project_root / "GryceEngine.slnx"

    if build_sln.exists():
        _sync_sln_text(project_root, build_dir, build_sln, root_sln)
    elif build_slnx.exists():
        _sync_slnx_xml(project_root, build_dir, build_slnx, root_slnx)
    else:
        return

    # 替换 vcxproj 中的 PlatformToolset 和 ToolsVersion 为 VS2026 标准，
    # 避免 VS 显示升级标识
    for base_dir in [build_dir, project_root / "out" / "vs"]:
        if not base_dir.exists():
            continue
        for vcxproj in base_dir.glob("**/*.vcxproj"):
            try:
                vcx_content = vcxproj.read_text(encoding='utf-8')
                changed = False
                # 统一 PlatformToolset 为 v145（VS2026）
                if '<PlatformToolset>v143</PlatformToolset>' in vcx_content:
                    vcx_content = vcx_content.replace(
                        '<PlatformToolset>v143</PlatformToolset>',
                        '<PlatformToolset>v145</PlatformToolset>'
                    )
                    changed = True
                # 统一 ToolsVersion 为 18.0（VS2026）
                if 'ToolsVersion="17.0"' in vcx_content:
                    vcx_content = vcx_content.replace(
                        'ToolsVersion="17.0"', 'ToolsVersion="18.0"'
                    )
                    changed = True
                if changed:
                    vcxproj.write_text(vcx_content, encoding='utf-8')
            except Exception:
                pass

    # 清理 .vs 缓存目录
    for vs_dir in [project_root / ".vs", project_root / "out" / "vs" / ".vs"]:
        if vs_dir.exists():
            shutil.rmtree(vs_dir, ignore_errors=True)
    print(f"{C_INFO}[Gryce Engine]{C_RESET} Cleaned VS solution cache")


# ---------------------------------------------------------------------------
# Build logic
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Gryce Engine build script -- wrapper around cmake + ninja"
    )
    parser.add_argument(
        "config", nargs="?", default="Debug",
        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
        help="CMake build configuration (default: Debug)"
    )
    parser.add_argument(
        "--setup-deps", action="store_true",
        help="Only download and extract dependencies, do not build"
    )
    parser.add_argument(
        "--clean", action="store_true",
        help="Clean build artifacts but preserve deps/ cache"
    )
    parser.add_argument(
        "--clean-all", action="store_true",
        help="Remove entire build directory including deps/ (forces re-download)"
    )
    parser.add_argument(
        "--verbose", action="store_true",
        help="Pass --verbose to ninja"
    )
    parser.add_argument(
        "--jobs", "-j", type=int, default=0,
        help="Number of parallel jobs (default: auto)"
    )
    parser.add_argument(
        "--build-dir", default="build",
        help="Build directory prefix (default: build)"
    )
    parser.add_argument(
        "--no-lock", action="store_true",
        help="Do NOT auto-lock compiler (use CMake default detection)"
    )
    parser.add_argument(
        "--msvc", action="store_true",
        help="Force MSVC compiler and Visual Studio 2026 generator"
    )
    parser.add_argument(
        "--editor", action="store_true",
        help="预留：启用 Editor（C#）编译开关（-DGRYCE_BUILD_EDITOR=ON）。"
             "当前 Editor 为 WPF，仅 Windows + Visual Studio generator 生效；"
             "未来移植跨平台 UI 后在 Linux 上亦可用。"
    )
    parser.add_argument(
        "--offline", action="store_true",
        help="Skip network downloads; use only local cached dependencies"
    )
    args = parser.parse_args()

    config = args.config
    # Visual Studio is a multi-config generator: use a single build root.
    # Ninja is single-config: keep per-config subdirectories.
    if args.msvc:
        build_dir = Path(args.build_dir)
    else:
        build_dir = Path(args.build_dir) / config
    project_root = Path(__file__).parent.resolve()

    # -----------------------------------------------------------------------
    # 0. Setup deps only mode
    # -----------------------------------------------------------------------
    if args.setup_deps:
        ensure_deps(offline=args.offline)
        print(f"{C_OK}[Gryce Engine]{C_RESET} Dependencies ready.")
        sys.exit(0)

    print(f"{C_INFO}[Gryce Engine]{C_RESET} Build configuration: {C_OK}{config}{C_RESET}")

    # -----------------------------------------------------------------------
    # 1. Detect compiler
    # -----------------------------------------------------------------------
    gcc_path = find_in_path("gcc")
    gxx_path = find_in_path("g++")
    cl_path = find_in_path("cl")
    msys_bin = None
    compiler_family = None

    if args.msvc:
        if not cl_path:
            print(f"{C_ERR}[ERROR] --msvc requested but cl.exe not found in PATH.{C_RESET}")
            print('    Open "x64 Native Tools Command Prompt for VS 2026" and run:')
            print("        python build.py --msvc")
            sys.exit(1)
        print(f"{C_OK}[OK]{C_RESET} Forced MSVC cl.exe: {cl_path}")
        compiler_family = "msvc"
    elif not args.no_lock:
        if gcc_path and gxx_path:
            print(f"{C_OK}[OK]{C_RESET} Found gcc in PATH: {gcc_path}")
            compiler_family = "gcc"
        else:
            gcc_path, gxx_path, msys_bin = find_msys2_mingw()
            if gcc_path and msys_bin:
                print(f"{C_INFO}[Gryce Engine]{C_RESET} Found MSYS2 MinGW: {msys_bin}")
                os.environ["PATH"] = msys_bin + os.pathsep + os.environ.get("PATH", "")
                compiler_family = "gcc"

        if compiler_family is None and cl_path:
            print(f"{C_OK}[OK]{C_RESET} Found MSVC cl.exe in PATH: {cl_path}")
            compiler_family = "msvc"

        if compiler_family is None:
            if IS_WINDOWS:
                tips = (
                    "This project supports:\n"
                    "  * MSYS2 UCRT64 MinGW-w64 (recommended)\n"
                    "  * MSVC (Visual Studio 2026+)\n\n"
                    "For MinGW (MSYS2 UCRT64 terminal):\n"
                    "    pacman -S mingw-w64-ucrt-x86_64-gcc "
                    "mingw-w64-ucrt-x86_64-cmake "
                    "mingw-w64-ucrt-x86_64-ninja "
                    "mingw-w64-ucrt-x86_64-glew "
                    "mingw-w64-ucrt-x86_64-glfw\n\n"
                    "Then either:\n"
                    "    1. Run this script from the MSYS2 UCRT64 terminal.\n"
                    "    2. Add C:\\msys64\\ucrt64\\bin to your system PATH and retry.\n\n"
                    "For MSVC:\n"
                    '    Open "x64 Native Tools Command Prompt for VS 2026" and run:\n'
                    "        python build.py --msvc"
                )
            else:
                tips = (
                    "This project supports GCC and Clang.\n\n"
                    "Install (Debian/Ubuntu):\n"
                    "    sudo apt install g++ cmake ninja-build python3 \\\n"
                    "        libx11-dev libxrandr-dev libxinerama-dev \\\n"
                    "        libxcursor-dev libxi-dev libxkbcommon-dev \\\n"
                    "        mesa-common-dev libgl-dev libglew-dev\n\n"
                    "Then retry: python build.py"
                )
            print(f"\n{C_ERR}[ERROR] No supported compiler found in PATH.{C_RESET}\n\n{tips}")
            sys.exit(1)
    else:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} --no-lock: using CMake default compiler detection")
        compiler_family = "auto"

    # -----------------------------------------------------------------------
    # 2. Detect cmake, ninja and select generator
    # -----------------------------------------------------------------------
    cmake = find_in_path("cmake")
    ninja = find_in_path("ninja")

    if not cmake:
        print(f"{C_ERR}[ERROR] cmake not found.{C_RESET}")
        print(f"Install: pacman -S mingw-w64-ucrt-x86_64-cmake")
        sys.exit(1)

    # Prefer Visual Studio 2026 when using MSVC so the generated solution
    # opens in VS2026 without upgrade prompts.
    generator = None
    if compiler_family == "msvc":
        generator = "Visual Studio 18 2026"
        print(f"{C_OK}[OK]{C_RESET} Using generator: {generator} (MSVC)")
    elif ninja:
        generator = "Ninja"
        print(f"{C_OK}[OK]{C_RESET} ninja: {ninja}")
        print(f"{C_OK}[OK]{C_RESET} Using generator: {generator}")
    else:
        print(f"{C_WARN}[WARN] ninja not found, falling back to default generator.{C_RESET}")

    print(f"{C_OK}[OK]{C_RESET} cmake: {cmake}")

    # -----------------------------------------------------------------------
    # 3. Clean if requested (先于 ensure_deps，避免清完又立刻重新下载)
    # -----------------------------------------------------------------------
    if args.clean_all:
        if build_dir.exists():
            clean_build_artifacts(build_dir)
        # 真正的共享依赖目录位于源码根 build/deps/，必须一并删除才能强制重新下载
        shared_deps = Path(args.build_dir) / "deps"
        if shared_deps.exists():
            print(f"{C_INFO}[Gryce Engine]{C_RESET} Removing shared dependency cache: {shared_deps} ...")
            shutil.rmtree(shared_deps)
    elif args.clean and build_dir.exists():
        clean_build_artifacts(build_dir)

    # -----------------------------------------------------------------------
    # 4. Ensure dependencies
    # -----------------------------------------------------------------------
    ensure_deps(offline=args.offline)

    # -----------------------------------------------------------------------
    # 5. Configure
    # -----------------------------------------------------------------------
    def needs_reconfigure():
        cache = build_dir / "CMakeCache.txt"
        if not cache.exists():
            return True
        if generator:
            expected = f"CMAKE_GENERATOR:INTERNAL={generator}"
            content = cache.read_text(encoding='utf-8', errors='ignore')
            if expected not in content:
                return True
        return False

    if needs_reconfigure():
        if build_dir.exists():
            print(f"{C_WARN}[Gryce Engine]{C_RESET} {build_dir} exists but generator/cache mismatch, reconfiguring ...")
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Configuring with CMake ...")
        configure_cmd = [
            cmake, "-B", str(build_dir),
            "-DCMAKE_BUILD_TYPE=" + config,
        ]
        if generator:
            configure_cmd += ["-G", generator]

        if not args.no_lock and compiler_family == "gcc" and gcc_path:
            configure_cmd += [
                "-DCMAKE_C_COMPILER=" + gcc_path,
                "-DCMAKE_CXX_COMPILER=" + gxx_path,
            ]

        # 预留接口：启用 Editor（C#）编译开关。当前 WPF Editor 仅
        # Windows + Visual Studio generator 生效，Linux 上暂不参与编译。
        if args.editor:
            configure_cmd += ["-DGRYCE_BUILD_EDITOR=ON"]
            if IS_WINDOWS:
                print(
                    f"{C_WARN}[Gryce Engine]{C_RESET} --editor: Editor(C#) build enabled "
                    "(requires Visual Studio generator)"
                )
            else:
                print(
                    f"{C_WARN}[Gryce Engine]{C_RESET} --editor: GRYCE_BUILD_EDITOR=ON passed; "
                    "WPF Editor is Windows-only, core will be unaffected"
                )

        configure_cmd += [str(project_root)]

        ok, output = run(configure_cmd, check=False)
        if not ok:
            print(f"{C_ERR}[ERROR] CMake configuration failed:{C_RESET}")
            print(output)
            sys.exit(1)
        print(f"{C_OK}[OK]{C_RESET} Configuration complete.")
    else:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Using existing configuration: {build_dir}")

    # -----------------------------------------------------------------------
    # 6. Build
    # -----------------------------------------------------------------------
    print(f"{C_INFO}[Gryce Engine]{C_RESET} Building ...")
    build_cmd = [cmake, "--build", str(build_dir)]
    if args.verbose:
        build_cmd.append("--verbose")
    if args.jobs > 0:
        build_cmd += ["-j", str(args.jobs)]

    ok, output = run(build_cmd, check=False)
    if not ok:
        print(f"{C_ERR}[ERROR] Build failed:{C_RESET}")
        print(output)
        sys.exit(1)

    print(f"{C_OK}[Gryce Engine]{C_RESET} Build complete.")
    print(f"  Binaries: {build_dir}/bin/{config}/")

    # -----------------------------------------------------------------------
    # 7. Sync root solution
    # -----------------------------------------------------------------------
    sync_solution_to_root(project_root, Path(args.build_dir))


if __name__ == "__main__":
    main()
