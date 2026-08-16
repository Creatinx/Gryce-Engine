#!/usr/bin/env python3
"""Gryce Engine -- one-click build script (pure CMake + Ninja, no VS solution)

用法:
    python build.py [config] [选项]

示例:
    python build.py                          # Debug，自动检测编译器
    python build.py Release --compiler msvc  # Release + MSVC（需 VS 开发者命令行）
    python build.py --compiler gcc           # 显式 MinGW GCC
    python build.py --compiler clang         # 显式 Clang
    python build.py --editor                 # Windows 上同时构建 WPF Editor（dotnet）
    python build.py --setup-deps             # 仅下载依赖
    python build.py --configure              # 只配置，不编译
    python build.py --clean                  # 清理构建产物（保留 deps）
    python build.py --clean-all              # 完全清理（含 deps）

本项目不依赖 Visual Studio 解决方案（无 .slnx / .vcxproj 生成）：
build.py 与 CMake 统一走单配置目录（build/<Config>），优先使用 Ninja
generator；CLion 可直接打开项目根目录，用任意工具链（MinGW / MSVC /
Clang）自行配置构建。

Linux 编译前置条件（安装 X11/GL 开发头文件，供 GLFW/GLEW 从源码构建）:
    sudo apt install build-essential cmake ninja-build python3 \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
        libxi-dev libxkbcommon-dev mesa-common-dev libgl-dev \
        libglew-dev libglfw3-dev  # 可选 libvulkan-dev / lld
"""

import argparse
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


def load_msvc_environment():
    """Locate Visual Studio via vswhere and import its x64 environment
    (INCLUDE/LIB/PATH) so MSVC + Ninja works from any terminal, not just a
    Developer prompt. Returns True on success."""
    pf86 = os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")
    vswhere = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return False
    try:
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True, encoding='utf-8', errors='replace'
        )
        install_root = result.stdout.strip()
        if not install_root:
            return False
        vcvars = Path(install_root) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if not vcvars.exists():
            return False
        env_result = subprocess.run(
            f'call "{vcvars}" >nul 2>&1 && set',
            shell=True, capture_output=True, text=True,
            encoding='utf-8', errors='replace'
        )
        if env_result.returncode != 0:
            return False
        for line in env_result.stdout.splitlines():
            if '=' in line:
                key, value = line.split('=', 1)
                os.environ[key] = value
        return True
    except Exception:
        return False


def find_msys2_toolchain(compiler_name, compiler_cxx_name):
    """Search known MSYS2 prefixes for the given compiler pair."""
    candidates = [
        Path("C:/msys64/ucrt64/bin"),
        Path("C:/msys64/mingw64/bin"),
        Path("C:/msys64/clang64/bin"),
    ]
    if os.environ.get("MSYS2_PREFIX"):
        candidates.insert(0, Path(os.environ["MSYS2_PREFIX"]) / "bin")

    for base in candidates:
        cc = base / (compiler_name + ".exe")
        cxx = base / (compiler_cxx_name + ".exe")
        if cc.exists() and cxx.exists():
            return str(cc), str(cxx), str(base)
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
# Compiler / generator detection
# ---------------------------------------------------------------------------
def resolve_compiler(family):
    """Return (family, cc_path, cxx_path). Raises SystemExit on failure."""
    if family == "msvc":
        if not load_msvc_environment():
            print(
                f"{C_WARN}[WARN]{C_RESET} vcvars64 environment not loaded; "
                "falling back to whatever is in PATH"
            )
        cl_path = find_in_path("cl")
        if not cl_path:
            print(f"{C_ERR}[ERROR] MSVC requested but cl.exe not found in PATH.{C_RESET}")
            print("    Install Visual Studio C++ workload, then retry "
                  "(build.py will load vcvars64 automatically).")
            print("        python build.py --compiler msvc")
            sys.exit(1)
        print(f"{C_OK}[OK]{C_RESET} MSVC cl.exe: {cl_path}")
        return "msvc", cl_path, cl_path

    if family == "gcc":
        cc, cxx = find_in_path("gcc"), find_in_path("g++")
        if not (cc and cxx):
            cc, cxx, msys_bin = find_msys2_toolchain("gcc", "g++")
            if cc and msys_bin:
                os.environ["PATH"] = msys_bin + os.pathsep + os.environ.get("PATH", "")
        if not (cc and cxx):
            print(f"{C_ERR}[ERROR] GCC/G++ not found in PATH.{C_RESET}")
            sys.exit(1)
        print(f"{C_OK}[OK]{C_RESET} GCC: {cc}")
        return "gcc", cc, cxx

    if family == "clang":
        cc, cxx = find_in_path("clang"), find_in_path("clang++")
        if not (cc and cxx):
            cc, cxx, msys_bin = find_msys2_toolchain("clang", "clang++")
            if cc and msys_bin:
                os.environ["PATH"] = msys_bin + os.pathsep + os.environ.get("PATH", "")
        if not (cc and cxx):
            print(f"{C_ERR}[ERROR] Clang/Clang++ not found in PATH.{C_RESET}")
            sys.exit(1)
        print(f"{C_OK}[OK]{C_RESET} Clang: {cc}")
        return "clang", cc, cxx

    # auto: gcc -> MSYS2 MinGW -> clang -> MSVC
    cc, cxx = find_in_path("gcc"), find_in_path("g++")
    if cc and cxx:
        print(f"{C_OK}[OK]{C_RESET} Found gcc in PATH: {cc}")
        return "gcc", cc, cxx
    cc, cxx, msys_bin = find_msys2_toolchain("gcc", "g++")
    if cc and msys_bin:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Found MSYS2 MinGW: {msys_bin}")
        os.environ["PATH"] = msys_bin + os.pathsep + os.environ.get("PATH", "")
        return "gcc", cc, cxx
    cc, cxx = find_in_path("clang"), find_in_path("clang++")
    if cc and cxx:
        print(f"{C_OK}[OK]{C_RESET} Found clang in PATH: {cc}")
        return "clang", cc, cxx
    cl_path = find_in_path("cl")
    if cl_path:
        print(f"{C_OK}[OK]{C_RESET} Found MSVC cl.exe in PATH: {cl_path}")
        return "msvc", cl_path, cl_path

    tips = (
        "No supported compiler found. Install one of:\n"
        "  * MSYS2 UCRT64 MinGW-w64 (recommended on Windows)\n"
        "  * Visual Studio C++ (then run from a Developer prompt)\n"
        "  * Clang / GCC on Linux/macOS"
    )
    print(f"\n{C_ERR}[ERROR] No supported compiler found in PATH.{C_RESET}\n\n{tips}")
    sys.exit(1)


def pick_generator(compiler_family, requested):
    """Return (generator_name, ninja_path). None = let CMake default."""
    ninja = find_in_path("ninja")
    if requested == "ninja":
        if not ninja:
            print(f"{C_ERR}[ERROR] --generator ninja requested but ninja not found.{C_RESET}")
            sys.exit(1)
        return "Ninja", ninja
    if requested == "make":
        return None, None

    if ninja:
        print(f"{C_OK}[OK]{C_RESET} ninja: {ninja}")
        return "Ninja", ninja

    if compiler_family == "msvc":
        print(
            f"{C_ERR}[ERROR] ninja not found; MSVC requires Ninja for a "
            "solution-free build.{C_RESET}"
        )
        print("    Install ninja (e.g. pacman -S mingw-w64-ucrt-x86_64-ninja)")
        sys.exit(1)
    print(f"{C_WARN}[WARN] ninja not found, falling back to CMake default generator.{C_RESET}")
    return None, None


# ---------------------------------------------------------------------------
# Build logic
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Gryce Engine build script -- wrapper around cmake + ninja "
                    "(pure CMake, no Visual Studio solution)"
    )
    parser.add_argument(
        "config", nargs="?", default="Debug",
        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
        help="CMake build configuration (default: Debug)"
    )
    parser.add_argument(
        "--compiler", default="auto",
        choices=["auto", "gcc", "clang", "msvc"],
        help="Compiler family to use (default: auto-detect)"
    )
    parser.add_argument(
        "--msvc", action="store_true",
        help="Deprecated alias for --compiler msvc"
    )
    parser.add_argument(
        "--generator", default="auto",
        choices=["auto", "ninja", "make"],
        help="CMake generator family (default: auto, prefers Ninja)"
    )
    parser.add_argument(
        "--setup-deps", action="store_true",
        help="Only download and extract dependencies, do not build"
    )
    parser.add_argument(
        "--configure", action="store_true",
        help="Only run CMake configure, do not build"
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
        help="Pass --verbose to the build tool"
    )
    parser.add_argument(
        "--jobs", "-j", type=int, default=0,
        help="Number of parallel jobs (default: auto)"
    )
    parser.add_argument(
        "--build-dir", default="build",
        help="Build directory prefix (default: build; each config uses build/<Config>)"
    )
    parser.add_argument(
        "--no-lock", action="store_true",
        help="Do NOT lock the compiler explicitly (use CMake default detection)"
    )
    parser.add_argument(
        "--editor", action="store_true",
        help="Windows: also build the WPF Editor (C#/dotnet) via CMake target"
    )
    parser.add_argument(
        "--offline", action="store_true",
        help="Skip network downloads; use only local cached dependencies"
    )
    args = parser.parse_args()

    config = args.config
    # 纯 CMake 流程统一单配置目录 build/<Config>（Ninja/Make 均为单配置 generator）。
    build_dir = Path(args.build_dir) / config
    project_root = Path(__file__).parent.resolve()

    if args.msvc:
        compiler = "msvc"
    else:
        compiler = args.compiler

    # -----------------------------------------------------------------------
    # 0. Setup deps only mode
    # -----------------------------------------------------------------------
    if args.setup_deps:
        ensure_deps(offline=args.offline)
        print(f"{C_OK}[Gryce Engine]{C_RESET} Dependencies ready.")
        sys.exit(0)

    print(f"{C_INFO}[Gryce Engine]{C_RESET} Build configuration: {C_OK}{config}{C_RESET}")

    # -----------------------------------------------------------------------
    # 1. Resolve compiler
    # -----------------------------------------------------------------------
    if args.no_lock:
        compiler_family = "auto"
        cc_path = cxx_path = None
        print(f"{C_INFO}[Gryce Engine]{C_RESET} --no-lock: using CMake default compiler detection")
    else:
        compiler_family, cc_path, cxx_path = resolve_compiler(compiler)

    # -----------------------------------------------------------------------
    # 2. Detect cmake, pick generator
    # -----------------------------------------------------------------------
    cmake = find_in_path("cmake")
    if not cmake:
        print(f"{C_ERR}[ERROR] cmake not found.{C_RESET}")
        print("Install: pacman -S mingw-w64-ucrt-x86_64-cmake")
        sys.exit(1)

    generator, ninja = pick_generator(compiler_family, args.generator)
    print(f"{C_OK}[OK]{C_RESET} cmake: {cmake}")

    # -----------------------------------------------------------------------
    # 3. Clean if requested (先于 ensure_deps，避免清完又立刻重新下载)
    # -----------------------------------------------------------------------
    if args.clean_all:
        if build_dir.exists():
            clean_build_artifacts(build_dir)
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
        # 归一化（小写 + 正斜杠），避免 Windows 盘符/大小写差异导致每次都重配。
        content = cache.read_text(encoding='utf-8', errors='ignore').lower().replace('\\', '/')
        if generator:
            if f"cmake_generator:internal={generator.lower()}" not in content:
                return True
        if not args.no_lock and compiler_family != "auto":
            cxx_lower = cxx_path.lower().replace('\\', '/')
            if (f"cmake_cxx_compiler:filepath={cxx_lower}" not in content and
                    f"cmake_cxx_compiler:uninitialized={cxx_lower}" not in content):
                return True
        return False

    if needs_reconfigure():
        if build_dir.exists():
            print(f"{C_WARN}[Gryce Engine]{C_RESET} {build_dir} exists but cache mismatch, reconfiguring ...")
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Configuring with CMake ...")
        configure_cmd = [
            cmake, "-B", str(build_dir),
            "-DCMAKE_BUILD_TYPE=" + config,
        ]
        if generator:
            configure_cmd += ["-G", generator]

        if not args.no_lock and compiler_family in ("gcc", "clang") and cc_path and cxx_path:
            configure_cmd += [
                "-DCMAKE_C_COMPILER=" + cc_path,
                "-DCMAKE_CXX_COMPILER=" + cxx_path,
            ]
        elif not args.no_lock and compiler_family == "msvc" and cc_path:
            configure_cmd += [
                "-DCMAKE_C_COMPILER=" + cc_path,
                "-DCMAKE_CXX_COMPILER=" + cxx_path,
            ]

        if args.editor:
            configure_cmd += ["-DGRYCE_BUILD_EDITOR=ON"]
            if IS_WINDOWS:
                print(
                    f"{C_INFO}[Gryce Engine]{C_RESET} --editor: WPF Editor will be built "
                    "by the CMake 'GryceEditor' target via dotnet"
                )
            else:
                print(
                    f"{C_WARN}[Gryce Engine]{C_RESET} --editor ignored: WPF Editor is "
                    "Windows-only"
                )

        configure_cmd += [str(project_root)]
        ok, output = run(configure_cmd, check=False)
        if not ok:
            print(f"{C_ERR}[ERROR] CMake configuration failed:{C_RESET}")
            print(output)
            # 清理失败的缓存，下次运行会重新配置而不是复用坏缓存。
            cache_file = build_dir / "CMakeCache.txt"
            if cache_file.exists():
                try:
                    cache_file.unlink()
                except OSError:
                    pass
            sys.exit(1)
        print(f"{C_OK}[OK]{C_RESET} Configuration complete.")
    else:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Using existing configuration: {build_dir}")

    if args.configure:
        print(f"{C_OK}[Gryce Engine]{C_RESET} Configure-only mode; build skipped.")
        sys.exit(0)

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
    if args.editor and IS_WINDOWS:
        print(f"  Editor:   editor/bin/{config}/net48/GryceEngine.Editor.exe")


if __name__ == "__main__":
    main()
