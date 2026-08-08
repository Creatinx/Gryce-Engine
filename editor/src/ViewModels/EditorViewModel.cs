using GryceEngine.Editor.Models;
using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text;
using System.Windows.Input;

namespace GryceEngine.Editor.ViewModels;

/// <summary>Main editor ViewModel that drives Hierarchy, Inspector, and general state.</summary>
public class EditorViewModel : INotifyPropertyChanged
{
    private readonly EngineService _engine;

    // Hierarchy
    public ObservableCollection<EntityModel> RootEntities { get; } = new();

    // Inspector
    private EntityModel? _selectedEntity;
    public EntityModel? SelectedEntity
    {
        get => _selectedEntity;
        set
        {
            if (_selectedEntity != value)
            {
                _selectedEntity = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(HasSelection));
                RefreshInspector();
            }
        }
    }

    public bool HasSelection => _selectedEntity != null;

    /// <summary>Selected entity has a MeshRenderer / SkinnedMeshRenderer (material editable).</summary>
    public bool HasRendererComponent => _selectedEntity?.Components != null &&
        System.Linq.Enumerable.Any(_selectedEntity.Components,
            c => c.TypeName is "MeshRenderer" or "SkinnedMeshRenderer");

    // Registered component types for Add Component dropdown
    public ObservableCollection<RegisteredTypeItem> RegisteredTypes { get; } = new();

    // Toolbar
    private bool _isPlaying;
    public bool IsPlaying
    {
        get => _isPlaying;
        set { _isPlaying = value; OnPropertyChanged(); OnPropertyChanged(nameof(IsStopped)); OnPropertyChanged(nameof(StatusText)); }
    }

    private bool _isPaused;
    public bool IsPaused
    {
        get => _isPaused;
        set { _isPaused = value; OnPropertyChanged(); OnPropertyChanged(nameof(StatusText)); }
    }

    public bool IsStopped => !_isPlaying;

    // Gizmo state
    private string _gizmoMode = "Translate";
    private bool _gizmoLocal = true;

    public string GizmoMode
    {
        get => _gizmoMode;
        set
        {
            _gizmoMode = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(GizmoModeLabel));
        }
    }

    public string GizmoModeLabel => LocalizationService.Instance.T("status.gizmo") + ": " + GizmoMode;

    public bool IsGizmoLocal
    {
        get => _gizmoLocal;
        set { _gizmoLocal = value; OnPropertyChanged(); }
    }

    // Title
    private string _title = "Gryce Engine Editor";
    public string Title
    {
        get => _title;
        set { _title = value; OnPropertyChanged(); }
    }

    // Status Bar
    public string StatusText => _isPlaying
        ? LocalizationService.Instance.T(_isPaused ? "status.paused" : "status.playing")
        : LocalizationService.Instance.T("status.ready");
    public int EntityCount => EntityAPI.GEntity_GetCount();

    // Console
    public ObservableCollection<LogEntry> LogEntries { get; } = new();

    private string _consoleText = string.Empty;
    public string ConsoleText
    {
        get => _consoleText;
        set { _consoleText = value; OnPropertyChanged(); }
    }

    // Scene tracking
    private string _sceneName = "Untitled";
    public string SceneName
    {
        get => _sceneName;
        set { _sceneName = value; OnPropertyChanged(); }
    }

    // Theme
    private string _currentTheme = "Dark";
    public string CurrentTheme
    {
        get => _currentTheme;
        set { _currentTheme = value; OnPropertyChanged(); }
    }

    // Commands
    public ICommand PlayCommand { get; }
    public ICommand PauseCommand { get; }
    public ICommand StopCommand { get; }
    public ICommand TogglePlayModeCommand { get; }
    public ICommand NewSceneCommand { get; }
    public ICommand SaveSceneCommand { get; }
    public ICommand CreateEntityCommand { get; }
    public ICommand DeleteEntityCommand { get; }
    public ICommand DuplicateCommand { get; }
    public ICommand CutCommand { get; }
    public ICommand CopyCommand { get; }
    public ICommand PasteCommand { get; }
    public ICommand UndoCommand { get; }
    public ICommand RedoCommand { get; }
    public ICommand RenameCommand { get; }
    public ICommand RefreshCommand { get; }
    public ICommand FocusCommand { get; }
    public ICommand GizmoTranslateCommand { get; }
    public ICommand GizmoRotateCommand { get; }
    public ICommand GizmoScaleCommand { get; }
    public ICommand GizmoToggleSpaceCommand { get; }

    // Clipboard for Cut/Copy/Paste
    private GEntityHandle _clipboardEntity = GEntityHandle.Null;
    private string _clipboardEntityName = string.Empty;
    private bool _clipboardIsCut;

    // Pending component attach after CreateEntityWithComponent
    private string? _pendingComponentForNewEntity;
    private string? _pendingNewEntityName;
    private string? _pendingSelectEntityName;

    // Undo/Redo stacks
    private readonly Stack<IUndoableAction> _undoStack = new();
    private readonly Stack<IUndoableAction> _redoStack = new();

    // Callback delegates (kept alive to prevent GC)
    private readonly GOnEntityListChanged _onEntityListChanged;
    private readonly GOnEntitySelected _onEntitySelected;
    private readonly GOnEntityDeselected _onEntityDeselected;
    private readonly GOnPlayModeChanged _onPlayModeChanged;
    private readonly GOnLogMessage _onLogMessage;
    private readonly GOnSceneLoaded _onSceneLoaded;

    public EditorViewModel(EngineService engine)
    {
        _engine = engine;

        PlayCommand = new RelayCommand(Play);
        PauseCommand = new RelayCommand(Pause);
        StopCommand = new RelayCommand(Stop);
        TogglePlayModeCommand = new RelayCommand(TogglePlayMode);
        NewSceneCommand = new RelayCommand(NewScene);
        SaveSceneCommand = new RelayCommand(SaveScene);
        CreateEntityCommand = new RelayCommand(CreateEntity);
        DeleteEntityCommand = new RelayCommand(DeleteSelectedEntity);
        DuplicateCommand = new RelayCommand(DuplicateSelectedEntity);
        CutCommand = new RelayCommand(CutSelectedEntity, () => SelectedEntity != null);
        CopyCommand = new RelayCommand(CopySelectedEntity, () => SelectedEntity != null);
        PasteCommand = new RelayCommand(PasteToSelected, () => _clipboardEntity != GEntityHandle.Null);
        UndoCommand = new RelayCommand(Undo, () => _undoStack.Count > 0);
        RedoCommand = new RelayCommand(Redo, () => _redoStack.Count > 0);
        RenameCommand = new RelayCommand(RenameSelectedEntity);
        RefreshCommand = new RelayCommand(() =>
        {
            RefreshHierarchy();
            RefreshInspector();
            OnPropertyChanged(nameof(EntityCount));
        });
        FocusCommand = new RelayCommand(FocusSelectedEntity);
        GizmoTranslateCommand = new RelayCommand(() => SetGizmoMode("Translate"));
        GizmoRotateCommand = new RelayCommand(() => SetGizmoMode("Rotate"));
        GizmoScaleCommand = new RelayCommand(() => SetGizmoMode("Scale"));
        GizmoToggleSpaceCommand = new RelayCommand(() =>
        {
            IsGizmoLocal = !IsGizmoLocal;
            PushGizmoCommand(IsGizmoLocal ? "Local" : "Global");
        });

        // Create delegates and register callbacks
        _onEntityListChanged = _ =>
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
            {
                RefreshHierarchy();
                AttachPendingComponentToNewEntity();
                SelectPendingNewEntity();
                OnPropertyChanged(nameof(EntityCount));
            });
        };
        _onEntitySelected = (entity, _) => System.Windows.Application.Current.Dispatcher.Invoke(() => SelectEntityByHandle(entity));
        _onEntityDeselected = _ => System.Windows.Application.Current.Dispatcher.Invoke(() => SelectedEntity = null);
        _onPlayModeChanged = (playing, paused, _) =>
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
            {
                IsPlaying = playing;
                IsPaused = paused;
            });
        };
        _onLogMessage = (level, msg, _) =>
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
                AppendConsole($"[{level}] {msg}"));
        };
        _onSceneLoaded = (path, _) =>
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
            {
                SceneName = System.IO.Path.GetFileNameWithoutExtension(path ?? "Untitled");
                AppendConsole($"Scene loaded: {path}");
            });
        };

        CoreAPI.GCore_SetCallback_UserData(IntPtr.Zero);
        CoreAPI.GCore_RegisterCallback_OnEntityListChanged(_onEntityListChanged);
        CoreAPI.GCore_RegisterCallback_OnEntitySelected(_onEntitySelected);
        CoreAPI.GCore_RegisterCallback_OnEntityDeselected(_onEntityDeselected);
        CoreAPI.GCore_RegisterCallback_OnPlayModeChanged(_onPlayModeChanged);
        CoreAPI.GCore_RegisterCallback_OnLogMessage(_onLogMessage);
        CoreAPI.GCore_RegisterCallback_OnSceneLoaded(_onSceneLoaded);

        engine.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(EngineService.IsPlaying)) IsPlaying = engine.IsPlaying;
            if (e.PropertyName == nameof(EngineService.IsPaused)) IsPaused = engine.IsPaused;
        };

        LocalizationService.Instance.LanguageChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(StatusText));
            OnPropertyChanged(nameof(GizmoModeLabel));
        };

        RefreshRegisteredTypes();
    }

    // === Toolbar actions ===

    public void Play() => _engine.Play();
    public void Pause() => _engine.Pause();
    public void Stop() => _engine.Stop();

    public void NewScene()
    {
        SceneAPI.GScene_New();
        AppendConsole("New scene created.");
        _undoStack.Clear();
        _redoStack.Clear();
        OnPropertyChanged(nameof(EntityCount));
    }

    public void SaveScene()
    {
        int result = SceneAPI.GScene_Save("res:/scenes/main.gesc");
        if (result == 0)
            AppendConsole("Scene saved.");
        else
            AppendConsole("Failed to save scene.");
    }

    public void CreateEntity()
    {
        CreateEntity(LocalizationService.Instance.T("hierarchy.new_entity_name"));
    }

    /// <summary>
    /// Creates an entity via the C API command buffer.
    /// Payload layout matches the C++ side: { char name[128]; int parent; }.
    /// </summary>
    public void CreateEntity(string name, GEntityHandle parent = GEntityHandle.Null)
    {
        Span<byte> payload = stackalloc byte[128 + sizeof(int)];
        var nameBytes = Encoding.UTF8.GetBytes(string.IsNullOrWhiteSpace(name) ? "Entity" : name);
        if (nameBytes.Length > 128) nameBytes = nameBytes.AsSpan(0, 128).ToArray();
        nameBytes.AsSpan().CopyTo(payload);
        BitConverterCompat.TryWriteBytes(payload.Slice(128), (int)parent);

        var cmd = GCommand.Create(GCommandType.CreateEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _undoStack.Push(new CreateEntityAction(this));
        _redoStack.Clear();
        AppendConsole($"Entity created: {name}");
        _pendingSelectEntityName = name;
    }

    /// <summary>Creates an entity as a child of the given parent.</summary>
    public void CreateChildEntity(GEntityHandle parent)
    {
        CreateEntity(LocalizationService.Instance.T("hierarchy.new_entity_name"), parent);
    }

    /// <summary>
    /// Creates an entity and attaches a component on the next engine frame.
    /// The component is resolved through the registered type table by its short name.
    /// </summary>
    public void CreateEntityWithComponent(string name, GEntityHandle parent, string componentTypeName)
    {
        CreateEntity(name, parent);
        if (!string.IsNullOrEmpty(componentTypeName))
        {
            _pendingComponentForNewEntity = componentTypeName;
            _pendingNewEntityName = name;
        }
    }

    public void DeleteSelectedEntity()
    {
        if (_selectedEntity == null) return;
        var handle = _selectedEntity.Handle;
        var name = _selectedEntity.Name;
        Span<byte> payload = stackalloc byte[sizeof(int)];
        BitConverterCompat.TryWriteBytes(payload, (int)handle);
        var cmd = GCommand.Create(GCommandType.DestroyEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _undoStack.Push(new DeleteEntityAction(this, handle, name));
        _redoStack.Clear();
        AppendConsole($"Deleted entity: {name}");
    }

    public void DuplicateSelectedEntity()
    {
        if (_selectedEntity == null) return;
        var name = _selectedEntity.Name;
        var parent = EntityAPI.GEntity_GetParent(_selectedEntity.Handle);
        string copyName = name + LocalizationService.Instance.T("hierarchy.duplicate_suffix");
        CreateEntity(copyName, parent);
        AppendConsole($"Duplicated: {name}");
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
            var parentHandle = _selectedEntity?.Handle ?? GEntityHandle.Null;
            Span<byte> payload = stackalloc byte[sizeof(int) * 2];
            BitConverterCompat.TryWriteBytes(payload, (int)_clipboardEntity);
            BitConverterCompat.TryWriteBytes(payload.Slice(sizeof(int)), (int)parentHandle);
            var cmd = GCommand.Create(GCommandType.ReparentEntity, payload);
            CoreAPI.GCore_PushCommand(ref cmd);
            _clipboardEntity = GEntityHandle.Null;
            _clipboardEntityName = string.Empty;
            _clipboardIsCut = false;
            AppendConsole("Pasted entity (cut).");
        }
        else
        {
            // For copy, create a named copy under the selected parent (or root)
            var parentHandle = _selectedEntity?.Handle ?? GEntityHandle.Null;
            CreateEntity(_clipboardEntityName, parentHandle);
            AppendConsole($"Pasted: {_clipboardEntityName}");
        }
    }

    public void TogglePlayMode()
    {
        if (IsPlaying)
            Stop();
        else
            Play();
    }

    public void SetSceneName(string name)
    {
        SceneName = name;
        AppendConsole($"Scene: {name}");
    }

    public void SetGizmoMode(string mode)
    {
        GizmoMode = mode;
        PushGizmoCommand(mode);
        AppendConsole($"Gizmo: {mode}");
    }

    private void PushGizmoCommand(string mode)
    {
        Span<byte> payload = stackalloc byte[256];
        var modeBytes = Encoding.UTF8.GetBytes(mode);
        modeBytes.AsSpan().CopyTo(payload);
        var cmd = GCommand.Create(GCommandType.GizmoSetOperation, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
    }

    public void RenameSelectedEntity()
    {
        if (_selectedEntity == null) return;
        var entity = _selectedEntity;
        var dialog = new Views.InputDialog("Rename Entity", "Enter new name:", entity.Name);
        dialog.Owner = System.Windows.Application.Current.MainWindow;
        if (dialog.ShowDialog() == true && !string.IsNullOrWhiteSpace(dialog.InputText))
        {
            var oldName = entity.Name;
            RenameEntity(entity.Handle, dialog.InputText);
            _undoStack.Push(new RenameEntityAction(this, entity.Handle, oldName, dialog.InputText));
            _redoStack.Clear();
        }
    }

    // === Undo/Redo ===

    private void Undo()
    {
        if (_undoStack.Count == 0) return;
        var action = _undoStack.Pop();
        action.Undo();
        _redoStack.Push(action);
        AppendConsole("Undo: " + action.Description);
    }

    private void Redo()
    {
        if (_redoStack.Count == 0) return;
        var action = _redoStack.Pop();
        action.Execute();
        _undoStack.Push(action);
        AppendConsole("Redo: " + action.Description);
    }

    // === Hierarchy ===

    public void RefreshHierarchy()
    {
        RootEntities.Clear();
        int count = EntityAPI.GEntity_GetCount();
        for (int i = 0; i < count; i++)
        {
            var handle = EntityAPI.GEntity_GetAt(i);
            if (handle == GEntityHandle.Null) continue;

            var sb = new StringBuilder(256);
            if (EntityAPI.GEntity_GetName(handle, sb, sb.Capacity) < 0) continue;

            var parent = EntityAPI.GEntity_GetParent(handle);
            if (parent == GEntityHandle.Null)
            {
                var model = BuildEntityTree(handle, sb.ToString());
                RootEntities.Add(model);
            }
        }
    }

    private EntityModel BuildEntityTree(GEntityHandle handle, string name)
    {
        var model = new EntityModel(handle, name);
        model.RefreshTransform();

        int childCount = EntityAPI.GEntity_GetChildCount(handle);
        for (int i = 0; i < childCount; i++)
        {
            var childHandle = EntityAPI.GEntity_GetChildAt(handle, i);
            if (childHandle == GEntityHandle.Null) continue;
            var sb = new StringBuilder(256);
            if (EntityAPI.GEntity_GetName(childHandle, sb, sb.Capacity) >= 0)
            {
                model.Children.Add(BuildEntityTree(childHandle, sb.ToString()));
            }
        }
        return model;
    }

    public void SelectEntityByHandle(GEntityHandle handle)
    {
        if (handle == GEntityHandle.Null)
        {
            SelectedEntity = null;
            return;
        }

        var path = FindEntityPath(handle, RootEntities, new List<EntityModel>());
        if (path != null && path.Count > 0)
        {
            // 展开到该实体的整条祖先链，确保嵌套节点可见。
            foreach (var e in path) e.IsExpanded = true;
            var entity = path[path.Count - 1];
            if (_selectedEntity != null)
                _selectedEntity.IsSelected = false;

            entity.IsSelected = true;
            SelectedEntity = entity;
        }
    }

    /// <summary>
    /// Refreshes the selected entity's transform + inspector after the viewport
    /// gizmo modifies it, so Inspector and node stay in sync.
    /// </summary>
    public void RaiseTransformChanged(GEntityHandle handle)
    {
        if (_selectedEntity != null && _selectedEntity.Handle == handle)
        {
            _selectedEntity.RefreshTransform();
            RefreshInspector();
            OnPropertyChanged(nameof(SelectedEntity));
        }
    }

    private void SelectPendingNewEntity()
    {
        var name = _pendingSelectEntityName;
        _pendingSelectEntityName = null;
        if (name == null) return;
        var created = FindNewestEntityByName(name, RootEntities);
        if (created != null) SelectEntityByHandle(created.Handle);
    }

    private static EntityModel? FindEntity(GEntityHandle handle, ObservableCollection<EntityModel> entities)
    {
        foreach (var e in entities)
        {
            if (e.Handle == handle) return e;
            var found = FindEntity(handle, e.Children);
            if (found != null) return found;
        }
        return null;
    }

    private static List<EntityModel>? FindEntityPath(
        GEntityHandle handle, ObservableCollection<EntityModel> entities, List<EntityModel> path)
    {
        foreach (var e in entities)
        {
            path.Add(e);
            if (e.Handle == handle) return path;
            var found = FindEntityPath(handle, e.Children, path);
            if (found != null) return found;
            path.RemoveAt(path.Count - 1);
        }
        return null;
    }

    /// <summary>
    /// After the create-entity command is processed by the engine, attach the
    /// component requested by CreateEntityWithComponent to the newest entity
    /// matching the requested name.
    /// </summary>
    private void AttachPendingComponentToNewEntity()
    {
        var typeName = _pendingComponentForNewEntity;
        var entityName = _pendingNewEntityName;
        _pendingComponentForNewEntity = null;
        _pendingNewEntityName = null;
        if (typeName == null || entityName == null) return;

        var created = FindNewestEntityByName(entityName, RootEntities);
        if (created == null) return;

        ulong hash = 0;
        foreach (var t in RegisteredTypes)
        {
            if (t.TypeName == typeName) { hash = t.TypeHash; break; }
        }
        if (hash != 0)
        {
            int result = ComponentAPI.GComponent_AddComponent(created.Handle, hash);
            if (result == 0)
            {
                AppendConsole($"Component '{typeName}' added to '{created.Name}'");
                if (_selectedEntity?.Handle == created.Handle) RefreshInspector();
            }
            else
            {
                AppendConsole($"Failed to add component '{typeName}'");
            }
        }
    }

    private static EntityModel? FindNewestEntityByName(string name, ObservableCollection<EntityModel> roots)
    {
        EntityModel? newest = null;
        foreach (var e in roots)
        {
            if (e.Name == name && (newest == null || e.Handle > newest.Handle))
                newest = e;
            var child = FindNewestEntityByName(name, e.Children);
            if (child != null && (newest == null || child.Handle > newest.Handle))
                newest = child;
        }
        return newest;
    }

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

    public void AddComponent(ulong typeHash)
    {
        if (_selectedEntity == null) return;
        int result = ComponentAPI.GComponent_AddComponent(_selectedEntity.Handle, typeHash);
        if (result == 0)
        {
            AppendConsole($"Added component to '{_selectedEntity.Name}'");
            RefreshInspector();
        }
        else
        {
            AppendConsole($"Failed to add component to '{_selectedEntity.Name}'");
        }
    }

    public void RemoveComponent(ulong typeHash)
    {
        if (_selectedEntity == null) return;
        int result = ComponentAPI.GComponent_RemoveComponent(_selectedEntity.Handle, typeHash);
        if (result == 0)
        {
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
        var pos = _selectedEntity.LocalPosition;
        var rot = _selectedEntity.LocalRotation;
        var scl = _selectedEntity.LocalScale;

        Span<byte> payload = stackalloc byte[sizeof(int) + 3 * 4 * sizeof(float)];
        int offset = 0;
        BitConverterCompat.TryWriteBytes(payload.Slice(offset), (int)_selectedEntity.Handle);
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
    }

    public void RefreshInspector()
    {
        if (_selectedEntity == null) return;
        _selectedEntity.RefreshTransform();
        _selectedEntity.RefreshComponents();
        foreach (var comp in _selectedEntity.Components)
        {
            comp.RefreshProperties();
        }
        OnPropertyChanged(nameof(HasRendererComponent));
    }

    public void WritePropertyValue(ComponentModel comp, PropertyModel prop)
    {
        if (_selectedEntity == null) return;
        prop.WriteToEngine(_selectedEntity.Handle, comp.TypeHash);
    }

    public void RenameEntity(GEntityHandle handle, string newName)
    {
        Span<byte> payload = stackalloc byte[sizeof(int) + 128];
        BitConverterCompat.TryWriteBytes(payload, (int)handle);
        var nameBytes = Encoding.UTF8.GetBytes(newName);
        nameBytes.AsSpan().CopyTo(payload.Slice(sizeof(int)));
        var cmd = GCommand.Create(GCommandType.RenameEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
    }

    // === Console ===

    public void AppendConsole(string text, LogLevel level = LogLevel.Info)
    {
        ConsoleText += text + Environment.NewLine;
        LogEntries.Add(new LogEntry(level, text));
    }

    public void ClearConsole()
    {
        ConsoleText = string.Empty;
        LogEntries.Clear();
    }

    // === Dispose ===

    public void DetachCallbacks()
    {
        CoreAPI.GCore_RegisterCallback_OnEntityListChanged(null!);
        CoreAPI.GCore_RegisterCallback_OnEntitySelected(null!);
        CoreAPI.GCore_RegisterCallback_OnEntityDeselected(null!);
        CoreAPI.GCore_RegisterCallback_OnPlayModeChanged(null!);
        CoreAPI.GCore_RegisterCallback_OnLogMessage(null!);
        CoreAPI.GCore_RegisterCallback_OnSceneLoaded(null!);
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}

// === Undo/Redo Action Types ===

internal interface IUndoableAction
{
    string Description { get; }
    void Execute();
    void Undo();
}

internal class CreateEntityAction(EditorViewModel vm) : IUndoableAction
{
    public string Description => "Create Entity";
    public void Execute() => vm.CreateEntity();
    public void Undo() => vm.DeleteSelectedEntity();
}

internal class DeleteEntityAction(EditorViewModel vm, GEntityHandle handle, string name) : IUndoableAction
{
    public string Description => $"Delete '{name}'";
    public void Execute()
    {
        Span<byte> payload = stackalloc byte[sizeof(int)];
        BitConverterCompat.TryWriteBytes(payload, (int)handle);
        var cmd = GCommand.Create(GCommandType.DestroyEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
    }
    public void Undo()
    {
        vm.CreateEntity();
    }
}

internal class RenameEntityAction(EditorViewModel vm, GEntityHandle handle, string oldName, string newName) : IUndoableAction
{
    public string Description => $"Rename to '{newName}'";
    public void Execute() => vm.RenameEntity(handle, newName);
    public void Undo() => vm.RenameEntity(handle, oldName);
}
