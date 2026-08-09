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

    /// <summary>Selected entity is a Prefab instance root (Apply/Revert enabled).</summary>
    public bool HasPrefabInstance => _selectedEntity?.Components != null &&
        System.Linq.Enumerable.Any(_selectedEntity.Components,
            c => c.TypeName == "PrefabInstance");

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
    public ICommand BackCommand { get; }
    public ICommand ForwardCommand { get; }

    // Selection navigation history (title-bar Back / Forward buttons)
    private readonly List<GEntityHandle> _selectionHistory = new();
    private int _historyIndex = -1;
    private bool _historySuppress;

    public bool CanNavigateBack => _historyIndex > 0;
    public bool CanNavigateForward => _historyIndex >= 0 && _historyIndex < _selectionHistory.Count - 1;

    // Clipboard for Cut/Copy/Paste
    private GEntityHandle _clipboardEntity = GEntityHandle.Null;
    private string _clipboardEntityName = string.Empty;
    private bool _clipboardIsCut;

    // Pending component attach after CreateEntityWithComponent
    private string? _pendingComponentForNewEntity;
    private string? _pendingNewEntityName;
    private string? _pendingSelectEntityName;
    // When set, after the pending entity is created and selected the editor
    // opens the Add-Component picker so creation flows straight into it.
    private bool _pendingOpenComponentPicker;

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
        BackCommand = new RelayCommand(NavigateBack, () => CanNavigateBack);
        ForwardCommand = new RelayCommand(NavigateForward, () => CanNavigateForward);
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
                ApplyPendingModelSetup();
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
        _onLogMessage = (level, msg, sourceFile, sourceLine, _) =>
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
                AppendConsole($"[{level}] {msg}", (LogLevel)level, sourceFile, sourceLine));
        };
        _onSceneLoaded = (path, _) =>
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
            {
                SceneName = System.IO.Path.GetFileNameWithoutExtension(path ?? "Untitled");
                AppendConsole($"Scene loaded: {path}");
                _engine.ClearDirty();
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
        engine.LogMessage += msg => AppendConsole(msg);
        engine.ProjectChanged += root =>
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
            {
                _undoStack.Clear();
                _redoStack.Clear();
                RefreshHierarchy();
                RefreshInspector();
                OnPropertyChanged(nameof(EntityCount));
                int rc = SceneAPI.GScene_Load("res:/scenes/editor_default.gesc");
                if (rc != 0) rc = SceneAPI.GScene_Load("res:/scenes/main.gesc");
                if (rc != 0) SceneAPI.GScene_New();
                AppendConsole($"Project switched: {root}");
            });
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
        _engine.ClearDirty();
        _undoStack.Clear();
        _redoStack.Clear();
        OnPropertyChanged(nameof(EntityCount));
    }

    public void SaveScene()
    {
        int result = SceneAPI.GScene_Save("res:/scenes/main.gesc");
        if (result == 0)
        {
            AppendConsole("Scene saved.");
            _engine.ClearDirty();
        }
        else
            AppendConsole("Failed to save scene.");
    }

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
        _engine.MarkSceneDirty();
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
        if (_selectedEntity == null) return;
        var handle = _selectedEntity.Handle;
        var name = _selectedEntity.Name;
        var parent = EntityAPI.GEntity_GetParent(handle);
        string copyName = name + LocalizationService.Instance.T("hierarchy.duplicate_suffix");
        string? json = EntityAPI.ExportJsonUtf8(handle);
        if (!string.IsNullOrEmpty(json))
        {
            var newHandle = EntityAPI.GEntity_ImportJson(json!, parent);
            if (newHandle != GEntityHandle.Null)
            {
                _engine.MarkSceneDirty();
                _undoStack.Push(new DeleteEntityAction(this, newHandle, copyName, null, parent));
                _redoStack.Clear();
                AppendConsole($"Duplicated: {name}");
                SelectEntityByHandle(newHandle);
                return;
            }
        }
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
            var oldParent = EntityAPI.GEntity_GetParent(_clipboardEntity);
            var parentHandle = _selectedEntity?.Handle ?? GEntityHandle.Null;
            Span<byte> payload = stackalloc byte[sizeof(int) * 2];
            BitConverterCompat.TryWriteBytes(payload, (int)_clipboardEntity);
            BitConverterCompat.TryWriteBytes(payload.Slice(sizeof(int)), (int)parentHandle);
            var cmd = GCommand.Create(GCommandType.ReparentEntity, payload);
            CoreAPI.GCore_PushCommand(ref cmd);
            _engine.MarkSceneDirty();
            _undoStack.Push(new ReparentAction(_clipboardEntity, oldParent, parentHandle));
            _redoStack.Clear();
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
                    _undoStack.Push(new DeleteEntityAction(this, newHandle, _clipboardEntityName, null, parentHandle));
                    _redoStack.Clear();
                    AppendConsole($"Pasted: {_clipboardEntityName}");
                    SelectEntityByHandle(newHandle);
                    return;
                }
            }
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
        if (ModalDialog.Show(dialog, System.Windows.Application.Current.MainWindow) == true &&
            !string.IsNullOrWhiteSpace(dialog.InputText))
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
        _engine.MarkSceneDirty();
        AppendConsole("Undo: " + action.Description);
    }

    private void Redo()
    {
        if (_redoStack.Count == 0) return;
        var action = _redoStack.Pop();
        action.Execute();
        _undoStack.Push(action);
        _engine.MarkSceneDirty();
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

            var entityName = EntityAPI.GetNameUtf8(handle);
            if (entityName == null) continue;

            var parent = EntityAPI.GEntity_GetParent(handle);
            if (parent == GEntityHandle.Null)
            {
                var model = BuildEntityTree(handle, entityName);
                RootEntities.Add(model);
            }
        }
    }

    private EntityModel BuildEntityTree(GEntityHandle handle, string name)
    {
        var model = new EntityModel(handle, name);
        model.RefreshTransform();
        model.RefreshComponents();

        int childCount = EntityAPI.GEntity_GetChildCount(handle);
        for (int i = 0; i < childCount; i++)
        {
            var childHandle = EntityAPI.GEntity_GetChildAt(handle, i);
            if (childHandle == GEntityHandle.Null) continue;
            var childName = EntityAPI.GetNameUtf8(childHandle);
            if (childName != null)
            {
                model.Children.Add(BuildEntityTree(childHandle, childName));
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

            if (!_historySuppress)
            {
                // Record the selection in the Back/Forward navigation history
                // (trim any forward entries first, browser-style).
                if (_historyIndex >= 0 && _historyIndex < _selectionHistory.Count - 1)
                {
                    _selectionHistory.RemoveRange(
                        _historyIndex + 1, _selectionHistory.Count - _historyIndex - 1);
                }
                _selectionHistory.Add(handle);
                _historyIndex = _selectionHistory.Count - 1;
                OnPropertyChanged(nameof(CanNavigateBack));
                OnPropertyChanged(nameof(CanNavigateForward));
            }
        }
    }

    /// <summary>Goes back to the previously selected entity.</summary>
    public void NavigateBack()
    {
        if (!CanNavigateBack) return;
        _historyIndex--;
        _historySuppress = true;
        try { SelectEntityByHandle(_selectionHistory[_historyIndex]); }
        finally { _historySuppress = false; }
        OnPropertyChanged(nameof(CanNavigateBack));
        OnPropertyChanged(nameof(CanNavigateForward));
        System.Windows.Input.CommandManager.InvalidateRequerySuggested();
    }

    /// <summary>Goes forward to the next selected entity in the history.</summary>
    public void NavigateForward()
    {
        if (!CanNavigateForward) return;
        _historyIndex++;
        _historySuppress = true;
        try { SelectEntityByHandle(_selectionHistory[_historyIndex]); }
        finally { _historySuppress = false; }
        OnPropertyChanged(nameof(CanNavigateBack));
        OnPropertyChanged(nameof(CanNavigateForward));
        System.Windows.Input.CommandManager.InvalidateRequerySuggested();
    }

    /// <summary>
    /// Refreshes the selected entity's transform + inspector after the viewport
    /// gizmo modifies it, so Inspector and node stay in sync.
    /// </summary>
    public void RaiseTransformChanged(GEntityHandle handle)
    {
        _engine.MarkSceneDirty();
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
        if (created == null) return;

        SelectEntityByHandle(created.Handle);
        if (_pendingOpenComponentPicker)
        {
            _pendingOpenComponentPicker = false;
            // Defer to the next dispatch so the entity-list callback completes
            // before a modal dialog re-enters the message loop.
            var vm = this;
            System.Windows.Application.Current.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (vm.SelectedEntity == null) return;
                ModalDialog.Show(new Views.AddComponentDialog(vm, renameEntityToType: true),
                                 System.Windows.Application.Current.MainWindow);
            }), System.Windows.Threading.DispatcherPriority.Normal);
        }
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
                _engine.MarkSceneDirty();
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

    public int AddComponent(ulong typeHash)
    {
        if (_selectedEntity == null) return -1;
        int result = ComponentAPI.GComponent_AddComponent(_selectedEntity.Handle, typeHash);
        if (result == 0)
        {
            _engine.MarkSceneDirty();
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
        int result = ComponentAPI.GComponent_RemoveComponent(_selectedEntity.Handle, typeHash);
        if (result == 0)
        {
            _engine.MarkSceneDirty();
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
        _engine.MarkSceneDirty();
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
        OnPropertyChanged(nameof(HasPrefabInstance));
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
            _undoStack.Push(new PropertyAction(this, entity, comp.TypeHash, prop.Name, oldBytes, newBytes));
            _redoStack.Clear();
        }
    }

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
    internal static void WritePropertyBytes(GEntityHandle entity, ulong typeHash, string propName, byte[] value)
    {
        var pin = GCHandle.Alloc(value, GCHandleType.Pinned);
        try
        {
            ComponentAPI.GComponent_SetProperty(entity, typeHash, propName, pin.AddrOfPinnedObject(), value.Length);
        }
        finally { pin.Free(); }
    }

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
        _undoStack.Push(new TransformAction(this, handle, oldPos, oldRot, oldScale, newPos, newRot, newScale));
        _redoStack.Clear();
    }

    private static bool Vec3Close(GVec3 a, GVec3 b)
        => Math.Abs(a.X - b.X) < 1e-5f && Math.Abs(a.Y - b.Y) < 1e-5f && Math.Abs(a.Z - b.Z) < 1e-5f;

    private static bool QuatClose(GQuat a, GQuat b)
        => Math.Abs(a.X - b.X) < 1e-5f && Math.Abs(a.Y - b.Y) < 1e-5f &&
           Math.Abs(a.Z - b.Z) < 1e-5f && Math.Abs(a.W - b.W) < 1e-5f;

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

    // === 资源拖放 / 导入 ===

    private string? _pendingModelPath;
    private string? _pendingModelName;
    private GEntityHandle _pendingMeshHandle;

    /// <summary>Creates an entity from a model file and attaches a MeshRenderer
    /// (setup completes over the next engine frames via ApplyPendingModelSetup).</summary>
    public void InstantiateModel(string filePath, GEntityHandle parent = GEntityHandle.Null)
    {
        if (string.IsNullOrWhiteSpace(filePath)) return;
        string name = System.IO.Path.GetFileNameWithoutExtension(filePath);
        CreateEntity(name, parent);
        _pendingModelPath = filePath;
        _pendingModelName = name;
        _pendingMeshHandle = GEntityHandle.Null;
    }

    private void ApplyPendingModelSetup()
    {
        if (_pendingModelPath == null) return;

        // 第一帧：实体已出现 → 挂 MeshRenderer
        if (_pendingMeshHandle == GEntityHandle.Null)
        {
            var entity = FindNewestEntityByName(_pendingModelName ?? "", RootEntities);
            if (entity == null) return;
            _pendingMeshHandle = entity.Handle;
            SelectEntityByHandle(entity.Handle);
            ulong hash = FindRegisteredTypeHash("MeshRenderer");
            if (hash != 0)
            {
                ComponentAPI.GComponent_AddComponent(entity.Handle, hash);
            }
            return;
        }

        // 第二帧：组件已挂上 → 设置 mesh_path（反射字符串字段）
        if (ComponentAPI.GComponent_GetTypeHashAt(_pendingMeshHandle, 0, out ulong typeHash) == 0 &&
            typeHash != 0)
        {
            var buf = new byte[256];
            string path = _pendingModelPath ?? "";
            int count = Encoding.UTF8.GetBytes(path, 0, Math.Min(path.Length, 250), buf, 0);
            buf[count] = 0;
            WritePropertyBytes(_pendingMeshHandle, typeHash, "mesh_path", buf);
            _engine.MarkSceneDirty();
            AppendConsole($"Imported model: {_pendingModelPath}");
            _pendingModelPath = null;
            _pendingModelName = null;
            _pendingMeshHandle = GEntityHandle.Null;
        }
    }

    private ulong FindRegisteredTypeHash(string typeName)
    {
        foreach (var t in RegisteredTypes)
        {
            if (t.TypeName == typeName) return t.TypeHash;
        }
        return 0;
    }

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

    public void LoadSceneFromPath(string path)
    {
        int rc = SceneAPI.GScene_Load(path);
        AppendConsole(rc == 0 ? $"Scene loaded: {path}" : $"Failed to load scene: {path}");
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
            _undoStack.Push(new DeleteEntityAction(this, handle,
                System.IO.Path.GetFileNameWithoutExtension(prefabPath), null, parent));
            _redoStack.Clear();
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
        _json = json;
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
        _vm.CreateEntity();
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
