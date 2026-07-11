#!/usr/bin/env python3
"""Gryce Engine — 一键构建脚本（Windows + MinGW-w64）"""

import argparse
import shutil
import subprocess
import sys
import os
from pathlib import Path


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
def run(cmd, cwd=None, check=False):
    """Run a command, return (ok, stdout+stderr)."""
    try:
        result = subprocess.run(
            cmd, cwd=cwd, capture_output=True, text=True, shell=(os.name == 'nt')
        )
        output = (result.stdout or '') + (result.stderr or '')
        if check and result.returncode != 0:
            print(f"{C_ERR}[ERROR] Command failed:{C_RESET}")
            print(f"  {' '.join(cmd)}")
            print(output)
            sys.exit(1)
        return result.returncode == 0, output
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
    # Also check MSYS2_PREFIX env var
    if os.environ.get("MSYS2_PREFIX"):
        candidates.insert(0, Path(os.environ["MSYS2_PREFIX"]) / "bin")

    for base in candidates:
        gcc = base / "gcc.exe"
        gxx = base / "g++.exe"
        if gcc.exists() and gxx.exists():
            return str(gcc), str(gxx), str(base)
    return None, None, None


# ---------------------------------------------------------------------------
# Build logic
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Gryce Engine build script — wrapper around cmake + ninja"
    )
    parser.add_argument(
        "config", nargs="?", default="Debug",
        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
        help="CMake build configuration (default: Debug)"
    )
    parser.add_argument(
        "--clean", action="store_true",
        help="Remove build directory and reconfigure from scratch"
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
        help="Do NOT auto-lock MinGW compiler (use system default)"
    )
    args = parser.parse_args()

    config = args.config
    build_dir = Path(args.build_dir) / config
    project_root = Path(__file__).parent.resolve()

    print(f"{C_INFO}[Gryce Engine]{C_RESET} Build configuration: {C_OK}{config}{C_RESET}")

    # -----------------------------------------------------------------------
    # 1. Detect compiler
    # -----------------------------------------------------------------------
    gcc_path = find_in_path("gcc")
    gxx_path = find_in_path("g++")
    msys_bin = None

    if not gcc_path or not gxx_path:
        gcc_path, gxx_path, msys_bin = find_msys2_mingw()
        if gcc_path and msys_bin:
            print(f"{C_INFO}[Gryce Engine]{C_RESET} Found MSYS2 MinGW: {msys_bin}")
            # Temporarily prepend to PATH so that cmake/ninja can find it
            os.environ["PATH"] = msys_bin + os.pathsep + os.environ.get("PATH", "")
    else:
        print(f"{C_OK}[OK]{C_RESET} Found gcc in PATH: {gcc_path}")

    if not gcc_path:
        print(f"""
{C_ERR}[ERROR] gcc not found in PATH.{C_RESET}

This project is primarily built with MSYS2 UCRT64 MinGW-w64.

Install (MSYS2 UCRT64 terminal):
    pacman -S mingw-w64-ucrt-x86_64-gcc \
              mingw-w64-ucrt-x86_64-cmake \
              mingw-w64-ucrt-x86_64-ninja \
              mingw-w64-ucrt-x86_64-glew \
              mingw-w64-ucrt-x86_64-glfw

Then either:
    1. Run this script from the MSYS2 UCRT64 terminal.
    2. Add C:\\msys64\\ucrt64\\bin to your system PATH and retry.
    3. Use MSVC Developer Prompt and run cmake directly:
       cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
       cmake --build build
""")
        sys.exit(1)

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
    # 3. Clean if requested
    # -----------------------------------------------------------------------
    if args.clean and build_dir.exists():
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Cleaning {build_dir} ...")
        import shutil as _shutil
        _shutil.rmtree(build_dir)

    # -----------------------------------------------------------------------
    # 4. Configure
    # -----------------------------------------------------------------------
    # Only trust build.ninja as the marker of a valid configuration.
    # A stale CMakeCache.txt (from a failed MSVC run) must not skip configure.
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

        if not args.no_lock:
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
    # 5. Build
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
