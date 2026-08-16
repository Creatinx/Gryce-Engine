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

    public void CreateEntity()
    {
        CreateEntity(LocalizationService.Instance.T("hierarchy.new_entity_name"));
    }

    /// <summary>
    /// Creates an entity via the C API command buffer.
    /// Payload layout matches the C++ side: { char name[128]; int parent; }.
    /// </summary>


    /// <summary>
    /// Creates an entity via the C API command buffer.
    /// Payload layout matches the C++ side: { char name[128]; int parent; }.
    /// </summary>
    public void CreateEntity(string name, GEntityHandle parent = GEntityHandle.Null)
        => CreateEntityCore(name, parent, recordUndo: true, componentTypeName: null);

    /// <summary>Creates an entity without recording an undo entry (used by undo/redo actions).</summary>


    /// <summary>Creates an entity without recording an undo entry (used by undo/redo actions).</summary>
    internal void CreateEntitySilent()
        => CreateEntityCore(LocalizationService.Instance.T("hierarchy.new_entity_name"),
            GEntityHandle.Null, recordUndo: false, componentTypeName: null);



    internal void CreateEntitySilent(string name, string? componentTypeName = null)
        => CreateEntityCore(name, GEntityHandle.Null, recordUndo: false, componentTypeName);



    private void CreateEntityCore(string name, GEntityHandle parent, bool recordUndo,
                                  string? componentTypeName)
    {
        Span<byte> payload = stackalloc byte[128 + sizeof(int)];
        var nameBytes = Encoding.UTF8.GetBytes(string.IsNullOrWhiteSpace(name) ? "Entity" : name);
        if (nameBytes.Length > 128) nameBytes = nameBytes.AsSpan(0, 128).ToArray();
        nameBytes.AsSpan().CopyTo(payload);
        BitConverterCompat.TryWriteBytes(payload.Slice(128), (int)parent);

        var cmd = GCommand.Create(GCommandType.CreateEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _engine.MarkSceneDirty();
        if (recordUndo)
        {
            var action = new CreateEntityAction(this, name, componentTypeName);
            PushUndo(action);
            _pendingCreateActions.Enqueue((name, action));
        }
        if (!string.IsNullOrEmpty(componentTypeName))
        {
            _pendingComponentForNewEntity = componentTypeName;
            _pendingNewEntityName = name;
        }
        AppendConsole($"Entity created: {name}");
        _pendingSelectEntityName = name;
    }

    /// <summary>Creates an entity as a child of the given parent.</summary>


    /// <summary>Creates an entity as a child of the given parent.</summary>
    public void CreateChildEntity(GEntityHandle parent)
    {
        CreateEntity(LocalizationService.Instance.T("hierarchy.new_entity_name"), parent);
    }

    /// <summary>
    /// Creates an entity (with the default name) and, once it appears in the
    /// hierarchy, selects it and opens the Add-Component picker so the user can
    /// immediately attach the first component (Create Entity -> New Component).
    /// </summary>


    /// <summary>
    /// Creates an entity (with the default name) and, once it appears in the
    /// hierarchy, selects it and opens the Add-Component picker so the user can
    /// immediately attach the first component (Create Entity -> New Component).
    /// </summary>
    public void CreateEntityThenOpenComponentPicker(GEntityHandle parent = GEntityHandle.Null)
    {
        CreateEntity(LocalizationService.Instance.T("hierarchy.new_entity_name"), parent);
        _pendingOpenComponentPicker = true;
    }

    /// <summary>
    /// Creates an entity and attaches a component on the next engine frame.
    /// The component is resolved through the registered type table by its short name.
    /// </summary>


    /// <summary>
    /// Creates an entity and attaches a component on the next engine frame.
    /// The component is resolved through the registered type table by its short name.
    /// </summary>
    public void CreateEntityWithComponent(string name, GEntityHandle parent, string componentTypeName)
    {
        CreateEntityCore(name, parent, recordUndo: true, componentTypeName);
    }

    /// <summary>Deletes the selected entity without recording an undo entry.</summary>


    /// <summary>Deletes the selected entity without recording an undo entry.</summary>
    internal void DeleteSelectedEntitySilent()
    {
        if (_selectedEntity == null) return;
        DeleteEntityCore(_selectedEntity.Handle, _selectedEntity.Name,
            EntityAPI.ExportJsonUtf8(_selectedEntity.Handle),
            EntityAPI.GEntity_GetParent(_selectedEntity.Handle), recordUndo: false);
    }

    /// <summary>Deletes a specific entity without recording an undo entry.</summary>


    /// <summary>Deletes a specific entity without recording an undo entry.</summary>
    internal void DeleteEntitySilent(GEntityHandle handle)
    {
        if (handle == GEntityHandle.Null) { DeleteSelectedEntitySilent(); return; }
        var name = EntityAPI.GetNameUtf8(handle) ?? "Entity";
        DeleteEntityCore(handle, name, EntityAPI.ExportJsonUtf8(handle),
            EntityAPI.GEntity_GetParent(handle), recordUndo: false);
    }



    private void DeleteEntityCore(GEntityHandle handle, string name, string? json,
                                  GEntityHandle parent, bool recordUndo)
    {
        Span<byte> payload = stackalloc byte[sizeof(int)];
        BitConverterCompat.TryWriteBytes(payload, (int)handle);
        var cmd = GCommand.Create(GCommandType.DestroyEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _engine.MarkSceneDirty();
        if (recordUndo) PushUndo(new DeleteEntityAction(this, handle, name, json, parent));
        AppendConsole($"Deleted entity: {name}");
    }



    public void DeleteSelectedEntity()
    {
        // 批量删除：多选时按顶层实体逐个删除（子实体随父删除，不重复）。
        if (_multiSelection.Count > 1)
        {
            foreach (var h in TopmostSelection(_multiSelection))
            {
                DeleteEntityCore(h, EntityAPI.GetNameUtf8(h) ?? "Entity",
                    EntityAPI.ExportJsonUtf8(h), EntityAPI.GEntity_GetParent(h), recordUndo: true);
            }
            ClearMultiSelection();
            SelectedEntity = null;
            return;
        }

        if (_selectedEntity == null) return;
        var handle = _selectedEntity.Handle;
        var name = _selectedEntity.Name;
        // 删除前导出完整实体树（含子层级/组件），Undo 时原样恢复。
        string? json = EntityAPI.ExportJsonUtf8(handle);
        var parent = EntityAPI.GEntity_GetParent(handle);
        Span<byte> payload = stackalloc byte[sizeof(int)];
        BitConverterCompat.TryWriteBytes(payload, (int)handle);
        var cmd = GCommand.Create(GCommandType.DestroyEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _engine.MarkSceneDirty();
        _undoStack.Push(new DeleteEntityAction(this, handle, name, json, parent));
        _redoStack.Clear();
        AppendConsole($"Deleted entity: {name}");
    }



    public void DuplicateSelectedEntity()
    {
        // 批量复制：多选时复制整个集合（仅顶层，父+子同选不重复），
        // 完成后选中第一个新实体。
        if (_multiSelection.Count > 1)
        {
            var created = new List<GEntityHandle>();
            foreach (var h in TopmostSelection(_multiSelection))
            {
                var nh = DuplicateEntityToParent(h, EntityAPI.GEntity_GetParent(h));
                if (nh != GEntityHandle.Null) created.Add(nh);
            }
            ClearMultiSelection();
            if (created.Count > 0) SelectEntityByHandle(created[0]);
            return;
        }

        if (_selectedEntity == null) return;
        var newHandle = DuplicateEntityToParent(_selectedEntity.Handle,
            EntityAPI.GEntity_GetParent(_selectedEntity.Handle));
        if (newHandle != GEntityHandle.Null) SelectEntityByHandle(newHandle);
    }

    /// <summary>复制实体子树到指定父级（含 Undo 记录）；失败回退创建空实体。</summary>
    private GEntityHandle DuplicateEntityToParent(GEntityHandle handle, GEntityHandle parent)
    {
        if (handle == GEntityHandle.Null) return GEntityHandle.Null;
        var name = EntityAPI.GetNameUtf8(handle) ?? "Entity";
        string copyName = name + LocalizationService.Instance.T("hierarchy.duplicate_suffix");
        string? json = EntityAPI.ExportJsonUtf8(handle);
        if (!string.IsNullOrEmpty(json))
        {
            var newHandle = EntityAPI.GEntity_ImportJson(json!, parent);
            if (newHandle != GEntityHandle.Null)
            {
                _engine.MarkSceneDirty();
                // Undo 需要能原样恢复副本，这里把新实体的完整 JSON 一并记下，
                // 避免 Undo 时因 json==null 回退到创建空白实体。
                string? copyJson = EntityAPI.ExportJsonUtf8(newHandle);
                PushUndo(new DeleteEntityAction(this, newHandle, copyName, copyJson, parent));
                AppendConsole($"Duplicated: {name}");
                return newHandle;
            }
        }
        CreateEntity(copyName, parent);
        AppendConsole($"Duplicated: {name}");
        return GEntityHandle.Null;
    }



    public void FocusSelectedEntity()
    {
        if (_selectedEntity == null) return;
        Span<byte> payload = stackalloc byte[sizeof(int)];
        BitConverterCompat.TryWriteBytes(payload, (int)_selectedEntity.Handle);
        var cmd = GCommand.Create(GCommandType.SelectEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        AppendConsole($"Focusing on: {_selectedEntity.Name}");
    }



    public void CutSelectedEntity()
    {
        if (_selectedEntity == null) return;
        _clipboardEntity = _selectedEntity.Handle;
        _clipboardEntityName = _selectedEntity.Name;
        _clipboardIsCut = true;
        AppendConsole($"Cut: {_selectedEntity.Name}");
    }



    public void CopySelectedEntity()
    {
        if (_selectedEntity == null) return;
        _clipboardEntity = _selectedEntity.Handle;
        _clipboardEntityName = _selectedEntity.Name;
        _clipboardIsCut = false;
        AppendConsole($"Copied: {_selectedEntity.Name}");
    }



    public void PasteToSelected()
    {
        if (_clipboardEntity == GEntityHandle.Null) return;
        if (_clipboardIsCut)
        {
            // For cut, reparent the clipboard entity to the selected entity (or root)
            var oldParent = EntityAPI.GEntity_GetParent(_clipboardEntity);
            var parentHandle = _selectedEntity?.Handle ?? GEntityHandle.Null;
            Span<byte> payload = stackalloc byte[sizeof(int) * 2];
            BitConverterCompat.TryWriteBytes(payload, (int)_clipboardEntity);
            BitConverterCompat.TryWriteBytes(payload.Slice(sizeof(int)), (int)parentHandle);
            var cmd = GCommand.Create(GCommandType.ReparentEntity, payload);
            CoreAPI.GCore_PushCommand(ref cmd);
            _engine.MarkSceneDirty();
            PushUndo(new ReparentAction(_clipboardEntity, oldParent, parentHandle));
            _clipboardEntity = GEntityHandle.Null;
            _clipboardEntityName = string.Empty;
            _clipboardIsCut = false;
            AppendConsole("Pasted entity (cut).");
        }
        else
        {
            // For copy, deep-copy the whole subtree (new UUIDs, hierarchy preserved)
            var parentHandle = _selectedEntity?.Handle ?? GEntityHandle.Null;
            string? json = EntityAPI.ExportJsonUtf8(_clipboardEntity);
            if (!string.IsNullOrEmpty(json))
            {
                var newHandle = EntityAPI.GEntity_ImportJson(json!, parentHandle);
                if (newHandle != GEntityHandle.Null)
                {
                    _engine.MarkSceneDirty();
                    string? copyJson = EntityAPI.ExportJsonUtf8(newHandle);
                    PushUndo(new DeleteEntityAction(this, newHandle, _clipboardEntityName, copyJson, parentHandle));
                    AppendConsole($"Pasted: {_clipboardEntityName}");
                    SelectEntityByHandle(newHandle);
                    return;
                }
            }
            CreateEntity(_clipboardEntityName, parentHandle);
            AppendConsole($"Pasted: {_clipboardEntityName}");
        }
    }



    public void RenameSelectedEntity()
    {
        if (_selectedEntity == null) return;
        var entity = _selectedEntity;
        var dialog = new Views.InputDialog("Rename Entity", "Enter new name:", entity.Name);
        if (ModalDialog.Show(dialog, System.Windows.Application.Current.MainWindow) == true &&
            !string.IsNullOrWhiteSpace(dialog.InputText))
        {
            var oldName = entity.Name;
            RenameEntity(entity.Handle, dialog.InputText);
            PushUndo(new RenameEntityAction(this, entity.Handle, oldName, dialog.InputText));
        }
    }

    // === Undo/Redo ===

    /// <summary>Records an action on the undo stack, clears redo, and refreshes command UI.</summary>


    // === Inspector ===

    public void RefreshRegisteredTypes()
    {
        RegisteredTypes.Clear();
        int count = ComponentAPI.GComponent_GetRegisteredTypeCount();
        for (int i = 0; i < count; i++)
        {
            var sb = new StringBuilder(128);
            if (ComponentAPI.GComponent_GetRegisteredTypeInfo(i, out ulong hash, sb, sb.Capacity) >= 0)
            {
                RegisteredTypes.Add(new RegisteredTypeItem(sb.ToString(), hash));
            }
        }
    }



    public void RenameEntity(GEntityHandle handle, string newName)
    {
        Span<byte> payload = stackalloc byte[sizeof(int) + 128];
        BitConverterCompat.TryWriteBytes(payload, (int)handle);
        var nameBytes = Encoding.UTF8.GetBytes(newName);
        nameBytes.AsSpan().CopyTo(payload.Slice(sizeof(int)));
        var cmd = GCommand.Create(GCommandType.RenameEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _engine.MarkSceneDirty();
    }

    /// <summary>重命名并记录一条 undo（Hierarchy/Inspector 入口与 F2 快捷键一致）。</summary>
    public void RenameEntityUndoable(GEntityHandle handle, string newName)
    {
        if (handle == GEntityHandle.Null) return;
        string oldName = EntityAPI.GetNameUtf8(handle) ?? string.Empty;
        if (string.Equals(oldName, newName, StringComparison.Ordinal)) return;
        RenameEntity(handle, newName);
        PushUndo(new RenameEntityAction(this, handle, oldName, newName));
    }

    /// <summary>Sets the script_path of the entity's Script component.</summary>


    /// <summary>Sets the script_path of the entity's Script component.</summary>
    public void SetScript(GEntityHandle handle, string path)
    {
        Span<byte> payload = stackalloc byte[sizeof(int) + 128];
        BitConverterCompat.TryWriteBytes(payload, (int)handle);
        var pathBytes = Encoding.UTF8.GetBytes(path ?? string.Empty);
        if (pathBytes.Length > 128) pathBytes = pathBytes.AsSpan(0, 128).ToArray();
        pathBytes.AsSpan().CopyTo(payload.Slice(sizeof(int)));
        var cmd = GCommand.Create(GCommandType.SetScript, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _engine.MarkSceneDirty();
    }

    /// <summary>Reloads all attached scripts (GryceSRT) on the next frame.</summary>


    /// <summary>Reloads all attached scripts (GryceSRT) on the next frame.</summary>
    public void ReloadScripts()
    {
        var cmd = GCommand.Create(GCommandType.ReloadScripts, Span<byte>.Empty);
        CoreAPI.GCore_PushCommand(ref cmd);
        AppendConsole("Scripts reload requested.");
    }

    /// <summary>Writes an exposed script property (GryceSRT) to the core.</summary>


    /// <summary>Writes an exposed script property (GryceSRT) to the core.</summary>
    public void WriteScriptProp(GEntityHandle handle, string propName, object value)
    {
        bool ok = value is float f
            ? ScriptAPI.SetPropFloat(handle, propName, f)
            : ScriptAPI.SetPropString(handle, propName, value?.ToString() ?? string.Empty);
        if (ok)
        {
            _engine.MarkSceneDirty();
        }
        else
        {
            AppendConsole($"Failed to set script prop '{propName}'");
        }
    }

    // === 资源拖放 / 导入 ===

    /// <summary>单个待导入模型的队列项：实体创建后按 2 帧状态机挂载
    /// MeshRenderer 并写入 mesh_path。队列化后连续拖入多个模型互不覆盖。</summary>
    private sealed class PendingModelImport
    {
        public PendingModelImport(string name, string path)
        {
            Name = name;
            Path = path;
        }

        public string Name { get; }
        public string Path { get; }
        public GEntityHandle MeshHandle { get; set; } = GEntityHandle.Null;
    }

    private readonly Queue<PendingModelImport> _pendingModels = new();

    /// <summary>Creates an entity from a model file and attaches a MeshRenderer
    /// (setup completes over the next engine frames via ApplyPendingModelSetup).</summary>
    public void InstantiateModel(string filePath, GEntityHandle parent = GEntityHandle.Null)
    {
        if (string.IsNullOrWhiteSpace(filePath)) return;
        string name = System.IO.Path.GetFileNameWithoutExtension(filePath);
        CreateEntity(name, parent);
        _pendingModels.Enqueue(new PendingModelImport(name, filePath));
    }



    private void ApplyPendingModelSetup()
    {
        if (_pendingModels.Count == 0) return;
        var pending = _pendingModels.Peek();

        // 第一帧：实体已出现 → 挂 MeshRenderer
        if (pending.MeshHandle == GEntityHandle.Null)
        {
            var entity = FindNewestEntityByName(pending.Name, RootEntities);
            if (entity == null) return;
            pending.MeshHandle = entity.Handle;
            SelectEntityByHandle(entity.Handle);
            ulong hash = FindRegisteredTypeHash("MeshRenderer");
            if (hash != 0)
            {
                ComponentAPI.GComponent_AddComponent(entity.Handle, hash);
            }
            return;
        }

        // 第二帧：MeshRenderer 组件已挂上 → 设置 mesh_path（反射字符串字段）。
        // 按组件类型名定位，而不是假定索引 0 就是 MeshRenderer。
        ulong rendererHash = FindComponentTypeHash(pending.MeshHandle, "MeshRenderer");
        if (rendererHash != 0)
        {
            var buf = new byte[256];
            string path = pending.Path;
            int count = Encoding.UTF8.GetBytes(path, 0, Math.Min(path.Length, 250), buf, 0);
            buf[count] = 0;
            WritePropertyBytes(pending.MeshHandle, rendererHash, "mesh_path", buf);
            _engine.MarkSceneDirty();
            AppendConsole($"Imported model: {pending.Path}");
            _pendingModels.Dequeue();
        }
    }

    /// <summary>在实体的组件列表中按类型短名查找 type hash（找不到返回 0）。</summary>
    internal ulong FindComponentTypeHash(GEntityHandle entity, string typeName)
    {
        int count = ComponentAPI.GComponent_GetCount(entity);
        for (int i = 0; i < count; i++)
        {
            if (ComponentAPI.GComponent_GetTypeHashAt(entity, i, out ulong hash) != 0) continue;
            var sb = new StringBuilder(128);
            if (ComponentAPI.GComponent_GetTypeNameAt(entity, i, sb, sb.Capacity) < 0) continue;
            if (sb.ToString() == typeName) return hash;
        }
        return 0;
    }



    internal ulong FindRegisteredTypeHash(string typeName)
    {
        foreach (var t in RegisteredTypes)
        {
            if (t.TypeName == typeName) return t.TypeHash;
        }
        return 0;
    }

    /// <summary>Applies a .gmat material file to the selected renderer component.</summary>

}
