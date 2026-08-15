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

    // === Undo/Redo ===

    /// <summary>Records an action on the undo stack, clears redo, and refreshes command UI.</summary>
    private void PushUndo(IUndoableAction action)
    {
        _undoStack.Push(action);
        _redoStack.Clear();
        System.Windows.Input.CommandManager.InvalidateRequerySuggested();
    }



    private void Undo()
    {
        if (_undoStack.Count == 0) return;
        var action = _undoStack.Pop();
        action.Undo();
        _redoStack.Push(action);
        _engine.MarkSceneDirty();
        AppendConsole("Undo: " + action.Description);
        System.Windows.Input.CommandManager.InvalidateRequerySuggested();
    }



    private void Redo()
    {
        if (_redoStack.Count == 0) return;
        var action = _redoStack.Pop();
        action.Execute();
        _undoStack.Push(action);
        _engine.MarkSceneDirty();
        AppendConsole("Redo: " + action.Description);
        System.Windows.Input.CommandManager.InvalidateRequerySuggested();
    }

    // === Hierarchy ===


}


// === Undo/Redo Action Types ===

internal interface IUndoableAction
{
    string Description { get; }
    void Execute();
    void Undo();
}

internal class CreateEntityAction(EditorViewModel vm, string name, string? componentTypeName) : IUndoableAction
{
    public string Description => $"Create '{name}'";
    public GEntityHandle Handle { get; set; } = GEntityHandle.Null;

    public void Execute() => vm.CreateEntitySilent(name, componentTypeName);
    public void Undo()
    {
        if (Handle != GEntityHandle.Null) vm.DeleteEntitySilent(Handle);
        else vm.DeleteSelectedEntitySilent();
    }
}

internal class DeleteEntityAction : IUndoableAction
{
    private readonly EditorViewModel _vm;
    private readonly GEntityHandle _originalHandle;
    private readonly string _name;
    private readonly string? _json;
    private readonly GEntityHandle _parent;
    private GEntityHandle _restoredHandle;

    public string Description => $"Delete '{_name}'";

    public DeleteEntityAction(EditorViewModel vm, GEntityHandle handle, string name,
                              string? json, GEntityHandle parent)
    {
        _vm = vm;
        _originalHandle = handle;
        _name = name;
        // Capture the entity state now (it still exists at push time for
        // duplicate/paste/prefab paths), so Undo restores the real entity.
        _json = json ?? EntityAPI.ExportJsonUtf8(handle);
        _parent = parent;
    }

    public void Execute()
    {
        GEntityHandle target = _restoredHandle != GEntityHandle.Null ? _restoredHandle : _originalHandle;
        Span<byte> payload = stackalloc byte[sizeof(int)];
        BitConverterCompat.TryWriteBytes(payload, (int)target);
        var cmd = GCommand.Create(GCommandType.DestroyEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _restoredHandle = GEntityHandle.Null;
    }

    public void Undo()
    {
        if (!string.IsNullOrEmpty(_json))
        {
            _restoredHandle = EntityAPI.GEntity_ImportJson(_json!, _parent);
            if (_restoredHandle != GEntityHandle.Null)
            {
                _vm.SelectEntityByHandle(_restoredHandle);
                return;
            }
        }
        _vm.CreateEntitySilent();
    }
}

internal class AddComponentAction(EditorViewModel vm, GEntityHandle entity, ulong typeHash) : IUndoableAction
{
    public string Description => "Add Component";
    public void Execute() => vm.AddComponentSilent(entity, typeHash);
    public void Undo() => vm.RemoveComponentSilent(entity, typeHash);
}

internal class RemoveComponentAction(EditorViewModel vm, GEntityHandle entity, ulong typeHash,
                                     string typeName, ComponentSnapshot snapshot) : IUndoableAction
{
    public string Description => $"Remove '{typeName}'";
    public void Execute() => vm.RemoveComponentSilent(entity, typeHash);
    public void Undo() => vm.RestoreComponentState(entity, typeHash, snapshot);
}

internal sealed class ComponentSnapshot
{
    public List<Field> Fields { get; } = new();
    public List<ScriptProp> ScriptProps { get; } = new();

    public sealed class Field(string name, int size, byte[] bytes)
    {
        public string Name { get; } = name;
        public int Size { get; } = size;
        public byte[] Bytes { get; } = bytes;
    }

    public sealed class ScriptProp(string name, int type, float? floatValue, string? stringValue)
    {
        public string Name { get; } = name;
        public int Type { get; } = type;
        public float? Float { get; } = floatValue;
        public string? String { get; } = stringValue;
    }
}

internal class EntityTypeAction : IUndoableAction
{
    private readonly EditorViewModel _vm;
    private readonly bool _old2d, _old3d;
    private readonly ComponentSnapshot? _old2dSnap, _old3dSnap;
    private bool _new2d, _new3d;
    private ComponentSnapshot? _new2dSnap, _new3dSnap;

    public string Description => "Entity Type";
    public GEntityHandle Entity { get; }

    public EntityTypeAction(EditorViewModel vm, GEntityHandle entity,
                            bool old2d, bool old3d, ComponentSnapshot? old2dSnap, ComponentSnapshot? old3dSnap,
                            bool new2d, bool new3d, ComponentSnapshot? new2dSnap, ComponentSnapshot? new3dSnap)
    {
        _vm = vm;
        Entity = entity;
        _old2d = old2d;
        _old3d = old3d;
        _old2dSnap = old2dSnap;
        _old3dSnap = old3dSnap;
        _new2d = new2d;
        _new3d = new3d;
        _new2dSnap = new2dSnap;
        _new3dSnap = new3dSnap;
    }

    public void SetNewState(bool has2d, bool has3d, ComponentSnapshot? snap2d, ComponentSnapshot? snap3d)
    {
        _new2d = has2d;
        _new3d = has3d;
        _new2dSnap = snap2d;
        _new3dSnap = snap3d;
    }

    public void Execute() => Apply(_new2d, _new3d, _new2dSnap, _new3dSnap);
    public void Undo() => Apply(_old2d, _old3d, _old2dSnap, _old3dSnap);

    private void Apply(bool want2d, bool want3d, ComponentSnapshot? snap2d, ComponentSnapshot? snap3d)
    {
        bool has2d = false, has3d = false;
        ulong hash2d = 0, hash3d = 0;
        int count = ComponentAPI.GComponent_GetCount(Entity);
        for (int i = 0; i < count; i++)
        {
            if (ComponentAPI.GComponent_GetTypeHashAt(Entity, i, out var h) != 0) continue;
            var sb = new StringBuilder(128);
            if (ComponentAPI.GComponent_GetTypeNameAt(Entity, i, sb, sb.Capacity) < 0) continue;
            string type = sb.ToString();
            if (type == "Node2D") { has2d = true; hash2d = h; }
            else if (type == "Node3D") { has3d = true; hash3d = h; }
        }
        if (want2d && !has2d)
        {
            ulong h = _vm.FindRegisteredTypeHash("Node2D");
            if (h != 0) _vm.RestoreComponentState(Entity, h, snap2d);
        }
        else if (!want2d && has2d)
        {
            _vm.RemoveComponentSilent(Entity, hash2d);
        }
        if (want3d && !has3d)
        {
            ulong h = _vm.FindRegisteredTypeHash("Node3D");
            if (h != 0) _vm.RestoreComponentState(Entity, h, snap3d);
        }
        else if (!want3d && has3d)
        {
            _vm.RemoveComponentSilent(Entity, hash3d);
        }
        _vm.NotifyEntityTypeSwitched();
    }
}

internal class RenameEntityAction(EditorViewModel vm, GEntityHandle handle, string oldName, string newName) : IUndoableAction
{
    public string Description => $"Rename to '{newName}'";
    public void Execute() => vm.RenameEntity(handle, newName);
    public void Undo() => vm.RenameEntity(handle, oldName);
}

internal class TransformAction : IUndoableAction
{
    private readonly EditorViewModel _vm;
    private readonly GEntityHandle _handle;
    private readonly GVec3 _oldPos, _newPos;
    private readonly GQuat _oldRot, _newRot;
    private readonly GVec3 _oldScale, _newScale;

    public string Description => "Transform";

    public TransformAction(EditorViewModel vm, GEntityHandle handle,
                           GVec3 oldPos, GQuat oldRot, GVec3 oldScale,
                           GVec3 newPos, GQuat newRot, GVec3 newScale)
    {
        _vm = vm;
        _handle = handle;
        _oldPos = oldPos; _newPos = newPos;
        _oldRot = oldRot; _newRot = newRot;
        _oldScale = oldScale; _newScale = newScale;
    }

    public void Execute() => Apply(_newPos, _newRot, _newScale);
    public void Undo() => Apply(_oldPos, _oldRot, _oldScale);

    private void Apply(GVec3 p, GQuat r, GVec3 s)
    {
        EntityAPI.GEntity_SetLocalPosition(_handle, ref p);
        EntityAPI.GEntity_SetLocalRotation(_handle, ref r);
        EntityAPI.GEntity_SetLocalScale(_handle, ref s);
        _vm.RaiseTransformChanged(_handle);
    }
}

internal class PropertyAction : IUndoableAction
{
    private readonly EditorViewModel _vm;
    private readonly GEntityHandle _entity;
    private readonly ulong _typeHash;
    private readonly string _propName;
    private readonly byte[] _oldValue, _newValue;

    public string Description => $"Property '{_propName}'";

    public PropertyAction(EditorViewModel vm, GEntityHandle entity, ulong typeHash,
                          string propName, byte[] oldValue, byte[] newValue)
    {
        _vm = vm;
        _entity = entity;
        _typeHash = typeHash;
        _propName = propName;
        _oldValue = oldValue;
        _newValue = newValue;
    }

    public void Execute() => Write(_newValue);
    public void Undo() => Write(_oldValue);

    private void Write(byte[] value)
    {
        EditorViewModel.WritePropertyBytes(_entity, _typeHash, _propName, value);
        _vm.RefreshInspector();
    }
}

internal class ReparentAction : IUndoableAction
{
    private readonly GEntityHandle _handle;
    private readonly GEntityHandle _oldParent, _newParent;

    public string Description => "Reparent";

    public ReparentAction(GEntityHandle handle, GEntityHandle oldParent, GEntityHandle newParent)
    {
        _handle = handle;
        _oldParent = oldParent;
        _newParent = newParent;
    }

    public void Execute() => Reparent(_newParent);
    public void Undo() => Reparent(_oldParent);

    private void Reparent(GEntityHandle parent)
    {
        Span<byte> payload = stackalloc byte[sizeof(int) * 2];
        BitConverterCompat.TryWriteBytes(payload, (int)_handle);
        BitConverterCompat.TryWriteBytes(payload.Slice(sizeof(int)), (int)parent);
        var cmd = GCommand.Create(GCommandType.ReparentEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
    }
}
