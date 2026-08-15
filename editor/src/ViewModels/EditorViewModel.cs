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
public partial class EditorViewModel : INotifyPropertyChanged
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

    // Grid snapping used by the gizmo translate and the align toolbar.
    private bool _gridSnap;
    private float _gridSnapSize = 1.0f;

    public bool IsGridSnap
    {
        get => _gridSnap;
        set { _gridSnap = value; OnPropertyChanged(); }
    }

    public float GridSnapSize
    {
        get => _gridSnapSize;
        set { _gridSnapSize = value; OnPropertyChanged(); }
    }

    /// <summary>Snap a displacement (delta) to the active grid, or return it
    /// unchanged when snapping is disabled.</summary>
    public double SnapDelta(double delta)
    {
        if (!_gridSnap || _gridSnapSize <= 0f) return delta;
        double g = _gridSnapSize;
        return Math.Round(delta / g) * g;
    }

    public void ToggleGridSnap() => IsGridSnap = !IsGridSnap;

    /// <summary>Snap the selected entity's position onto the nearest grid point
    /// along the given axis ('X', 'Y', 'Z', or null for all). Commits one undo.</summary>
    public void AlignToGrid(string? axis = null)
    {
        if (_selectedEntity == null) return;
        var pos = _selectedEntity.LocalPosition;
        var oldPos = pos;
        double g = _gridSnapSize > 0f ? _gridSnapSize : 1.0;
        if (axis == null || axis == "X") pos.X = (float)(Math.Round(pos.X / g) * g);
        if (axis == null || axis == "Y") pos.Y = (float)(Math.Round(pos.Y / g) * g);
        if (axis == null || axis == "Z") pos.Z = (float)(Math.Round(pos.Z / g) * g);
        if (Vec3Close(oldPos, pos)) return;
        _selectedEntity.LocalPosition = pos;
        WriteTransformLive();
        PushUndo(new TransformAction(this, _selectedEntity.Handle, oldPos,
            _selectedEntity.LocalRotation, _selectedEntity.LocalScale, pos,
            _selectedEntity.LocalRotation, _selectedEntity.LocalScale));
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

    // Skeleton entities to wire up (name -> component type) on the next frame
    // after a New Scene, so the fresh scene is immediately useful.
    private readonly List<(string name, string componentType)> _pendingSkeleton = new();

    // Undo/Redo stacks
    private readonly Stack<IUndoableAction> _undoStack = new();
    private readonly Stack<IUndoableAction> _redoStack = new();
    // Created-entity actions whose handle is resolved when the entity appears.
    private readonly Queue<CreateEntityAction> _pendingCreateActions = new();
    // Component restores that must wait for the async AddComponent command to
    // complete on the next engine frame before their fields can be written.
    private readonly List<(GEntityHandle Entity, ulong TypeHash, ComponentSnapshot Snapshot, int Retries)> _pendingComponentRestores = new();
    // Entity-type switch undo entry, finalized once the component commands run.
    private EntityTypeAction? _pendingEntityTypeAction;

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
                ApplyPendingComponentRestores();
                AttachPendingComponentToNewEntity();
                ProcessPendingSkeleton();
                SelectPendingNewEntity();
                ApplyPendingModelSetup();
                if (_entityTypeSwitchPending)
                {
                    _entityTypeSwitchPending = false;
                    FinalizePendingEntityTypeAction();
                    RefreshEntityType();
                    RefreshInspector();
                }
                OnPropertyChanged(nameof(EntityCount));
                // Keep the Inspector in sync after any engine-side entity change
                // (e.g. an AddComponent command completed on the next frame):
                // RefreshHierarchy rebuilt the models, so re-resolve the
                // selection by handle (suppressing history) or refresh directly.
                if (_selectedEntity != null)
                {
                    var selHandle = _selectedEntity.Handle;
                    var fresh = FindEntity(selHandle, RootEntities);
                    if (fresh != null && !ReferenceEquals(fresh, _selectedEntity))
                    {
                        _historySuppress = true;
                        try { SelectEntityByHandle(selHandle); }
                        finally { _historySuppress = false; }
                    }
                    else
                    {
                        RefreshInspector();
                    }
                }
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
                _pendingCreateActions.Clear();
                _pendingComponentRestores.Clear();
                System.Windows.Input.CommandManager.InvalidateRequerySuggested();
                RefreshHierarchy();
                RefreshInspector();
                OnPropertyChanged(nameof(EntityCount));
                // Default to the project's main scene (project_settings.json
                // "main_scene"), with the legacy scenes as fallbacks.
                int rc = SceneAPI.GScene_Load(ProjectSettingsService.Load().MainScene);
                if (rc != 0) rc = SceneAPI.GScene_Load("res:/scenes/main.gesc");
                if (rc != 0) rc = SceneAPI.GScene_Load("res:/scenes/editor_default.gesc");
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
        if (_pendingCreateActions.Count > 0)
        {
            _pendingCreateActions.Dequeue().Handle = created.Handle;
        }
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

    /// <summary>Attach the Camera / Light components to the skeleton entities that
    /// were created by CreateSceneSkeleton on the previous frame.</summary>
    private void ProcessPendingSkeleton()
    {
        if (_pendingSkeleton.Count == 0) return;
        var items = _pendingSkeleton.ToList();
        _pendingSkeleton.Clear();
        foreach (var (name, componentType) in items)
        {
            var created = FindNewestEntityByName(name, RootEntities);
            if (created == null) continue;
            ulong hash = 0;
            foreach (var t in RegisteredTypes)
            {
                if (t.TypeName == componentType) { hash = t.TypeHash; break; }
            }
            if (hash == 0) continue;
            if (ComponentAPI.GComponent_AddComponent(created.Handle, hash) == 0)
            {
                _engine.MarkSceneDirty();
                AppendConsole($"Component '{componentType}' added to '{name}'");
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
    private int _selectedEntityType; // 0=None, 1=Node2D, 2=Node3D
    private bool _entityTypeSwitchPending;

    public int SelectedEntityType
    {
        get => _selectedEntityType;
        set
        {
            if (_selectedEntityType == value) return;
            _selectedEntityType = value;
            OnPropertyChanged();
            ApplyEntityType(value);
        }
    }
    private string? _pendingModelPath;
    private string? _pendingModelName;
    private GEntityHandle _pendingMeshHandle;
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
