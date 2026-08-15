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

    /// <summary>Applies a .gmat material file to the selected renderer component.</summary>
    public void ApplyMaterialFileToSelection(string path)
    {
        if (_selectedEntity == null || !System.IO.File.Exists(path)) return;
        foreach (var comp in _selectedEntity.Components)
        {
            if (comp.TypeName is "MeshRenderer" or "SkinnedMeshRenderer")
            {
                int rc = MaterialAPI.GMaterial_LoadFromFile(_selectedEntity.Handle, comp.TypeHash, path);
                if (rc == 0)
                {
                    _engine.MarkSceneDirty();
                    AppendConsole($"Material applied: {path}");
                    RefreshInspector();
                }
                else
                {
                    AppendConsole($"Failed to apply material: {path}");
                }
                return;
            }
        }
        AppendConsole("Selected entity has no MeshRenderer / SkinnedMeshRenderer.");
    }



    // === Prefab 工作流 ===

    public void CreatePrefabFromSelection()
    {
        if (_selectedEntity == null) return;
        string defaultDir = System.IO.Path.Combine(_engine.ProjectRoot, "prefabs");
        var dialog = new Microsoft.Win32.SaveFileDialog
        {
            Title = "Save Prefab",
            Filter = "Prefab (*.gesc)|*.gesc|All Files (*.*)|*.*",
            FileName = _selectedEntity.Name + ".gesc",
            InitialDirectory = System.IO.Directory.Exists(defaultDir) ? defaultDir : _engine.ProjectRoot
        };
        if (dialog.ShowDialog() == true)
        {
            int rc = EntityAPI.GEntity_SaveAsPrefab(_selectedEntity.Handle, dialog.FileName);
            AppendConsole(rc == 0 ? $"Prefab saved: {dialog.FileName}" : $"Failed to save prefab: {dialog.FileName}");
        }
    }



    public void InstantiatePrefabDialog()
    {
        var dialog = new Microsoft.Win32.OpenFileDialog
        {
            Title = "Instantiate Prefab",
            Filter = "Prefab (*.gesc;*.geprefab;*.geprefabvariant)|*.gesc;*.geprefab;*.geprefabvariant|All Files (*.*)|*.*"
        };
        if (dialog.ShowDialog() == true)
        {
            InstantiatePrefab(dialog.FileName);
        }
    }



    public void InstantiatePrefab(string prefabPath, GEntityHandle parent = GEntityHandle.Null)
    {
        GEntityHandle handle = EntityAPI.GEntity_CreatePrefabInstance(prefabPath, parent);
        if (handle != GEntityHandle.Null)
        {
            _engine.MarkSceneDirty();
            PushUndo(new DeleteEntityAction(this, handle,
                System.IO.Path.GetFileNameWithoutExtension(prefabPath), null, parent));
            AppendConsole($"Instantiated prefab: {prefabPath}");
            SelectEntityByHandle(handle);
        }
        else
        {
            AppendConsole($"Failed to instantiate prefab: {prefabPath}");
        }
    }



    public void ApplySelectedPrefab()
    {
        if (_selectedEntity == null) return;
        int rc = EntityAPI.GEntity_ApplyPrefab(_selectedEntity.Handle);
        if (rc == 0)
        {
            _engine.MarkSceneDirty();
            AppendConsole("Prefab applied (template updated).");
        }
        else
        {
            AppendConsole("Failed to apply prefab (not a prefab instance?).");
        }
    }



    public void RevertSelectedPrefab()
    {
        if (_selectedEntity == null) return;
        int rc = EntityAPI.GEntity_RevertPrefab(_selectedEntity.Handle);
        if (rc == 0)
        {
            _engine.MarkSceneDirty();
            AppendConsole("Prefab reverted.");
        }
        else
        {
            AppendConsole("Failed to revert prefab (not a prefab instance?).");
        }
    }

    // === Console ===


}
