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

    public int AddComponent(ulong typeHash)
    {
        if (_selectedEntity == null) return -1;
        var entity = _selectedEntity.Handle;
        int result = ComponentAPI.GComponent_AddComponent(entity, typeHash);
        if (result == 0)
        {
            _engine.MarkSceneDirty();
            PushUndo(new AddComponentAction(this, entity, typeHash));
            AppendConsole($"Added component to '{_selectedEntity.Name}'");
            RefreshInspector();
        }
        else
        {
            AppendConsole($"Failed to add component to '{_selectedEntity.Name}'");
        }
        return result;
    }



    public void RemoveComponent(ulong typeHash)
    {
        if (_selectedEntity == null) return;
        var entity = _selectedEntity.Handle;
        string typeName = "Component";
        foreach (var c in _selectedEntity.Components)
        {
            if (c.TypeHash == typeHash) { typeName = c.TypeName; break; }
        }
        var snapshot = CaptureComponentState(entity, typeHash, typeName);
        int result = ComponentAPI.GComponent_RemoveComponent(entity, typeHash);
        if (result == 0)
        {
            _engine.MarkSceneDirty();
            PushUndo(new RemoveComponentAction(this, entity, typeHash, typeName, snapshot));
            AppendConsole($"Removed component from '{_selectedEntity.Name}'");
            RefreshInspector();
        }
        else
        {
            AppendConsole($"Failed to remove component from '{_selectedEntity.Name}'");
        }
    }



    public void WriteTransform()
    {
        if (_selectedEntity == null) return;
        var handle = _selectedEntity.Handle;
        EntityAPI.GEntity_GetLocalPosition(handle, out var oldPos);
        EntityAPI.GEntity_GetLocalRotation(handle, out var oldRot);
        EntityAPI.GEntity_GetLocalScale(handle, out var oldScale);
        var pos = _selectedEntity.LocalPosition;
        var rot = _selectedEntity.LocalRotation;
        var scl = _selectedEntity.LocalScale;

        Span<byte> payload = stackalloc byte[sizeof(int) + 3 * 4 * sizeof(float)];
        int offset = 0;
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), (int)handle);
        offset += sizeof(int);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), pos.X);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), pos.Y);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), pos.Z);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), rot.X);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), rot.Y);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), rot.Z);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), rot.W);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), scl.X);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), scl.Y);
        offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), scl.Z);

        var cmd = GCommand.Create(GCommandType.SetTransform, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _engine.MarkSceneDirty();
        if (!Vec3Close(oldPos, pos) || !QuatClose(oldRot, rot) || !Vec3Close(oldScale, scl))
        {
            PushUndo(new TransformAction(this, handle, oldPos, oldRot, oldScale, pos, rot, scl));
        }
    }

    /// <summary>实时写入 Transform 到引擎（不推 undo），供 Inspector 拖拽/输入时实时预览。</summary>


    /// <summary>实时写入 Transform 到引擎（不推 undo），供 Inspector 拖拽/输入时实时预览。</summary>
    public void WriteTransformLive()
    {
        if (_selectedEntity == null) return;
        var handle = _selectedEntity.Handle;
        var pos = _selectedEntity.LocalPosition;
        var rot = _selectedEntity.LocalRotation;
        var scl = _selectedEntity.LocalScale;

        Span<byte> payload = stackalloc byte[sizeof(int) + 3 * 4 * sizeof(float)];
        int offset = 0;
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), (int)handle); offset += sizeof(int);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), pos.X); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), pos.Y); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), pos.Z); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), rot.X); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), rot.Y); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), rot.Z); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), rot.W); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), scl.X); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), scl.Y); offset += sizeof(float);
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), scl.Z);

        var cmd = GCommand.Create(GCommandType.SetTransform, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _engine.MarkSceneDirty();
    }

    /// <summary>提交一次 Transform 编辑：用聚焦时捕获的旧状态推一条 undo。</summary>


    /// <summary>提交一次 Transform 编辑：用聚焦时捕获的旧状态推一条 undo。</summary>
    public void FlushTransformUndo(GVec3 oldPos, GQuat oldRot, GVec3 oldScale)
    {
        if (_selectedEntity == null) return;
        var pos = _selectedEntity.LocalPosition;
        var rot = _selectedEntity.LocalRotation;
        var scl = _selectedEntity.LocalScale;
        if (!Vec3Close(oldPos, pos) || !QuatClose(oldRot, rot) || !Vec3Close(oldScale, scl))
            PushUndo(new TransformAction(this, _selectedEntity.Handle, oldPos, oldRot, oldScale, pos, rot, scl));
    }



    public void RefreshInspector()
    {
        if (_selectedEntity == null) return;
        _selectedEntity.RefreshTransform();
        _selectedEntity.RefreshComponents();
        foreach (var comp in _selectedEntity.Components)
        {
            comp.RefreshProperties();
            comp.RefreshScriptProps();
        }
        OnPropertyChanged(nameof(HasRendererComponent));
        OnPropertyChanged(nameof(HasPrefabInstance));
        RefreshEntityType();
    }

    // === 实体类型（None / Node2D / Node3D 热切换）===



    /// <summary>Collects the distinct component types currently present in the
    /// scene (across all entities) — used by the New Script dialog's
    /// "Parent Component" list.</summary>
    public List<string> GetSceneComponentTypes()
    {
        var result = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        int entityCount = EntityAPI.GEntity_GetCount();
        for (int i = 0; i < entityCount; i++)
        {
            var handle = EntityAPI.GEntity_GetAt(i);
            if (handle == GEntityHandle.Null) continue;
            int compCount = ComponentAPI.GComponent_GetCount(handle);
            for (int c = 0; c < compCount; c++)
            {
                var sb = new StringBuilder(128);
                if (ComponentAPI.GComponent_GetTypeNameAt(handle, c, sb, sb.Capacity) >= 0)
                {
                    string shortName = Models.EntityModel.ShortTypeName(sb.ToString());
                    if (seen.Add(shortName)) result.Add(shortName);
                }
            }
        }
        return result;
    }



    private void ApplyEntityType(int target)
    {
        if (_selectedEntity == null) return;

        bool has2d = false, has3d = false;
        ulong node2dHash = 0, node3dHash = 0;
        foreach (var c in _selectedEntity.Components)
        {
            if (c.TypeName == "Node2D") { has2d = true; node2dHash = c.TypeHash; }
            else if (c.TypeName == "Node3D") { has3d = true; node3dHash = c.TypeHash; }
        }
        var old2dSnap = has2d ? CaptureComponentState(_selectedEntity.Handle, node2dHash, "Node2D") : null;
        var old3dSnap = has3d ? CaptureComponentState(_selectedEntity.Handle, node3dHash, "Node3D") : null;

        bool changed = false;
        switch (target)
        {
            case 1: // Node2D
                if (has3d)
                {
                    ComponentAPI.GComponent_RemoveComponent(_selectedEntity.Handle, node3dHash);
                    changed = true;
                }
                if (!has2d)
                {
                    ulong h = FindRegisteredTypeHash("Node2D");
                    if (h != 0)
                    {
                        ComponentAPI.GComponent_AddComponent(_selectedEntity.Handle, h);
                        changed = true;
                    }
                }
                break;
            case 2: // Node3D
                if (has2d)
                {
                    ComponentAPI.GComponent_RemoveComponent(_selectedEntity.Handle, node2dHash);
                    changed = true;
                }
                if (!has3d)
                {
                    ulong h = FindRegisteredTypeHash("Node3D");
                    if (h != 0)
                    {
                        ComponentAPI.GComponent_AddComponent(_selectedEntity.Handle, h);
                        changed = true;
                    }
                }
                break;
            default: // None：移除两个类型组件
                if (has2d)
                {
                    ComponentAPI.GComponent_RemoveComponent(_selectedEntity.Handle, node2dHash);
                    changed = true;
                }
                if (has3d)
                {
                    ComponentAPI.GComponent_RemoveComponent(_selectedEntity.Handle, node3dHash);
                    changed = true;
                }
                break;
        }

        if (changed)
        {
            var action = new EntityTypeAction(this, _selectedEntity.Handle,
                has2d, has3d, old2dSnap, old3dSnap,
                has2d, has3d, old2dSnap, old3dSnap);
            _pendingEntityTypeAction = action;
            _entityTypeSwitchPending = true;
            _engine.MarkSceneDirty();
        }
    }

    /// <summary>
    /// Captures the post-switch component state once the engine has processed
    /// the async component commands, then records the undo entry.
    /// </summary>


    /// <summary>
    /// Captures the post-switch component state once the engine has processed
    /// the async component commands, then records the undo entry.
    /// </summary>
    private void FinalizePendingEntityTypeAction()
    {
        var action = _pendingEntityTypeAction;
        _pendingEntityTypeAction = null;
        if (action == null) return;

        bool has2d = false, has3d = false;
        ulong hash2d = 0, hash3d = 0;
        int count = ComponentAPI.GComponent_GetCount(action.Entity);
        for (int i = 0; i < count; i++)
        {
            if (ComponentAPI.GComponent_GetTypeHashAt(action.Entity, i, out var h) != 0) continue;
            var sb = new StringBuilder(128);
            if (ComponentAPI.GComponent_GetTypeNameAt(action.Entity, i, sb, sb.Capacity) < 0) continue;
            string type = sb.ToString();
            if (type == "Node2D") { has2d = true; hash2d = h; }
            else if (type == "Node3D") { has3d = true; hash3d = h; }
        }
        action.SetNewState(has2d, has3d,
            has2d ? CaptureComponentState(action.Entity, hash2d, "Node2D") : null,
            has3d ? CaptureComponentState(action.Entity, hash3d, "Node3D") : null);
        PushUndo(action);
    }



    internal void NotifyEntityTypeSwitched()
    {
        _entityTypeSwitchPending = true;
        _engine.MarkSceneDirty();
    }

    /// <summary>从当前组件推导实体类型；切换命令未处理完时保持目标值。</summary>


    /// <summary>从当前组件推导实体类型；切换命令未处理完时保持目标值。</summary>
    private void RefreshEntityType()
    {
        if (_entityTypeSwitchPending) return;
        if (_selectedEntity == null)
        {
            if (_selectedEntityType != 0)
            {
                _selectedEntityType = 0;
                OnPropertyChanged(nameof(SelectedEntityType));
            }
            return;
        }

        // 注意：这里不能重建 Components——RefreshInspector() 刚填充过属性，
        // 重建会得到属性为空的 ComponentModel，导致检查器属性行消失。
        bool has2d = false, has3d = false;
        foreach (var c in _selectedEntity.Components)
        {
            if (c.TypeName == "Node2D") has2d = true;
            else if (c.TypeName == "Node3D") has3d = true;
        }
        int value = has2d ? 1 : has3d ? 2 : 0;
        if (_selectedEntityType != value)
        {
            _selectedEntityType = value;
            OnPropertyChanged(nameof(SelectedEntityType));
        }
    }



    public void WritePropertyValue(ComponentModel comp, PropertyModel prop)
    {
        if (_selectedEntity == null) return;
        var entity = _selectedEntity.Handle;
        byte[]? oldBytes = ReadPropertyBytes(entity, comp.TypeHash, prop.Name, prop.Size);
        prop.WriteToEngine(entity, comp.TypeHash);
        _engine.MarkSceneDirty();
        byte[]? newBytes = ReadPropertyBytes(entity, comp.TypeHash, prop.Name, prop.Size);
        if (oldBytes != null && newBytes != null &&
            !System.Linq.Enumerable.SequenceEqual(oldBytes, newBytes))
        {
            PushUndo(new PropertyAction(this, entity, comp.TypeHash, prop.Name, oldBytes, newBytes));
        }
    }

    /// <summary>实时写入组件反射属性（不推 undo），供 Inspector 输入/滚轮实时预览。</summary>


    /// <summary>实时写入组件反射属性（不推 undo），供 Inspector 输入/滚轮实时预览。</summary>
    public void WritePropertyLive(ComponentModel comp, PropertyModel prop)
    {
        if (_selectedEntity == null) return;
        prop.WriteToEngine(_selectedEntity.Handle, comp.TypeHash);
        _engine.MarkSceneDirty();
    }

    /// <summary>提交一次组件属性编辑：用聚焦时捕获的旧值推一条 undo。</summary>


    /// <summary>提交一次组件属性编辑：用聚焦时捕获的旧值推一条 undo。</summary>
    public void FlushPropertyUndo(ComponentModel comp, PropertyModel prop, byte[]? baseline)
    {
        if (_selectedEntity == null) return;
        if (baseline == null || baseline.Length == 0) { WritePropertyValue(comp, prop); return; }
        byte[]? cur = ReadPropertyBytes(_selectedEntity.Handle, comp.TypeHash, prop.Name, prop.Size);
        if (cur != null && !System.Linq.Enumerable.SequenceEqual(baseline, cur))
            PushUndo(new PropertyAction(this, _selectedEntity.Handle, comp.TypeHash, prop.Name, baseline, cur));
    }

    /// <summary>Reads a raw reflection property value from the engine.</summary>


    /// <summary>Reads a raw reflection property value from the engine.</summary>
    internal static byte[]? ReadPropertyBytes(GEntityHandle entity, ulong typeHash, string propName, int size)
    {
        int len = Math.Max(size, 4);
        var buf = new byte[len];
        var pin = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try
        {
            return ComponentAPI.GComponent_GetProperty(entity, typeHash, propName, pin.AddrOfPinnedObject(), len) == 0
                ? buf
                : null;
        }
        finally { pin.Free(); }
    }

    /// <summary>Writes a raw reflection property value to the engine.</summary>


    /// <summary>Writes a raw reflection property value to the engine.</summary>
    internal static void WritePropertyBytes(GEntityHandle entity, ulong typeHash, string propName, byte[] value)
    {
        var pin = GCHandle.Alloc(value, GCHandleType.Pinned);
        try
        {
            ComponentAPI.GComponent_SetProperty(entity, typeHash, propName, pin.AddrOfPinnedObject(), value.Length);
        }
        finally { pin.Free(); }
    }

    /// <summary>Adds a component without recording an undo entry.</summary>


    /// <summary>Adds a component without recording an undo entry.</summary>
    internal void AddComponentSilent(GEntityHandle entity, ulong typeHash)
    {
        if (entity == GEntityHandle.Null) return;
        if (ComponentAPI.GComponent_AddComponent(entity, typeHash) != 0) return;
        _engine.MarkSceneDirty();
        if (_selectedEntity?.Handle == entity) RefreshInspector();
    }

    /// <summary>Removes a component without recording an undo entry.</summary>


    /// <summary>Removes a component without recording an undo entry.</summary>
    internal void RemoveComponentSilent(GEntityHandle entity, ulong typeHash)
    {
        if (entity == GEntityHandle.Null) return;
        if (ComponentAPI.GComponent_RemoveComponent(entity, typeHash) != 0) return;
        _engine.MarkSceneDirty();
        if (_selectedEntity?.Handle == entity) RefreshInspector();
    }

    /// <summary>
    /// Captures the full state of one component (reflection fields + exposed
    /// script props) so it can be restored after an undo of "Remove Component".
    /// </summary>


    /// <summary>
    /// Captures the full state of one component (reflection fields + exposed
    /// script props) so it can be restored after an undo of "Remove Component".
    /// </summary>
    private static ComponentSnapshot CaptureComponentState(GEntityHandle entity, ulong typeHash, string typeName)
    {
        var snapshot = new ComponentSnapshot();
        int fieldCount = ComponentAPI.GComponent_GetPropertyCount(entity, typeHash);
        for (int i = 0; i < fieldCount; i++)
        {
            var sb = new StringBuilder(128);
            if (ComponentAPI.GComponent_GetPropertyInfo(entity, typeHash, i, sb, sb.Capacity,
                    out int propType, out int propSize) < 0)
                continue;
            int len = Math.Max(propSize, 4);
            var buf = new byte[len];
            var pin = GCHandle.Alloc(buf, GCHandleType.Pinned);
            try
            {
                if (ComponentAPI.GComponent_GetProperty(entity, typeHash, sb.ToString(),
                        pin.AddrOfPinnedObject(), len) == 0)
                {
                    snapshot.Fields.Add(new ComponentSnapshot.Field(sb.ToString(), len, buf));
                }
            }
            finally { pin.Free(); }
        }
        if (typeName == "Script")
        {
            int propCount = ScriptAPI.GetPropCount(entity);
            for (int i = 0; i < propCount; i++)
            {
                var info = ScriptAPI.GetPropInfo(entity, i);
                if (info == null) continue;
                if (info.Value.Type == 0)
                {
                    snapshot.ScriptProps.Add(new ComponentSnapshot.ScriptProp(
                        info.Value.Name, 0, ScriptAPI.GetPropFloat(entity, info.Value.Name), null));
                }
                else
                {
                    snapshot.ScriptProps.Add(new ComponentSnapshot.ScriptProp(
                        info.Value.Name, 1, null, ScriptAPI.GetPropString(entity, info.Value.Name)));
                }
            }
        }
        return snapshot;
    }

    /// <summary>
    /// Re-attaches a component (async) and queues the captured state so it is
    /// written back once the AddComponent command completes on the next frame.
    /// </summary>


    /// <summary>
    /// Re-attaches a component (async) and queues the captured state so it is
    /// written back once the AddComponent command completes on the next frame.
    /// </summary>
    internal void RestoreComponentState(GEntityHandle entity, ulong typeHash, ComponentSnapshot? snapshot)
    {
        if (entity == GEntityHandle.Null) return;
        if (ComponentAPI.GComponent_AddComponent(entity, typeHash) != 0) return;
        _pendingComponentRestores.Add((entity, typeHash, snapshot ?? new ComponentSnapshot(), 0));
        _engine.MarkSceneDirty();
    }

    /// <summary>Writes a captured snapshot onto an existing component.</summary>


    /// <summary>Writes a captured snapshot onto an existing component.</summary>
    private static void WriteComponentSnapshot(GEntityHandle entity, ulong typeHash, ComponentSnapshot snapshot)
    {
        foreach (var f in snapshot.Fields)
        {
            WritePropertyBytes(entity, typeHash, f.Name, f.Bytes);
        }
        foreach (var p in snapshot.ScriptProps)
        {
            if (p.Type == 0) ScriptAPI.SetPropFloat(entity, p.Name, p.Float ?? 0f);
            else ScriptAPI.SetPropString(entity, p.Name, p.String ?? string.Empty);
        }
    }

    /// <summary>
    /// Applies deferred component restores once the engine has processed the
    /// async AddComponent command. Script props retry until the script loads.
    /// </summary>


    /// <summary>
    /// Applies deferred component restores once the engine has processed the
    /// async AddComponent command. Script props retry until the script loads.
    /// </summary>
    private void ApplyPendingComponentRestores()
    {
        if (_pendingComponentRestores.Count == 0) return;
        bool refreshed = false;
        for (int i = _pendingComponentRestores.Count - 1; i >= 0; i--)
        {
            var entry = _pendingComponentRestores[i];
            if (entry.Entity == GEntityHandle.Null) { _pendingComponentRestores.RemoveAt(i); continue; }
            if (!HasComponent(entry.Entity, entry.TypeHash)) continue; // command not processed yet

            bool scriptReady = entry.Snapshot.ScriptProps.Count == 0 ||
                               ScriptAPI.GetPropCount(entry.Entity) > 0;
            if (!scriptReady)
            {
                int retries = entry.Retries + 1;
                if (retries >= 240) _pendingComponentRestores.RemoveAt(i);
                else _pendingComponentRestores[i] = (entry.Entity, entry.TypeHash, entry.Snapshot, retries);
                continue;
            }

            WriteComponentSnapshot(entry.Entity, entry.TypeHash, entry.Snapshot);
            _pendingComponentRestores.RemoveAt(i);
            refreshed = true;
        }
        if (refreshed && _selectedEntity != null) RefreshInspector();
    }



    private static bool HasComponent(GEntityHandle entity, ulong typeHash)
    {
        int count = ComponentAPI.GComponent_GetCount(entity);
        for (int i = 0; i < count; i++)
        {
            if (ComponentAPI.GComponent_GetTypeHashAt(entity, i, out var hash) == 0 && hash == typeHash)
                return true;
        }
        return false;
    }

    /// <summary>
    /// Pushes a Transform undo action for a gizmo drag (called at drag end with
    /// the values captured at drag start).
    /// </summary>


    /// <summary>
    /// Pushes a Transform undo action for a gizmo drag (called at drag end with
    /// the values captured at drag start).
    /// </summary>
    public void PushTransformAction(GEntityHandle handle, GVec3 oldPos, GQuat oldRot, GVec3 oldScale)
    {
        if (handle == GEntityHandle.Null) return;
        if (EntityAPI.GEntity_GetLocalPosition(handle, out var newPos) != 0) return;
        if (EntityAPI.GEntity_GetLocalRotation(handle, out var newRot) != 0) return;
        if (EntityAPI.GEntity_GetLocalScale(handle, out var newScale) != 0) return;
        if (Vec3Close(oldPos, newPos) && QuatClose(oldRot, newRot) && Vec3Close(oldScale, newScale)) return;
        PushUndo(new TransformAction(this, handle, oldPos, oldRot, oldScale, newPos, newRot, newScale));
    }



    private static bool Vec3Close(GVec3 a, GVec3 b)
        => Math.Abs(a.X - b.X) < 1e-5f && Math.Abs(a.Y - b.Y) < 1e-5f && Math.Abs(a.Z - b.Z) < 1e-5f;



    private static bool QuatClose(GQuat a, GQuat b)
        => Math.Abs(a.X - b.X) < 1e-5f && Math.Abs(a.Y - b.Y) < 1e-5f &&
           Math.Abs(a.Z - b.Z) < 1e-5f && Math.Abs(a.W - b.W) < 1e-5f;


}
