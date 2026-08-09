#!/usr/bin/env python3
"""GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).

Assembles a standalone game directory: the template executable (GryceGame),
the core runtime DLLs, and the game's res:// content.

Usage:
    python tools/grycegc.py --project examples/3dtest --name MyGame \
        --build-dir build --config Release --out build/game
"""

import argparse
import os
import shutil
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description="GryceGC - package a GryceEngine game")
    parser.add_argument("--project", required=True, help="game project directory (res:// root)")
    parser.add_argument("--name", default="MyGame", help="output game name")
    parser.add_argument("--build-dir", default="build", help="CMake build directory")
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"])
    parser.add_argument("--out", default="build/game", help="output parent directory")
    args = parser.parse_args()

    bin_dir = os.path.join(args.build_dir, "bin", args.config)
    exe = os.path.join(bin_dir, "GryceGame.exe")
    if not os.path.isfile(exe):
        print(f"[grycegc] ERROR: {exe} not found; build the GryceGame target first")
        return 1

    out_dir = os.path.join(args.out, args.name)
    os.makedirs(out_dir, exist_ok=True)

    # Core runtime DLLs (Debug builds use the 'd' suffix)
    suffix = "d" if args.config == "Debug" else ""
    dlls = [
        f"GryceCore{suffix}.dll",
        f"GryceRenderer{suffix}.dll",
        f"GrycePlatform{suffix}.dll",
        f"GrycePhysics{suffix}.dll",
        "glfw3.dll" if args.config == "Release" else "glfw3d.dll",
    ]
    copied = 0
    for dll in dlls:
        src = os.path.join(bin_dir, dll)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(out_dir, dll))
            copied += 1
        else:
            print(f"[grycegc] warning: {dll} not found in {bin_dir}")

    shutil.copy2(exe, os.path.join(out_dir, f"{args.name}.exe"))

    # Game content: res:// = project root
    res_dst = os.path.join(out_dir, "res")
    os.makedirs(res_dst, exist_ok=True)
    copied_items = 0
    for entry in sorted(os.listdir(args.project)):
        src = os.path.join(args.project, entry)
        dst = os.path.join(res_dst, entry)
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            shutil.copy2(src, dst)
        copied_items += 1

    print(f"[grycegc] packaged {args.name} -> {out_dir}")
    print(f"[grycegc] exe + {copied} runtime DLLs + {copied_items} project items")
    print(f"[grycegc] run with: {os.path.join(out_dir, args.name + '.exe')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
