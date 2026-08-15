using GryceEngine.Editor.Models;
using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
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

    public void Play() => _engine.Play();

    public void Pause() => _engine.Pause();

    public void Stop() => _engine.Stop();



    public void NewScene()
    {
        SceneAPI.GScene_New();
        CreateSceneSkeleton();
        AppendConsole("New scene created.");
        _engine.ClearDirty();
        _undoStack.Clear();
        _redoStack.Clear();
        _pendingCreateActions.Clear();
        _pendingComponentRestores.Clear();
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
        int result = SceneAPI.GScene_Save(path);
        if (result == 0)
        {
            AppendConsole($"Scene saved: {path}");
            _engine.ClearDirty();
        }
        else
            AppendConsole("Failed to save scene.");
    }

    /// <summary>Saves the scene to the given path (used by Save As / auto-save UI).</summary>


    /// <summary>Saves the scene to the given path (used by Save As / auto-save UI).</summary>
    public void SaveSceneTo(string path)
    {
        int result = SceneAPI.GScene_Save(path);
        if (result == 0)
        {
            AppendConsole($"Scene saved: {path}");
            _engine.ClearDirty();
        }
        else
        {
            AppendConsole($"Failed to save scene: {path}");
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
        int rc = SceneAPI.GScene_Load(path);
        AppendConsole(rc == 0 ? $"Scene loaded: {path}" : $"Failed to load scene: {path}");
    }

    // === Prefab 工作流 ===



    // === Console ===

    public void AppendConsole(string text, LogLevel level = LogLevel.Info,
                              string sourceFile = "", int sourceLine = 0)
    {
        ConsoleText += text + Environment.NewLine;
        LogEntries.Add(new LogEntry(level, text, sourceFile, sourceLine));
    }



    public void ClearConsole()
    {
        ConsoleText = string.Empty;
        LogEntries.Clear();
    }

    // === Dispose ===


}
