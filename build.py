#!/usr/bin/env python3
"""Gryce Engine -- one-click build script (Windows, MinGW-w64 / MSVC)

用法:
    python build.py [config] [选项]

示例:
    python build.py                    # 编译 Debug
    python build.py Release            # 编译 Release
    python build.py --setup-deps       # 仅下载依赖
    python build.py --clean            # 清理构建产物（保留 deps）
    python build.py --clean-all        # 完全清理（包括 deps）
"""

import argparse
import shutil
import subprocess
import sys
import os
from pathlib import Path

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
    """Remove build artifacts but optionally preserve deps/ cache."""
    bd = Path(build_dir)
    if not bd.exists():
        return

    if not keep_deps:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Cleaning {bd} (including deps) ...")
        shutil.rmtree(bd)
        return

    print(f"{C_INFO}[Gryce Engine]{C_RESET} Cleaning {bd} (preserving deps cache) ...")
    deps_dir = bd / "deps"
    if deps_dir.exists():
        import tempfile
        tmp = Path(tempfile.gettempdir()) / f"gryce_deps_{os.getpid()}"
        shutil.move(str(deps_dir), str(tmp))
        shutil.rmtree(bd)
        bd.mkdir(parents=True, exist_ok=True)
        shutil.move(str(tmp), str(deps_dir))
        print(f"{C_OK}[OK]{C_RESET} Preserved dependency cache: {deps_dir}")
    else:
        shutil.rmtree(bd)


def ensure_deps():
    """Ensure all dependencies are downloaded via deps_manager.py."""
    deps_script = Path(__file__).parent / "tools" / "deps_manager.py"
    if not deps_script.exists():
        print(f"{C_ERR}[ERROR]{C_RESET} deps_manager.py not found at {deps_script}")
        sys.exit(1)

    print(f"{C_INFO}[Gryce Engine]{C_RESET} Checking dependencies ...")
    ok, output = run([sys.executable, str(deps_script), "download"], check=False)
    if not ok:
        print(f"{C_ERR}[ERROR]{C_RESET} Dependency download failed:")
        print(output)
        sys.exit(1)


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
    args = parser.parse_args()

    config = args.config
    build_dir = Path(args.build_dir) / config
    project_root = Path(__file__).parent.resolve()

    # -----------------------------------------------------------------------
    # 0. Setup deps only mode
    # -----------------------------------------------------------------------
    if args.setup_deps:
        ensure_deps()
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

    if not args.no_lock:
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
            print(
                f"\n{C_ERR}[ERROR] No supported compiler found in PATH.{C_RESET}\n\n"
                "This project supports:\n"
                "  * MSYS2 UCRT64 MinGW-w64 (recommended)\n"
                "  * MSVC (Visual Studio 2022+)\n\n"
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
                '    Open "x64 Native Tools Command Prompt for VS 2022" and run:\n'
                "        python build.py\n"
            )
            sys.exit(1)
    else:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} --no-lock: using CMake default compiler detection")
        compiler_family = "auto"

    # -----------------------------------------------------------------------
    # 2. Detect cmake and ninja
    # -----------------------------------------------------------------------
    cmake = find_in_path("cmake")
    ninja = find_in_path("ninja")

    if not cmake:
        print(f"{C_ERR}[ERROR] cmake not found.{C_RESET}")
        print(f"Install: pacman -S mingw-w64-ucrt-x86_64-cmake")
        sys.exit(1)
    if not ninja:
        print(f"{C_WARN}[WARN] ninja not found, falling back to default generator.{C_RESET}")
        generator = None
    else:
        generator = "Ninja"

    print(f"{C_OK}[OK]{C_RESET} cmake: {cmake}")
    if ninja:
        print(f"{C_OK}[OK]{C_RESET} ninja: {ninja}")

    # -----------------------------------------------------------------------
    # 3. Ensure dependencies
    # -----------------------------------------------------------------------
    ensure_deps()

    # -----------------------------------------------------------------------
    # 4. Clean if requested
    # -----------------------------------------------------------------------
    if args.clean_all and build_dir.exists():
        clean_build_artifacts(build_dir, keep_deps=False)
    elif args.clean and build_dir.exists():
        clean_build_artifacts(build_dir, keep_deps=True)

    # -----------------------------------------------------------------------
    # 5. Configure
    # -----------------------------------------------------------------------
    if not (build_dir / "build.ninja").exists():
        if build_dir.exists():
            print(f"{C_WARN}[Gryce Engine]{C_RESET} {build_dir} exists but has no build.ninja, reconfiguring ...")
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


if __name__ == "__main__":
    main()
