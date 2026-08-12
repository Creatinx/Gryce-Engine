#!/usr/bin/env python3
"""GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).

Assembles a standalone game directory:
  * the template executable (GryceGame),
  * the core runtime DLLs,
  * the game's res:// content packaged into one or more .gpkg archives
    (GPAK format) instead of a raw res/ directory copy.

The archives are produced by GryceCore's GPackWriter through its C API
(GCore_PackCreate / GCore_PackAddFile / GCore_PackWrite), so the on-disk
layout always stays in sync with the reader used by the runtime
(GPackReader, mounted automatically by GCore_Init for every *.gpkg/*.gpack
in the project root).

Usage:
    python tools/grycegc.py --project examples/3dtest --name MyGame \
        --build-dir build --config Release --out build/game

The packaged game is run with:
    build/game/MyGame/MyGame.exe --project build/game/MyGame
"""

import argparse
import ctypes
import os
import shutil
import struct
import sys


# ---------------------------------------------------------------------------
# Resource categories: each category becomes its own .gpkg (multiple archives
# are supported by design). `misc` catches everything else.
# ---------------------------------------------------------------------------
CATEGORY_EXTENSIONS = {
    "scenes": {".gesc", ".scene", ".tscn"},
    "scripts": {".lua"},
    "shaders": {".vert", ".frag", ".geom", ".tesc", ".tese", ".comp",
                ".glsl", ".hlsl", ".spv"},
    "models": {".obj", ".fbx", ".gltf", ".glb", ".dae", ".ply", ".stl",
               ".3ds", ".blend"},
    "textures": {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".ktx",
                 ".hdr", ".exr", ".gif", ".webp"},
    "audio": {".wav", ".ogg", ".mp3", ".flac", ".aac"},
    "fonts": {".ttf", ".otf", ".fnt", ".woff", ".woff2"},
    "config": {".json", ".gryce", ".cfg", ".ini", ".toml", ".yaml", ".yml",
               ".mat", ".txt"},
}

# Files / directories that never belong in a resource archive.
SKIP_DIRS = {".git", ".vs", ".idea", "__pycache__", "build", "bin", "obj",
             "x64", "out"}
SKIP_EXTS = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".inl",
             ".py", ".md", ".sln", ".pdb", ".ilk", ".exp", ".lib", ".dll",
             ".exe"}
SKIP_NAMES = {"cmakelists.txt", ".gitignore", ".gitattributes", "license",
              "readme.md"}


def classify(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    for category, exts in CATEGORY_EXTENSIONS.items():
        if ext in exts:
            return category
    return "misc"


def collect_project_files(project_root: str):
    """Yield (internal_path, source_path) for every packable project file."""
    root = os.path.abspath(project_root)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            if name.lower() in SKIP_NAMES:
                continue
            if os.path.splitext(name)[1].lower() in SKIP_EXTS:
                continue
            source = os.path.join(dirpath, name)
            rel = os.path.relpath(source, root).replace("\\", "/")
            yield rel, source


def load_core_packer(bin_dir: str, config: str):
    """Load GryceCore and bind the GPack C API via ctypes."""
    bin_dir = os.path.abspath(bin_dir)
    dll_name = "GryceCored.dll" if config == "Debug" else "GryceCore.dll"
    if not os.path.isfile(os.path.join(bin_dir, dll_name)):
        # MinGW builds name the DLL libGryceCore(d).dll
        dll_name = f"lib{dll_name}"
    dll_path = os.path.join(bin_dir, dll_name)
    if not os.path.isfile(dll_path):
        raise FileNotFoundError(
            f"core DLL not found: {dll_path} (build the GryceCore target first)")

    # Windows: let dependent DLLs (glfw, assimp, ...) resolve from bin_dir.
    if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
        os.add_dll_directory(bin_dir)

    core = ctypes.CDLL(dll_path)
    core.GCore_PackCreate.restype = ctypes.c_void_p
    core.GCore_PackCreate.argtypes = []
    core.GCore_PackAddFile.restype = ctypes.c_int
    core.GCore_PackAddFile.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    core.GCore_PackWrite.restype = ctypes.c_int
    core.GCore_PackWrite.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    core.GCore_PackDestroy.restype = None
    core.GCore_PackDestroy.argtypes = [ctypes.c_void_p]
    return core, dll_path


def write_bundle(core, internal_files, output_path):
    """Write one .gpkg through GryceCore's GPackWriter."""
    handle = core.GCore_PackCreate()
    if not handle:
        raise RuntimeError(f"GCore_PackCreate failed for {output_path}")
    try:
        for internal_path, source_path in internal_files:
            rc = core.GCore_PackAddFile(
                handle,
                internal_path.encode("utf-8"),
                os.fspath(source_path).encode("utf-8"),
            )
            if rc != 0:
                raise RuntimeError(
                    f"GCore_PackAddFile('{internal_path}') failed (rc={rc})")
        rc = core.GCore_PackWrite(handle, os.fspath(output_path).encode("utf-8"))
        if rc != 0:
            raise RuntimeError(f"GCore_PackWrite('{output_path}') failed (rc={rc})")
    finally:
        core.GCore_PackDestroy(handle)


def verify_bundle(path: str) -> tuple:
    """Read-only sanity check of a GPAK header/entry table (not a writer)."""
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) != 12 or head[:4] != b"GPAK":
            raise RuntimeError(f"{path}: bad GPAK magic")
        version, count = struct.unpack("<II", head[4:])
        if version != 1:
            raise RuntimeError(f"{path}: unsupported version {version}")
        table_size = 0
        offsets = []
        for _ in range(count):
            (path_len,) = struct.unpack("<I", f.read(4))
            f.seek(path_len, os.SEEK_CUR)
            size, offset, _crc = struct.unpack("<QQI", f.read(20))
            table_size += 4 + path_len + 20
            offsets.append((offset, size))
        file_size = os.path.getsize(path)
        for offset, size in offsets:
            if offset < 12 + table_size or offset + size > file_size:
                raise RuntimeError(f"{path}: entry data range out of file")
    return count, file_size


def copy_runtime(bin_dir: str, config: str, out_dir: str, exe: str, name: str):
    suffix = "d" if config == "Debug" else ""
    dlls = [
        f"GryceCore{suffix}.dll",
        f"GryceRenderer{suffix}.dll",
        f"GrycePlatform{suffix}.dll",
        f"GrycePhysics{suffix}.dll",
    ]
    copied = []
    for dll in dlls:
        src = None
        for candidate in (dll, f"lib{dll}"):
            p = os.path.join(bin_dir, candidate)
            if os.path.isfile(p):
                src = p
                break
        if src is None:
            print(f"[grycegc] warning: {dll} not found in {bin_dir}")
            continue
        shutil.copy2(src, os.path.join(out_dir, os.path.basename(src)))
        copied.append(os.path.basename(src))
    # GLFW: Debug builds may use glfw3d.dll (MSVC) or plain glfw3.dll (MinGW).
    for glfw in ("glfw3d.dll", "glfw3.dll"):
        src = os.path.join(bin_dir, glfw)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(out_dir, glfw))
            copied.append(glfw)
            break
    # MinGW runtime DLLs (self-contained packages; harmless to include).
    for mingw_dll in ("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
        src = os.path.join(bin_dir, mingw_dll)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(out_dir, mingw_dll))
            copied.append(mingw_dll)
    dst_exe = os.path.join(out_dir, f"{name}.exe")
    shutil.copy2(exe, dst_exe)
    return copied, dst_exe


def main() -> int:
    parser = argparse.ArgumentParser(description="GryceGC - package a GryceEngine game")
    parser.add_argument("--project", required=True, help="game project directory (res:// root)")
    parser.add_argument("--name", default="MyGame", help="output game name")
    parser.add_argument("--build-dir", default="build", help="CMake build directory")
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"])
    parser.add_argument("--out", default="build/game", help="output parent directory")
    parser.add_argument("--single", action="store_true",
                        help="pack everything into one <name>.gpkg instead of category bundles")
    parser.add_argument("--core-dll", default=None,
                        help="override path to GryceCore DLL (default: <build-dir>/bin/<config>)")
    args = parser.parse_args()

    bin_dir = os.path.join(args.build_dir, "bin", args.config)
    exe = os.path.join(bin_dir, "GryceGame.exe")
    if not os.path.isfile(exe):
        print(f"[grycegc] ERROR: {exe} not found; build the GryceGame target first")
        return 1

    try:
        core, dll_path = load_core_packer(
            os.path.dirname(args.core_dll) if args.core_dll else bin_dir, args.config)
    except (OSError, FileNotFoundError) as exc:
        print(f"[grycegc] ERROR: {exc}")
        return 1

    out_dir = os.path.join(args.out, args.name)
    os.makedirs(out_dir, exist_ok=True)

    # 1) Runtime: exe + core DLLs (same layout as before, no res/ copy).
    copied, dst_exe = copy_runtime(bin_dir, args.config, out_dir, exe, args.name)

    # 2) Content: group project files into .gpkg archives.
    files = list(collect_project_files(args.project))
    if not files:
        print("[grycegc] ERROR: no packable resources found in project")
        return 1

    bundles = {}
    for internal_path, source_path in files:
        key = "all" if args.single else classify(internal_path)
        bundles.setdefault(key, []).append((internal_path, source_path))

    bundle_paths = []
    total_entries = 0
    for key, members in sorted(bundles.items()):
        suffix = "" if args.single else f".{key}"
        bundle_path = os.path.join(out_dir, f"{args.name}{suffix}.gpkg")
        try:
            write_bundle(core, members, bundle_path)
            count, file_size = verify_bundle(bundle_path)
            total_entries += count
            bundle_paths.append(bundle_path)
            print(f"[grycegc] {os.path.basename(bundle_path)}: {count} files, "
                  f"{file_size / (1024 * 1024):.2f} MiB")
        except (OSError, RuntimeError) as exc:
            print(f"[grycegc] ERROR: {exc}")
            return 1

    print(f"[grycegc] packaged {args.name} -> {out_dir}")
    print(f"[grycegc] exe + {len(copied)} runtime DLLs + {total_entries} resources "
          f"in {len(bundle_paths)} .gpkg archive(s)")
    print(f"[grycegc] run with: {dst_exe} --project {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
