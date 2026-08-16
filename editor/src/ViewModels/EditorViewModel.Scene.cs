using GryceEngine.Editor.Models;
using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.Views;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Input;

namespace GryceEngine.Editor.ViewModels;

public partial class EditorViewModel
{

    // === Toolbar actions ===

    public void Play()
    {
        _engine.Play();
        // 播放必须进入 Game 视图：输入同步（GameViewActive）与相机移交
        // （GGameView_SetCamera）都在 EnterGameView 中完成；否则工具栏 Play /
        // Ctrl+P 播放时 Lua 收不到键盘鼠标、编辑相机还会覆盖游戏相机。
        ViewportView.RequestGameView?.Invoke();
    }

    public void Pause() => _engine.Pause();

    public void Stop()
    {
        _engine.Stop();
        // 停止播放后回到场景编辑视图（类 Unity 行为）。
        ViewportView.RequestSceneView?.Invoke();
    }



    public void NewScene()
    {
        if (!Services.UnsavedChangesGuard.CheckCanDiscard()) return;
        SceneAPI.GScene_New();
        CreateSceneSkeleton();
        AppendConsole("New scene created.");
        _engine.ClearDirty();
        _undoStack.Clear();
        _redoStack.Clear();
        _pendingCreateActions.Clear();
        _pendingComponentRestores.Clear();
        _pendingModels.Clear();
        _clipboardEntity = GEntityHandle.Null;
        _clipboardEntityName = string.Empty;
        _clipboardIsCut = false;
        _pendingEntityTypeAction = null;
        _entityTypeSwitchPending = false;
        _pendingOpenComponentPicker = false;
        System.Windows.Input.CommandManager.InvalidateRequerySuggested();
        OnPropertyChanged(nameof(EntityCount));
    }

    /// <summary>Populate a fresh scene with a main camera and a key light so it is
    /// immediately usable. Components are attached on the next engine frame via
    /// ProcessPendingSkeleton.</summary>


    /// <summary>Populate a fresh scene with a main camera and a key light so it is
    /// immediately usable. Components are attached on the next engine frame via
    /// ProcessPendingSkeleton.</summary>
    private void CreateSceneSkeleton()
    {
        _pendingSkeleton.Clear();
        _pendingSkeleton.Add(("MainCamera", "Camera"));
        _pendingSkeleton.Add(("MainLight", "Light"));
        CreateEntitySilent("MainCamera");
        CreateEntitySilent("MainLight");
        // Don't auto-select the skeleton entities.
        _pendingSelectEntityName = null;
    }



    public void SaveScene()
    {
        var sb = new StringBuilder(512);
        string path = SceneAPI.GScene_GetCurrentPath(sb, sb.Capacity) > 0
            ? sb.ToString()
            : (SceneAPI.GScene_GetMode() == 0
                ? "res:/scenes/scene_2d.gesc"
                : "res:/scenes/scene_3d.gesc");
        var result = _engine.SaveScene(path);
        if (result == EngineService.SaveResult.Completed)
        {
            AppendConsole($"Scene saved: {path}");
        }
        else if (result == EngineService.SaveResult.Pending)
        {
            AppendConsole("Scene save deferred (editor 2D camera cleanup).");
        }
        else
            Notify("Failed to save scene.", true);
    }

    /// <summary>Saves the scene to the given path (used by Save As / auto-save UI).</summary>


    /// <summary>Saves the scene to the given path (used by Save As / auto-save UI).</summary>
    public void SaveSceneTo(string path)
    {
        var result = _engine.SaveScene(path);
        if (result == EngineService.SaveResult.Completed)
        {
            AppendConsole($"Scene saved: {path}");
        }
        else if (result == EngineService.SaveResult.Pending)
        {
            AppendConsole("Scene save deferred (editor 2D camera cleanup).");
        }
        else
        {
            Notify($"Failed to save scene: {path}", true);
        }
    }



    public void TogglePlayMode()
    {
        // 播放/暂停切换（与播放按钮一致）：播放中再触发则暂停，否则开始播放。
        if (IsPlaying)
            Pause();
        else
            Play();
    }



    public void SetSceneName(string name)
    {
        SceneName = name;
        AppendConsole($"Scene: {name}");
    }



    public void LoadSceneFromPath(string path)
    {
        if (!Services.UnsavedChangesGuard.CheckCanDiscard()) return;
        int rc = SceneAPI.GScene_Load(path);
        if (rc == 0)
            AppendConsole($"Scene loaded: {path}");
        else
            Notify($"Failed to load scene: {path}", true);
    }

    // === Prefab 工作流 ===



    // === Console ===

    public void AppendConsole(string text, LogLevel level = LogLevel.Info,
                              string sourceFile = "", int sourceLine = 0)
    {
        ConsoleText += text + Environment.NewLine;
        LogEntries.Add(new LogEntry(level, text, sourceFile, sourceLine));
        // 控制台有上限：超出后裁剪最旧的一批，避免长时间运行内存无限增长、
        // ConsoleText 拼接退化为 O(n²)。
        if (LogEntries.Count > 4000)
        {
            int trim = 1000;
            while (trim-- > 0 && LogEntries.Count > 0) LogEntries.RemoveAt(0);
            var sb = new StringBuilder();
            foreach (var entry in LogEntries)
            {
                sb.Append(entry.Message).Append(Environment.NewLine);
            }
            ConsoleText = sb.ToString();
        }
    }



    public void ClearConsole()
    {
        ConsoleText = string.Empty;
        LogEntries.Clear();
    }

    // === Dispose ===


}
