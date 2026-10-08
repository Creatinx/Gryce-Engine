#!/usr/bin/env python3
"""
main.py — Gryce-Engine 工具链 CLI

把构建好的工程打包成一个**可脱机运行**的文件夹，并校验其自洽性。

子命令
------
  package      打包：exe + 引擎 core DLL（与 exe 同级）+ 依赖 DLL（dll/ 文件夹）
                + demo_project 资源 + 启动器 / README。
  verify       校验已有发布包：所有导入 DLL 是否都能在包内解析、core 是否同级。
  help         显示帮助（无参数运行也等价于 help）；help <命令> 显示该命令详情。

缩写/全写
---------
  无参数            -> 显示帮助（等同于 help）
  -h / --help       -> 显示帮助（argparse 原生，顶层与各子命令均可用）
  help              -> 显示总帮助
  help package      -> 显示 package 子命令的详细帮助

常用示例
--------
  # 用默认 build 目录打包到 dist/GryceEngine（默认 OpenGL 启动器）
  python toolchain/main.py package

  # 指定构建目录 / 可执行文件 / 包名，并打包后自动校验
  python toolchain/main.py package --build-dir cmake-build-debug \\
      --exe GryceRenderSmoke.exe --name GryceEngine --verify

  # 仅 Vulkan 后端，且不要资源
  python toolchain/main.py package --api vulkan --no-assets

  # 校验
  python toolchain/main.py verify --package dist/GryceEngine

依赖解析优先用 objdump（MinGW/CLion 自带），缺失时回退内置纯 Python PE 解析器。
"""

import argparse
import os
import sys

import resource as resource_mod


# ---------------------------------------------------------------------------
# objdump 探测
# ---------------------------------------------------------------------------

def find_objdump(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    # PATH 上找
    import shutil
    for name in ("objdump.exe", "objdump"):
        p = shutil.which(name)
        if p:
            return p
    # 常见 MinGW / CLion 位置
    import glob
    candidates = [
        "D:/msys64/mingw64/bin/objdump.exe",
        "D:/msys64/ucrt64/bin/objdump.exe",
        "C:/msys64/mingw64/bin/objdump.exe",
    ]
    for pat in ("D:/JetBrains/CLion*/bin/mingw/bin/objdump.exe",
                "C:/Program Files/JetBrains/CLion*/bin/mingw/bin/objdump.exe"):
        candidates.extend(glob.glob(pat))
    for c in candidates:
        if os.path.exists(c):
            return c
    return None


# ---------------------------------------------------------------------------
# 子命令实现
# ---------------------------------------------------------------------------

def cmd_package(args):
    objdump = find_objdump(args.objdump)
    build_dir = args.build_dir
    if not os.path.isdir(build_dir):
        print(f"[错误] 构建目录不存在：{build_dir}", file=sys.stderr)
        return 2

    packager = resource_mod.Packager(
        build_dir=build_dir,
        exe=args.exe,
        output=args.output,
        name=args.name,
        project_src=args.project,
        dll_folder=args.dll_folder,
        objdump=objdump,
        extra_dirs=args.extra_dir if args.extra_dir else [],
        no_assets=args.no_assets,
        api=args.api,
        clean=args.clean,
    )
    try:
        res = packager.run()
    except FileNotFoundError as e:
        print(f"[错误] {e}", file=sys.stderr)
        return 2

    print(f"[完成] 发布包已生成：{res['pkg_root']}")
    print(f"  可执行文件   : {res['exe']}")
    print(f"  core DLL(同级): {', '.join(res['core']) if res['core'] else '(无)'}")
    print(f"  依赖 DLL({args.dll_folder}/): {', '.join(res['dep']) if res['dep'] else '(无)'}")
    print(f"  资源 demo_project: {'已打包' if res['assets'] else '跳过'}")
    if res["missing"]:
        print(f"  [警告] 未找到的导入 DLL: {', '.join(res['missing'])}")

    if args.verify:
        ok, problems = resource_mod.verify_package(
            res["pkg_root"], dll_folder=args.dll_folder, objdump=objdump)
        if ok:
            print("[校验] 通过：所有导入 DLL 均可在包内解析。")
        else:
            print("[校验] 存在问题：")
            for p in problems:
                print(f"    - {p}")
            return 1
    return 0


def cmd_verify(args):
    if not os.path.isdir(args.package):
        print(f"[错误] 包目录不存在：{args.package}", file=sys.stderr)
        return 2
    objdump = find_objdump(args.objdump)
    ok, problems = resource_mod.verify_package(
        args.package, dll_folder=args.dll_folder, objdump=objdump)
    if ok:
        print(f"[校验] 通过：{args.package} 自洽。")
        return 0
    print(f"[校验] {args.package} 存在问题：")
    for p in problems:
        print(f"    - {p}")
    return 1


def cmd_list_deps(args):
    objdump = find_objdump(args.objdump)
    build_dir = args.build_dir
    if not os.path.isdir(build_dir):
        print(f"[错误] 构建目录不存在：{build_dir}", file=sys.stderr)
        return 2
    packager = resource_mod.Packager(
        build_dir=build_dir, exe=args.exe, output=".", name="__tmp__",
        objdump=objdump, extra_dirs=args.extra_dir if args.extra_dir else [],
    )
    exe_path = packager.resolve_exe()
    if not exe_path:
        print("[错误] 找不到可执行文件，请先用 --exe 指定。", file=sys.stderr)
        return 2
    extra = [os.path.dirname(exe_path)] + resource_mod.discover_mingw_bins()
    resolver = resource_mod.DepResolver(build_dir, objdump=objdump, extra_dirs=extra)
    found, missing = resolver.resolve(exe_path)
    print(f"可执行文件: {exe_path}\n")
    print("依赖闭包：")
    core, dep = [], []
    for low, src in found.items():
        bucket = resolver.classify(low)
        (core if bucket == "core" else dep).append((os.path.basename(src), src))
    print("  [core · 与 exe 同级]")
    for n, s in sorted(core):
        print(f"    {n}  <- {s}")
    print("  [dependency · 进 dll/ 文件夹]")
    for n, s in sorted(dep):
        print(f"    {n}  <- {s}")
    if missing:
        print("  [未找到]")
        for m in sorted(missing):
            print(f"    {m}")
    return 0


# ---------------------------------------------------------------------------
# 参数
# ---------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        prog="toolchain/main.py",
        description="Gryce-Engine 离线发布打包工具链",
    )
    # 注：-h / --help 由 argparse 原生提供（顶层与各子命令均可用），无需手动添加。
    sub = p.add_subparsers(dest="command", required=False)

    def add_common(sp):
        sp.add_argument("--build-dir", default="cmake-build-debug",
                        help="CMake 构建目录（默认 cmake-build-debug）")
        sp.add_argument("--exe", default=None,
                        help="可执行文件路径或文件名（默认自动探测 GryceRenderSmoke.exe）")
        sp.add_argument("--objdump", default=None,
                        help="objdump 路径（默认自动探测，缺失则用内置 PE 解析器）")
        sp.add_argument("--extra-dir", action="append", default=[],
                        help="额外 DLL 搜索目录（可重复）")

    sp = sub.add_parser("package", help="打包成可脱机运行的文件夹")
    add_common(sp)
    sp.add_argument("--output", default="dist", help="输出根目录（默认 dist）")
    sp.add_argument("--name", default="GryceEngine", help="发布包文件夹名（默认 GryceEngine）")
    sp.add_argument("--project", default="test/demo_project",
                    help="资源项目根（默认 test/demo_project；不存在则跳过）")
    sp.add_argument("--dll-folder", default="dll",
                    help="依赖 DLL 存放的文件夹名（默认 dll）")
    sp.add_argument("--api", choices=("opengl", "vulkan"), default="opengl",
                    help="默认启动器使用的渲染后端（默认 opengl）")
    sp.add_argument("--no-assets", action="store_true",
                    help="不打包 demo_project 资源")
    sp.add_argument("--clean", action="store_true", help="打包前清空输出目录")
    sp.add_argument("--verify", action="store_true", help="打包后立即校验自洽性")
    sp.set_defaults(func=cmd_package)

    sp = sub.add_parser("verify", help="校验已有发布包")
    sp.add_argument("--package", required=True, help="发布包目录")
    sp.add_argument("--dll-folder", default="dll", help="依赖 DLL 文件夹名（默认 dll）")
    sp.add_argument("--objdump", default=None, help="objdump 路径")
    sp.set_defaults(func=cmd_verify)

    sp = sub.add_parser("list-deps", help="仅列出依赖闭包与分类")
    add_common(sp)
    sp.set_defaults(func=cmd_list_deps)

    # help 子命令：help / help <topic>
    help_sp = sub.add_parser("help", help="显示帮助（无参数运行也等价于 help）")
    help_sp.add_argument("topic", nargs="?", default=None,
                         help="子命令名，显示该命令的详细帮助")
    help_sp.set_defaults(func=make_help_func(p, sub))

    return p


def make_help_func(parser, sub_action):
    """生成 help 子命令的处理函数：无参数显示总帮助，带子命令名显示其详情。"""
    def cmd_help(args):
        if args.topic:
            target = sub_action.choices.get(args.topic)
            if target is None:
                print(f"[错误] 未知的的子命令：{args.topic}\n", file=sys.stderr)
                parser.print_help()
                return 2
            target.print_help()
            return 0
        parser.print_help()
        return 0
    return cmd_help


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    # 无参数（或只给了 -h/--help）时，argparse 已处理 -h/--help；
    # command 为 None 表示没给子命令 -> 显示帮助并正常退出。
    if args.command is None:
        parser.print_help()
        return 0
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
