#!/usr/bin/env python3
"""Gryce Engine -- one-click build script (Windows, MinGW-w64 / MSVC)"""

import argparse
import hashlib
import shutil
import subprocess
import sys
import os
import urllib.request
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
# Dependency definitions
# ---------------------------------------------------------------------------
DEPENDENCIES = {
    "glfw": {
        "url": "https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz",
        "filename": "glfw-3.4.tar.gz",
        "sha256": None,
        "required": False,
    },
    "assimp": {
        "url": "https://github.com/assimp/assimp/archive/refs/tags/v5.4.3.tar.gz",
        "filename": "assimp-v5.4.3.tar.gz",
        "sha256": "66dfbaee288f2bc43172440a55d0235dfc7bf885dda6435c038e8000e79582cb",
        "required": True,
    },
    "box2d": {
        "url": "https://github.com/erincatto/box2d/archive/refs/tags/v3.0.0.tar.gz",
        "filename": "box2d-v3.0.0.tar.gz",
        "sha256": None,
        "required": False,
    },
    "jolt": {
        "url": "https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.2.0.tar.gz",
        "filename": "jolt-v5.2.0.tar.gz",
        "sha256": None,
        "required": False,
    },
}


# ---------------------------------------------------------------------------
# Download helpers -- every download shows a progress bar
# ---------------------------------------------------------------------------
def _download_simple(url, dest, description=""):
    """Fallback download with manual percentage progress bar."""
    import time
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as response:
        total = int(response.headers.get("Content-Length", 0))
        downloaded = 0
        chunk_size = 65536
        start_time = time.time()
        desc = description or os.path.basename(dest)

        with open(dest, "wb") as f:
            while True:
                chunk = response.read(chunk_size)
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                elapsed = time.time() - start_time
                speed = downloaded / elapsed if elapsed > 0 else 0.0

                if total > 0:
                    pct = downloaded / total
                    bar_len = 30
                    filled = int(bar_len * pct)
                    bar = "#" * filled + "-" * (bar_len - filled)
                    eta = (total - downloaded) / speed if speed > 0 else 0
                    print(
                        f"\r  {desc} |{bar}| {pct*100:5.1f}% "
                        f"{downloaded/1024/1024:.1f}/{total/1024/1024:.1f} MB "
                        f"{speed/1024/1024:.1f} MB/s ETA {eta:.0f}s",
                        end="", flush=True,
                    )
                else:
                    print(f"\r  {desc} {downloaded/1024/1024:.1f} MB downloaded", end="", flush=True)
        print()


def _download_rich(url, dest, description=""):
    """Download with rich progress bar."""
    from rich.progress import (
        Progress, TextColumn, BarColumn, DownloadColumn, TransferSpeedColumn, TimeRemainingColumn
    )
    from rich.console import Console

    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as response:
        total = int(response.headers.get("Content-Length", 0))
        console = Console()

        with Progress(
            TextColumn(f"[bold blue]{description}"),
            BarColumn(bar_width=40),
            "[progress.percentage]{task.percentage:>3.0f}%",
            "*",
            DownloadColumn(binary_units=True),
            "*",
            TransferSpeedColumn(),
            "*",
            TimeRemainingColumn(),
            console=console,
        ) as progress:
            task = progress.add_task(description, total=total)
            with open(dest, "wb") as f:
                while True:
                    chunk = response.read(65536)
                    if not chunk:
                        break
                    f.write(chunk)
                    progress.update(task, advance=len(chunk))


def download_with_progress(url, dest, description=""):
    """Download a file with progress bar (rich if available, else manual)."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)

    try:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Downloading {description} from {url} ...")
        _download_rich(url, dest, description)
        return True
    except ImportError:
        print(f"{C_WARN}[WARN]{C_RESET} rich not installed, using manual progress bar (pip install rich for colors)")
        try:
            _download_simple(url, dest, description)
            return True
        except Exception as e:
            print(f"{C_ERR}[ERROR]{C_RESET} Download failed: {e}")
            if os.path.exists(dest):
                os.remove(dest)
            return False
    except Exception as e:
        print(f"{C_WARN}[WARN]{C_RESET} Download failed: {e}")
        if os.path.exists(dest):
            os.remove(dest)
        return False


def prefetch_dependencies(cache_dir):
    """Pre-download tar.gz dependencies to cache_dir before cmake runs."""
    cache_path = Path(cache_dir)
    cache_path.mkdir(parents=True, exist_ok=True)

    for name, info in DEPENDENCIES.items():
        dest = cache_path / info["filename"]
        if dest.exists():
            print(f"{C_OK}[OK]{C_RESET} {name}: cached {dest}")
            continue
        dest = cache_path / info["filename"]
        if dest.exists():
            if info["sha256"]:
                with open(dest, "rb") as f:
                    actual = hashlib.sha256(f.read()).hexdigest()
                if actual.lower() == info["sha256"].lower():
                    print(f"{C_OK}[OK]{C_RESET} {name}: cached {dest} (SHA256 verified)")
                    continue
                else:
                    print(f"{C_WARN}[WARN]{C_RESET} {name}: SHA256 mismatch, re-downloading...")
                    os.remove(dest)
            else:
                print(f"{C_OK}[OK]{C_RESET} {name}: cached {dest}")
                continue

        if not download_with_progress(info["url"], str(dest), f"Downloading {name}"):
            if info["required"]:
                print(f"{C_ERR}[ERROR]{C_RESET} Failed to download required dependency: {name}")
                sys.exit(1)
            else:
                print(f"{C_WARN}[WARN]{C_RESET} Failed to download optional dependency: {name}, cmake will try fallback")


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
            text=True, shell=(os.name == 'nt')
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
    """Remove build artifacts but optionally preserve _deps/ cache."""
    import shutil
    bd = Path(build_dir)
    if not bd.exists():
        return

    if not keep_deps:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Cleaning {bd} (including _deps) ...")
        shutil.rmtree(bd)
        return

    print(f"{C_INFO}[Gryce Engine]{C_RESET} Cleaning {bd} (preserving _deps cache) ...")
    # Preserve _deps/ directory; remove everything else
    deps_dir = bd / "_deps"
    if deps_dir.exists():
        # Move _deps to a temp location, rmtree build_dir, restore _deps
        import tempfile
        tmp = Path(tempfile.gettempdir()) / f"gryce_deps_{os.getpid()}"
        shutil.move(str(deps_dir), str(tmp))
        shutil.rmtree(bd)
        bd.mkdir(parents=True, exist_ok=True)
        shutil.move(str(tmp), str(deps_dir))
        print(f"{C_OK}[OK]{C_RESET} Preserved dependency cache: {deps_dir}")
    else:
        shutil.rmtree(bd)


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
        "--clean", action="store_true",
        help="Clean build artifacts but preserve FetchContent _deps/ cache"
    )
    parser.add_argument(
        "--clean-all", action="store_true",
        help="Remove entire build directory including _deps/ (forces re-download)"
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
        "--cache-dir", default="deps_cache",
        help="Directory to cache downloaded dependencies (default: deps_cache)"
    )
    parser.add_argument(
        "--no-prefetch", action="store_true",
        help="Skip pre-downloading dependencies to cache"
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
                "    pacman -S mingw-w64-ucrt-x86_64-gcc \\\n"
                "              mingw-w64-ucrt-x86_64-cmake \\\n"
                "              mingw-w64-ucrt-x86_64-ninja \\\n"
                "              mingw-w64-ucrt-x86_64-glew \\\n"
                "              mingw-w64-ucrt-x86_64-glfw\n\n"
                "Then either:\n"
                "    1. Run this script from the MSYS2 UCRT64 terminal.\n"
                "    2. Add C:\\\\msys64\\\\ucrt64\\\\bin to your system PATH and retry.\n\n"
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
    # 3. Pre-fetch dependencies
    # -----------------------------------------------------------------------
    if not args.no_prefetch:
        prefetch_dependencies(args.cache_dir)
    else:
        print(f"{C_INFO}[Gryce Engine]{C_RESET} Skipping dependency prefetch (--no-prefetch)")

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

        configure_cmd += [
            "-DGRYCE_CACHE_DIR=" + str(Path(args.cache_dir).resolve()),
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
