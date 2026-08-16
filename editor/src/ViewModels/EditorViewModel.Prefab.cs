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
        ApplyMaterialToEntity(_selectedEntity?.Handle ?? GEntityHandle.Null, path);
    }

    /// <summary>Applies a .gmat material file to the given entity's renderer
    /// component（拖放材质到实体时使用目标实体，而不是当前选中项）。</summary>
    public void ApplyMaterialToEntity(GEntityHandle entity, string path)
    {
        if (entity == GEntityHandle.Null || !System.IO.File.Exists(path)) return;
        var target = FindEntity(entity, RootEntities);
        if (target == null) return;
        foreach (var comp in target.Components)
        {
            if (comp.TypeName is "MeshRenderer" or "SkinnedMeshRenderer")
            {
                int rc = MaterialAPI.GMaterial_LoadFromFile(entity, comp.TypeHash, path);
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
            string? instanceJson = EntityAPI.ExportJsonUtf8(handle);
            PushUndo(new DeleteEntityAction(this, handle,
                System.IO.Path.GetFileNameWithoutExtension(prefabPath), instanceJson, parent));
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
