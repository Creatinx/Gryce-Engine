"""
resource.py — Gryce-Engine 离线发布打包引擎 (packaging core)

本模块把"构建目录"整理成一个**可脱机运行**的分发文件夹：

    <output>/<name>/
        GryceRenderSmoke.exe      # 可执行文件
        GryceCore.dll             # 引擎 core DLL —— 与 exe 同级
        GryceRenderer.dll
        GrycePlatform.dll
        dll/                      # 必须依赖的 DLL 统一放这里
            glfw3.dll
            glew32d.dll
            libbox2dd.dll
            libJolt.dll
            libstdc++-6.dll       # MinGW 运行时（依赖，不是 core）
            libgcc_s_seh-1.dll
            libwinpthread-1.dll
        demo_project/             # 资源（models / scenes / shaders）
        run.bat                   # 启动器：把 dll/ 预置到 PATH 再启动
        run_vulkan.bat
        README.txt

设计要点
--------
1. **依赖闭包**：可执行文件只直接导入引擎三 DLL 与 MinGW 运行时；glfw/glew/
   box2d/jolt/vulkan 是引擎 DLL 的*传递*依赖。所以必须递归解析每个 DLL 的导入
   表，得到完整闭包，否则漏拷一个就会在目标机上 0xc0000135。
2. **以真实导入名搜索**：Debug 构建下 GLEW 实为 `glew32d.dll`、Box2D 实为
   `libbox2dd.dll`，不能假设成 release 名字。闭包解析用的是导入表里的字面值。
3. **core 同级、依赖进文件夹**：引擎自身产出的 DLL（GryceCore/Renderer/Platform）
   必须和可执行文件同级（OS 优先从 exe 目录加载）；其余第三方 + MinGW 运行时全部
   放进 `dll/`，由 `run.bat` 把 `dll/` 预置进 PATH，Windows 会在 exe 目录之后、
   系统目录之后搜索 PATH 找到它们，从而脱机运行。
4. **系统 DLL 不打包**：KERNEL32 / USER32 / OPENGL32 / ... 视为目标 Windows 自带，
   跳过（vulkan-1.dll 也按系统处理——用 Vulkan 后端需目标机装有 Vulkan 驱动）。

依赖解析优先用 `objdump -p`（MinGW/CLion 自带），缺失时回退到内置的纯 Python
PE 导入表解析器，因此本工具在任何装有 Python 的机器上都能跑，不强制 binutils。
"""

import os
import shutil
import struct
import subprocess

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

# 引擎自身产出的 DLL（必须与 exe 同级）
ENGINE_CORE_DLLS = {"grycecore", "grycerenderer", "gryceplatform"}

# 视为目标 Windows 自带的系统 DLL（不打包）
SYSTEM_DLLS = {
    "kernel32.dll", "msvcrt.dll", "ntdll.dll", "user32.dll", "gdi32.dll",
    "shell32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll", "uuid.dll",
    "winmm.dll", "bcrypt.dll", "crypt32.dll", "rpcrt4.dll", "ws2_32.dll",
    "opengl32.dll", "gdiplus.dll", "shcore.dll", "dwmapi.dll", "comctl32.dll",
    "comdlg32.dll", "msimg32.dll", "version.dll", "usp10.dll", "imm32.dll",
    "setupapi.dll", "cfgmgr32.dll", "devobj.dll", "iphlpapi.dll", "powrprof.dll",
    "propsys.dll", "shellfolder.dll", "vulkan-1.dll",  # Vulkan 后端依赖驱动，按系统处理
    "msvcp140.dll", "vcruntime140.dll", "api-ms-win-*.dll",  # MSVC 运行库
    "ext-ms-win-*.dll",
}

# MinGW 运行时（按"依赖"处理，进 dll/ 文件夹）
MINGW_RUNTIME = {
    "libstdc++-6.dll", "libgcc_s_seh-1.dll", "libgcc_s_dw2-1.dll",
    "libwinpthread-1.dll",
}

ASSET_SUBDIR_BY_TYPE = {
    "model": "models",
    "scene": "scenes",
    "shader": "shaders",
    "asset": "assets",
}


# ---------------------------------------------------------------------------
# 纯 Python PE 导入表解析（objdump 缺失时的兜底）
# ---------------------------------------------------------------------------

def pe_import_dll_names(path):
    """解析一个 PE 文件的导入 DLL 名列表（大小写保持文件内写法）。

    支持 PE32 / PE32+（MinGW-w64 产出的 64 位 PE 走 PE32+ 分支）。
    解析失败返回空列表，绝不抛异常。
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    if len(data) < 64 or data[:2] != b"MZ":
        return []
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew + 4 > len(data) or data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        return []
    coff = e_lfanew + 4
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]
    opt_start = coff + 20
    if size_opt < 24 or opt_start + size_opt > len(data):
        return []
    magic = struct.unpack_from("<H", data, opt_start)[0]
    if magic == 0x20B:        # PE32+
        dd_offset = opt_start + 112
        num_rva_off = opt_start + 108
    elif magic == 0x10B:      # PE32
        dd_offset = opt_start + 96
        num_rva_off = opt_start + 92
    else:
        return []
    num_rva = struct.unpack_from("<I", data, num_rva_off)[0]
    if num_rva < 2:
        return []
    import_rva, _ = struct.unpack_from("<II", data, dd_offset + 8)  # 数据目录[1]=导入表
    if import_rva == 0:
        return []

    sect_start = opt_start + size_opt
    num_sect = struct.unpack_from("<H", data, coff + 2)[0]

    def rva_to_off(rva):
        for i in range(num_sect):
            sh = sect_start + i * 40
            if sh + 20 > len(data):
                break
            vsize = struct.unpack_from("<I", data, sh + 8)[0]
            vaddr = struct.unpack_from("<I", data, sh + 12)[0]
            rawsize = struct.unpack_from("<I", data, sh + 16)[0]
            rawptr = struct.unpack_from("<I", data, sh + 20)[0]
            span = max(vsize, rawsize)
            if vaddr <= rva < vaddr + span:
                return rawptr + (rva - vaddr)
        return None

    names = []
    base = rva_to_off(import_rva)
    if base is None:
        return []
    i = 0
    while True:
        desc = base + i * 20
        if desc + 20 > len(data):
            break
        ilt, _ts, _fwd, name_rva, _iat = struct.unpack_from("<IIIII", data, desc)
        if ilt == 0 and name_rva == 0:
            break
        no = rva_to_off(name_rva)
        if no is not None and no < len(data):
            end = data.find(b"\x00", no)
            if end == -1:
                end = len(data)
            nm = data[no:end].decode("ascii", "ignore")
            if nm:
                names.append(nm)
        i += 1
        if i > 8192:
            break
    return names


# ---------------------------------------------------------------------------
# 依赖解析器
# ---------------------------------------------------------------------------

class DepResolver:
    """解析从可执行文件出发的完整 DLL 依赖闭包。"""

    def __init__(self, build_dir, objdump=None, extra_dirs=None):
        self.build_dir = build_dir
        self.objdump = objdump
        self.extra_dirs = [d for d in (extra_dirs or []) if d and os.path.isdir(d)]
        self._name_map = None  # lower(name) -> path

    # -- 候选文件索引 -----------------------------------------------------
    def _build_name_map(self):
        if self._name_map is not None:
            return
        m = {}
        search_dirs = []
        if self.build_dir and os.path.isdir(self.build_dir):
            search_dirs.append(self.build_dir)
        search_dirs.extend(self.extra_dirs)
        for d in search_dirs:
            for root, _dirs, files in os.walk(d):
                for fn in files:
                    if fn.lower().endswith(".dll"):
                        m.setdefault(fn.lower(), os.path.join(root, fn))
        # PATH 目录兜底（能抓到系统包前缀里的 DLL）
        for p in os.environ.get("PATH", "").split(os.pathsep):
            if not p or not os.path.isdir(p):
                continue
            try:
                for fn in os.listdir(p):
                    if fn.lower().endswith(".dll"):
                        m.setdefault(fn.lower(), os.path.join(p, fn))
            except OSError:
                pass
        self._name_map = m

    # -- 导入名提取 -------------------------------------------------------
    def imports_of(self, path):
        if self.objdump and os.path.exists(self.objdump):
            return self._imports_objdump(path)
        return pe_import_dll_names(path)

    def _imports_objdump(self, path):
        try:
            out = subprocess.run(
                [self.objdump, "-p", path],
                capture_output=True, text=True, errors="ignore",
            )
        except OSError:
            return pe_import_dll_names(path)
        names = []
        for line in out.stdout.splitlines():
            s = line.strip()
            if s.lower().startswith("dll name"):
                nm = s.split(":", 1)[1].strip()
                if nm:
                    names.append(nm)
        return names

    # -- 闭包求解 ---------------------------------------------------------
    def resolve(self, exe_path):
        """返回 (found, missing)：found=lower名->路径，missing=未找到的 lower 名集合。"""
        self._build_name_map()
        found = {}
        missing = set()
        queue = [exe_path]
        seen_bins = set()
        while queue:
            b = queue.pop()
            if b in seen_bins:
                continue
            seen_bins.add(b)
            try:
                imps = self.imports_of(b)
            except Exception:
                imps = []
            for nm in imps:
                low = nm.lower()
                if self._is_system(low):
                    continue
                if low in found or low in missing:
                    continue
                fp = self._name_map.get(low)
                if fp and os.path.exists(fp):
                    found[low] = fp
                    queue.append(fp)
                else:
                    missing.add(low)
        return found, missing

    @staticmethod
    def _is_system(name):
        if name in SYSTEM_DLLS:
            return True
        # 通配（api-ms-win-*, ext-ms-win-*）
        if name.startswith("api-ms-win-") or name.startswith("ext-ms-win-"):
            return True
        return False

    @staticmethod
    def classify(name):
        """'core' 表示引擎自身 DLL（与 exe 同级），其余均归 'dependency'。"""
        base = os.path.splitext(name.lower())[0]
        if base in ENGINE_CORE_DLLS:
            return "core"
        return "dependency"


def discover_mingw_bins():
    """探测常见的 MinGW 运行时所在目录，补充到依赖搜索路径。"""
    dirs = []
    for env_key in ("MSYS2_ROOT", "MINGW_PREFIX", "MINGW_DIR"):
        v = os.environ.get(env_key)
        if v:
            cand = os.path.join(v, "bin")
            if os.path.isdir(cand):
                dirs.append(cand)
    roots = [
        "D:/msys64/mingw64/bin", "D:/msys64/ucrt64/bin", "D:/msys64/clang64/bin",
        "C:/msys64/mingw64/bin", "C:/msys64/ucrt64/bin",
    ]
    for r in roots:
        if os.path.isdir(r):
            dirs.append(r)
    # CLion 自带 MinGW
    try:
        import glob
        for hit in glob.glob("D:/JetBrains/CLion*/bin/mingw/bin"):
            if os.path.isdir(hit):
                dirs.append(hit)
        for hit in glob.glob("C:/Program Files/JetBrains/CLion*/bin/mingw/bin"):
            if os.path.isdir(hit):
                dirs.append(hit)
    except Exception:
        pass
    # 只保留确实含 MinGW 运行时的目录
    return [d for d in dirs if os.path.exists(os.path.join(d, "libstdc++-6.dll"))]


# ---------------------------------------------------------------------------
# 资源打包（用户既有 ResourceToPak 骨架的可用实现）
# ---------------------------------------------------------------------------

class ResourceToPak:
    """把项目资源（demo_project：models / scenes / shaders）整理进发布包。

    保留用户既有方法名 ``file_to_pak(file, type)``，并新增 ``pack_project``
    做整目录递归拷贝。
    """

    def __init__(self, package_root):
        self.package_root = package_root
        self.assets_dir = os.path.join(package_root, "demo_project")

    def file_to_pak(self, file, type):
        """把单个资源文件拷进发布包对应子目录。

        type: model | scene | shader | asset（其余归入 assets）。
        """
        sub = ASSET_SUBDIR_BY_TYPE.get((type or "asset").lower(), "assets")
        dst_dir = os.path.join(self.assets_dir, sub)
        os.makedirs(dst_dir, exist_ok=True)
        dst = os.path.join(dst_dir, os.path.basename(file))
        shutil.copy2(file, dst)
        return dst

    def pack_project(self, src_root):
        """递归拷贝整个 demo_project 到 <package>/demo_project，保留目录结构。"""
        if not src_root or not os.path.isdir(src_root):
            return False
        os.makedirs(self.assets_dir, exist_ok=True)
        for entry in os.listdir(src_root):
            s = os.path.join(src_root, entry)
            d = os.path.join(self.assets_dir, entry)
            if os.path.isdir(s):
                shutil.copytree(s, d, dirs_exist_ok=True, copy_function=shutil.copy2)
            else:
                shutil.copy2(s, d)
        return True


# ---------------------------------------------------------------------------
# 打包编排
# ---------------------------------------------------------------------------

class Packager:
    def __init__(self, build_dir, exe, output, name, project_src=None,
                 dll_folder="dll", objdump=None, extra_dirs=None,
                 no_assets=False, api="opengl", clean=False):
        self.build_dir = os.path.abspath(build_dir)
        self.exe = exe
        self.output = os.path.abspath(output)
        self.name = name
        self.project_src = project_src
        self.dll_folder = dll_folder
        self.objdump = objdump
        self.extra_dirs = list(extra_dirs or [])
        self.no_assets = no_assets
        self.api = api
        self.clean = clean
        self.pkg_root = os.path.join(self.output, self.name)

    # -- 定位 exe ---------------------------------------------------------
    def resolve_exe(self):
        if self.exe and os.path.exists(self.exe):
            return os.path.abspath(self.exe)
        # 在 build_dir 内递归找 GryceRenderSmoke.exe，优先顶层
        if not os.path.isdir(self.build_dir):
            return None
        top = os.path.join(self.build_dir, "GryceRenderSmoke.exe")
        if os.path.exists(top):
            return top
        hits = []
        for root, _d, files in os.walk(self.build_dir):
            for fn in files:
                if fn.lower() == "grycerendersmoke.exe":
                    hits.append(os.path.join(root, fn))
        hits.sort(key=lambda p: (len(os.path.relpath(p, self.build_dir)), p))
        return hits[0] if hits else None

    # -- 主流程 -----------------------------------------------------------
    def run(self):
        exe_path = self.resolve_exe()
        if not exe_path:
            raise FileNotFoundError(
                f"找不到可执行文件（build_dir={self.build_dir}, exe={self.exe}）。"
                "请用 --exe 指定，或先构建工程。"
            )

        # 依赖搜索路径：exe 同目录 + build_dir + 探测到的 MinGW bin
        extra = list(self.extra_dirs)
        extra.append(os.path.dirname(exe_path))
        for d in discover_mingw_bins():
            if d not in extra:
                extra.append(d)

        resolver = DepResolver(self.build_dir, objdump=self.objdump, extra_dirs=extra)
        found, missing = resolver.resolve(exe_path)

        # 准备输出目录
        if self.clean and os.path.isdir(self.pkg_root):
            shutil.rmtree(self.pkg_root)
        os.makedirs(self.pkg_root, exist_ok=True)
        dll_dir = os.path.join(self.pkg_root, self.dll_folder)
        os.makedirs(dll_dir, exist_ok=True)

        copied_core = []
        copied_dep = []
        # exe 自身
        shutil.copy2(exe_path, os.path.join(self.pkg_root, os.path.basename(exe_path)))
        # 分类拷贝
        for low, src in found.items():
            bucket = resolver.classify(low)
            if bucket == "core":
                dst = os.path.join(self.pkg_root, os.path.basename(src))
                shutil.copy2(src, dst)
                copied_core.append(os.path.basename(src))
            else:
                dst = os.path.join(dll_dir, os.path.basename(src))
                shutil.copy2(src, dst)
                copied_dep.append(os.path.basename(src))

        # 资源
        assets_copied = False
        if not self.no_assets and self.project_src:
            rtp = ResourceToPak(self.pkg_root)
            assets_copied = rtp.pack_project(self.project_src)

        # 启动器 + 说明
        self._write_launchers(exe_path)
        self._write_readme(copied_core, copied_dep, missing, assets_copied)

        return {
            "pkg_root": self.pkg_root,
            "exe": os.path.basename(exe_path),
            "core": sorted(set(copied_core)),
            "dep": sorted(set(copied_dep)),
            "missing": sorted(missing),
            "assets": assets_copied,
        }

    # -- 启动器 -----------------------------------------------------------
    def _write_launchers(self, exe_path):
        exe_name = os.path.basename(exe_path)
        # 批处理变量用字面值 %HERE%，不要进 Python f-string 插值
        common = (
            "@echo off\r\n"
            "setlocal\r\n"
            'set "HERE=%~dp0"\r\n'
            'set "PATH=%HERE%dll;%PATH%"\r\n'
        )
        proj_arg = '"%HERE%demo_project"'

        def cmdline(api, extra=""):
            return ('"%HERE%' + exe_name + '" --project ' + proj_arg +
                    f" --api {api} {extra}%*\r\n")

        # OpenGL（默认）
        with open(os.path.join(self.pkg_root, "run.bat"), "w", newline="") as f:
            f.write(common + cmdline("opengl") + "endlocal\r\n")
        # Vulkan 后端
        with open(os.path.join(self.pkg_root, "run_vulkan.bat"), "w", newline="") as f:
            f.write(common + cmdline("vulkan") + "endlocal\r\n")
        # 交互模式（--play）
        with open(os.path.join(self.pkg_root, "run_play.bat"), "w", newline="") as f:
            f.write(common + cmdline("opengl", "--play ") + "endlocal\r\n")

    # -- README -----------------------------------------------------------
    def _write_readme(self, core, dep, missing, assets):
        lines = []
        lines.append(f"Gryce-Engine 离线发布包：{self.name}")
        lines.append("=" * 48)
        lines.append("")
        lines.append("本文件夹可脱离构建环境（无需 MinGW / MSYS2 / PATH 配置）直接运行。")
        lines.append("")
        lines.append("运行方式（二选一）：")
        lines.append("  1) 双击 run.bat         —— OpenGL 后端（默认，最稳）")
        lines.append("  2) 双击 run_vulkan.bat   —— Vulkan 后端（目标机需装有 Vulkan 驱动）")
        lines.append("     run_play.bat          —— 交互模式（WASD + 鼠标）")
        lines.append("")
        lines.append("也可以命令行加参数，例如：")
        lines.append(f'  run.bat --frames 120 --shot out.png')
        lines.append("")
        lines.append("目录结构")
        lines.append("-" * 48)
        lines.append(f"{self.name}/")
        lines.append(f"  {os.path.basename(self.name)}.exe   (可执行文件)")
        for c in sorted(set(core)):
            lines.append(f"  {c}   (引擎 core DLL，与 exe 同级)")
        lines.append(f"  {self.dll_folder}/")
        for d in sorted(set(dep)):
            lines.append(f"    {d}   (依赖 DLL，由 run.bat 注入 PATH)")
        if assets:
            lines.append("  demo_project/   (资源：models / scenes / shaders)")
        lines.append("  run.bat / run_vulkan.bat / run_play.bat")
        lines.append("")
        lines.append("为什么能脱机运行")
        lines.append("-" * 48)
        lines.append("- 引擎自身 DLL 放在 exe 同目录，OS 优先从 exe 目录加载；")
        lines.append(f"- 其余第三方 + MinGW 运行时放进 {self.dll_folder}/，run.bat 启动前")
        lines.append("  将其预置进 PATH，Windows 在 exe 目录之后搜索 PATH 找到它们；")
        lines.append("- 资源通过 --project 指向包内 demo_project，不依赖源码树路径。")
        lines.append("")
        if missing:
            lines.append("注意：以下导入 DLL 在本次打包时未能在构建树中找到，")
            lines.append("可能在目标机缺失（若是系统 DLL 则无碍，否则需手动补齐）：")
            for m in sorted(set(missing)):
                lines.append(f"  - {m}")
            lines.append("")
        lines.append("由 toolchain/main.py 生成。")
        with open(os.path.join(self.pkg_root, "README.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines))


# ---------------------------------------------------------------------------
# 校验：一个已有包是否自洽（所有导入 DLL 在包内可解析）
# ---------------------------------------------------------------------------

def verify_package(pkg_root, dll_folder="dll", objdump=None):
    """校验包内 exe 的完整导入闭包是否都能在包内解析。"""
    exe_hits = [f for f in os.listdir(pkg_root)
                if f.lower().endswith(".exe")] if os.path.isdir(pkg_root) else []
    if not exe_hits:
        return False, ["包内未找到 .exe"]
    exe_path = os.path.join(pkg_root, exe_hits[0])
    dll_dir = os.path.join(pkg_root, dll_folder)
    search = [pkg_root, dll_dir]
    resolver = DepResolver(pkg_root, objdump=objdump, extra_dirs=search)
    # 让 name_map 同时覆盖 root 与 dll/
    resolver._build_name_map()
    found, missing = resolver.resolve(exe_path)
    problems = []
    if missing:
        problems.append("包内缺失以下导入 DLL：" + ", ".join(sorted(missing)))
    # 确认 core 与 exe 同级、依赖在 dll/
    misplaced = []
    for low, src in found.items():
        bucket = resolver.classify(low)
        base = os.path.basename(src)
        if bucket == "core" and not os.path.dirname(os.path.abspath(src)) == os.path.abspath(pkg_root):
            misplaced.append(f"{base} 应放在 exe 同级（{pkg_root}）")
        if bucket == "dependency" and not os.path.dirname(os.path.abspath(src)) == os.path.abspath(dll_dir):
            misplaced.append(f"{base} 应放在 {dll_folder}/")
    if misplaced:
        problems.append("；".join(misplaced))
    return (len(problems) == 0), problems
