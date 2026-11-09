# GryceIDE

`GryceIDE.exe` 是 GryceUI 的最小可运行编辑器宿主。

```powershell
.\build\GryceIDE.exe --project test/demo_project --scene res:/scenes/main.gesc
```

可选参数：

- `--api opengl|vulkan|dx12`
- `--frames N`：运行 N 帧后退出
- `--shot <png>`：退出前截图
- `--w N --h N`

当前能力：

- 项目/场景命令行打开
- 场景树与实体选择
- 选中实体 Transform XYZ 编辑与 Apply
- Undo / Redo
- Play / Stop
- Reload
- 3D 场景背景与 GryceUI overlay

IDE 自身的面板、命令处理和场景树位于 `ide/`；通用控件和布局位于
`engine/ui/` 与 `components/node_ui.h`；引擎 DLL 不反向依赖 IDE。
