#!/usr/bin/env python3
"""Gryce Engine — 外部依赖下载管理器

所有外部依赖统一下载到 build/deps/ 下，供 CMake add_subdirectory 使用。
CMake 侧不再使用 FetchContent，改为直接检测 build/deps/<name>/ 是否存在。

用法:
    python tools/deps_manager.py [命令] [选项]

命令:
    download    下载/更新所有外部依赖（默认）
    status      显示依赖状态
    clean       删除已解压的依赖，保留下载缓存
    clean-all   删除所有依赖和缓存
"""

import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import tempfile
import time
import urllib.request
from pathlib import Path

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
def _color_support():
    if os.name == 'nt' and 'ANSICON' not in os.environ:
        return False
    return True

if _color_support():
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
DEPENDENCIES = [
    {
        "name": "glfw",
        "url": "https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz",
        "filename": "glfw-3.4.tar.gz",
        "sha256": None,
        "extracted_dir": "glfw-3.4",
        "required": True,
    },
    {
        "name": "assimp",
        "url": "https://github.com/assimp/assimp/archive/refs/tags/v5.4.3.tar.gz",
        "filename": "assimp-v5.4.3.tar.gz",
        "sha256": "66dfbaee288f2bc43172440a55d0235dfc7bf885dda6435c038e8000e79582cb",
        "extracted_dir": "assimp-5.4.3",
        "required": True,
    },
    {
        "name": "box2d",
        "url": "https://github.com/erincatto/box2d/archive/refs/tags/v3.0.0.tar.gz",
        "filename": "box2d-v3.0.0.tar.gz",
        "sha256": None,
        "extracted_dir": "box2d-3.0.0",
        "required": False,
    },
    {
        "name": "jolt",
        "url": "https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.2.0.tar.gz",
        "filename": "jolt-v5.2.0.tar.gz",
        "sha256": None,
        "extracted_dir": "JoltPhysics-5.2.0",
        "required": False,
    },
]

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
def project_root() -> Path:
    """返回项目根目录（本文件在 tools/ 下）"""
    return Path(__file__).parent.parent.resolve()


def deps_root() -> Path:
    """返回依赖根目录：build/deps/"""
    return project_root() / "build" / "deps"


def cache_dir() -> Path:
    """返回下载缓存目录：build/deps/.download_cache/"""
    return deps_root() / ".download_cache"

# ---------------------------------------------------------------------------
# Download helpers
# ---------------------------------------------------------------------------
def _download_simple(url: str, dest: Path, description: str = ""):
    """Fallback download with manual progress bar."""
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as response:
        total = int(response.headers.get("Content-Length", 0))
        downloaded = 0
        chunk_size = 65536
        start_time = time.time()
        desc = description or dest.name

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


def _download_rich(url: str, dest: Path, description: str = ""):
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


def download_file(url: str, dest: Path, description: str = "") -> bool:
    """Download a file with progress bar."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        print(f"{C_INFO}[deps]{C_RESET} Downloading {description} from {url} ...")
        _download_rich(url, dest, description)
        return True
    except ImportError:
        print(f"{C_WARN}[WARN]{C_RESET} rich not installed, using manual progress bar")
        try:
            _download_simple(url, dest, description)
            return True
        except Exception as e:
            print(f"{C_ERR}[ERROR]{C_RESET} Download failed: {e}")
            if dest.exists():
                dest.unlink()
            return False
    except Exception as e:
        print(f"{C_ERR}[ERROR]{C_RESET} Download failed: {e}")
        if dest.exists():
            dest.unlink()
        return False


def verify_sha256(path: Path, expected: str) -> bool:
    """Verify SHA256 of a file."""
    if not expected:
        return True
    with open(path, "rb") as f:
        actual = hashlib.sha256(f.read()).hexdigest()
    return actual.lower() == expected.lower()


def extract_tarball(tarball: Path, dest: Path, extracted_dir: str) -> bool:
    """Extract a tarball to dest, renaming the top-level directory to the dep name."""
    dest.mkdir(parents=True, exist_ok=True)
    tmpdir = Path(tempfile.gettempdir()) / f"gryce_extract_{os.getpid()}"
    
    if tmpdir.exists():
        shutil.rmtree(tmpdir)
    tmpdir.mkdir(parents=True, exist_ok=True)

    try:
        with tarfile.open(tarball, "r:gz") as tar:
            # GitHub 发布的 tar.gz 是可信任源，使用 fully_trusted 避免警告
            tar.extractall(path=tmpdir, filter='fully_trusted')
        
        # Find the extracted top-level directory
        extracted = tmpdir / extracted_dir
        if not extracted.exists():
            # Sometimes GitHub archives use different names; try to find any single directory
            dirs = [d for d in tmpdir.iterdir() if d.is_dir()]
            if len(dirs) == 1:
                extracted = dirs[0]
            else:
                print(f"{C_ERR}[ERROR]{C_RESET} Could not find extracted directory '{extracted_dir}' in tarball")
                return False
        
        # Remove existing dest if it exists
        if dest.exists():
            shutil.rmtree(dest)
        
        shutil.move(str(extracted), str(dest))
        return True
    except Exception as e:
        print(f"{C_ERR}[ERROR]{C_RESET} Extraction failed: {e}")
        return False
    finally:
        if tmpdir.exists():
            shutil.rmtree(tmpdir)

# ---------------------------------------------------------------------------
# Dependency management
# ---------------------------------------------------------------------------
def ensure_dependency(dep: dict) -> bool:
    """Download and extract a single dependency if needed."""
    name = dep["name"]
    url = dep["url"]
    filename = dep["filename"]
    sha256 = dep["sha256"]
    extracted_dir = dep["extracted_dir"]
    required = dep["required"]
    
    dep_dir = deps_root() / name
    cache_file = cache_dir() / filename
    
    # Already extracted?
    if dep_dir.exists() and (dep_dir / "CMakeLists.txt").exists():
        print(f"{C_OK}[OK]{C_RESET} {name}: already present at {dep_dir}")
        return True
    
    # Download if not cached
    if cache_file.exists() and sha256 and verify_sha256(cache_file, sha256):
        print(f"{C_OK}[OK]{C_RESET} {name}: cached tarball verified")
    elif cache_file.exists() and not sha256:
        print(f"{C_OK}[OK]{C_RESET} {name}: cached tarball (no SHA256)")
    else:
        if cache_file.exists() and sha256 and not verify_sha256(cache_file, sha256):
            print(f"{C_WARN}[WARN]{C_RESET} {name}: SHA256 mismatch, re-downloading...")
            cache_file.unlink()
        
        if not download_file(url, cache_file, f"Downloading {name}"):
            if required:
                print(f"{C_ERR}[ERROR]{C_RESET} Failed to download required dependency: {name}")
                return False
            else:
                print(f"{C_WARN}[WARN]{C_RESET} Failed to download optional dependency: {name}")
                return False
        
        # Verify after download
        if sha256 and not verify_sha256(cache_file, sha256):
            print(f"{C_ERR}[ERROR]{C_RESET} {name}: SHA256 mismatch after download!")
            cache_file.unlink()
            return False
    
    # Extract
    print(f"{C_INFO}[deps]{C_RESET} Extracting {name} ...")
    if extract_tarball(cache_file, dep_dir, extracted_dir):
        print(f"{C_OK}[OK]{C_RESET} {name}: extracted to {dep_dir}")
        return True
    else:
        print(f"{C_ERR}[ERROR]{C_RESET} {name}: extraction failed")
        return False


def download_all() -> bool:
    """Download and extract all dependencies."""
    print(f"{C_INFO}[deps]{C_RESET} Dependency root: {deps_root()}")
    print(f"{C_INFO}[deps]{C_RESET} Cache directory: {cache_dir()}")
    
    deps_root().mkdir(parents=True, exist_ok=True)
    cache_dir().mkdir(parents=True, exist_ok=True)
    
    all_ok = True
    for dep in DEPENDENCIES:
        if not ensure_dependency(dep):
            if dep["required"]:
                all_ok = False
    
    return all_ok


def status():
    """Print dependency status table."""
    print(f"{C_INFO}Dependency Status{C_RESET}")
    print(f"{'Name':<12} {'Required':<10} {'Status':<10} {'Path'}")
    print("-" * 70)
    for dep in DEPENDENCIES:
        name = dep["name"]
        dep_dir = deps_root() / name
        if dep_dir.exists() and (dep_dir / "CMakeLists.txt").exists():
            status_str = f"{C_OK}OK{C_RESET}"
        else:
            status_str = f"{C_WARN}MISSING{C_RESET}"
        req_str = "yes" if dep["required"] else "no"
        print(f"{name:<12} {req_str:<10} {status_str:<10} {dep_dir}")


def clean(keep_cache: bool = True):
    """Remove extracted dependencies."""
    for dep in DEPENDENCIES:
        dep_dir = deps_root() / dep["name"]
        if dep_dir.exists():
            print(f"{C_INFO}[deps]{C_RESET} Removing {dep_dir} ...")
            shutil.rmtree(dep_dir)
    
    if not keep_cache:
        if cache_dir().exists():
            print(f"{C_INFO}[deps]{C_RESET} Removing {cache_dir()} ...")
            shutil.rmtree(cache_dir())
    
    print(f"{C_OK}[OK]{C_RESET} Clean complete.")

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Gryce Engine dependency manager")
    parser.add_argument(
        "command", nargs="?", default="download",
        choices=["download", "status", "clean", "clean-all"],
        help="Command to run (default: download)"
    )
    args = parser.parse_args()
    
    if args.command == "download":
        ok = download_all()
        sys.exit(0 if ok else 1)
    elif args.command == "status":
        status()
    elif args.command == "clean":
        clean(keep_cache=True)
    elif args.command == "clean-all":
        clean(keep_cache=False)


if __name__ == "__main__":
    main()
