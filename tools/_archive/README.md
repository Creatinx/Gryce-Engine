# 归档脚本（一次性迁移工具）

此目录存放 Gryce Engine 历史迁移过程中使用过的一次性脚本，
仅在旧 ImGui C++ 编辑器 → 模块化 DLL + C API + WPF 编辑器
重构期间使用，现不再维护。

- `fix_*`：C API 桥接层批量修正脚本
- `check_*`：构建产物/命名检查脚本
- `del_lines.py` / `find_git.py`：文本与 git 工具
- `run_build_e0.bat`：E0 里程碑一键构建脚本

如需重新引入其中的逻辑，请参考最新 `core/api/*` 与 `editor/src/*` 的实现。
