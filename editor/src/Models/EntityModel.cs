using GryceEngine.Editor.Native;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text;

namespace GryceEngine.Editor.Models;

/// <summary>Represents an entity in the scene hierarchy, driven by the C API.</summary>
public class EntityModel : INotifyPropertyChanged
{
    private string _name = "Entity";
    private GVec3 _position;
    private GQuat _rotation;
    private GVec3 _scale = new(1, 1, 1);
    private bool _isSelected;
    private bool _isExpanded = true;
    private bool _enabled = true;

    public GEntityHandle Handle { get; }

    public string Name
    {
        get => _name;
        set { _name = value; OnPropertyChanged(); }
    }

    public GVec3 LocalPosition
    {
        get => _position;
        set { _position = value; OnPropertyChanged(); OnPositionComponentChanged(); }
    }

    public GQuat LocalRotation
    {
        get => _rotation;
        set { _rotation = value; OnPropertyChanged(); OnRotationComponentChanged(); }
    }

    public GVec3 LocalScale
    {
        get => _scale;
        set { _scale = value; OnPropertyChanged(); OnScaleComponentChanged(); }
    }

    // Individual float properties for TwoWay binding (struct copy issue workaround)
    public float PositionX { get => _position.X; set { _position.X = value; OnPropertyChanged(); } }
    public float PositionY { get => _position.Y; set { _position.Y = value; OnPropertyChanged(); } }
    public float PositionZ { get => _position.Z; set { _position.Z = value; OnPropertyChanged(); } }
    public float RotationX { get => _rotation.X; set { _rotation.X = value; OnPropertyChanged(); } }
    public float RotationY { get => _rotation.Y; set { _rotation.Y = value; OnPropertyChanged(); } }
    public float RotationZ { get => _rotation.Z; set { _rotation.Z = value; OnPropertyChanged(); } }
    public float RotationW { get => _rotation.W; set { _rotation.W = value; OnPropertyChanged(); } }
    public float ScaleX { get => _scale.X; set { _scale.X = value; OnPropertyChanged(); } }
    public float ScaleY { get => _scale.Y; set { _scale.Y = value; OnPropertyChanged(); } }
    public float ScaleZ { get => _scale.Z; set { _scale.Z = value; OnPropertyChanged(); } }

    private void OnPositionComponentChanged()
    {
        OnPropertyChanged(nameof(PositionX));
        OnPropertyChanged(nameof(PositionY));
        OnPropertyChanged(nameof(PositionZ));
    }
    private void OnRotationComponentChanged()
    {
        OnPropertyChanged(nameof(RotationX));
        OnPropertyChanged(nameof(RotationY));
        OnPropertyChanged(nameof(RotationZ));
        OnPropertyChanged(nameof(RotationW));
    }
    private void OnScaleComponentChanged()
    {
        OnPropertyChanged(nameof(ScaleX));
        OnPropertyChanged(nameof(ScaleY));
        OnPropertyChanged(nameof(ScaleZ));
    }

    public bool IsSelected
    {
        get => _isSelected;
        set { _isSelected = value; OnPropertyChanged(); }
    }

    public bool IsExpanded
    {
        get => _isExpanded;
        set { _isExpanded = value; OnPropertyChanged(); }
    }

    public bool Enabled
    {
        get => _enabled;
        set { _enabled = value; OnPropertyChanged(); }
    }

    public ObservableCollection<EntityModel> Children { get; } = new();
    public ObservableCollection<ComponentModel> Components { get; } = new();

    public EntityModel(GEntityHandle handle, string name)
    {
        Handle = handle;
        _name = name;
    }

    public override string ToString() => Name;

    /// <summary>Refresh transform data from the engine.</summary>
    public void RefreshTransform()
    {
        EntityAPI.GEntity_GetLocalPosition(Handle, out var pos);
        EntityAPI.GEntity_GetLocalRotation(Handle, out var rot);
        EntityAPI.GEntity_GetLocalScale(Handle, out var scl);
        LocalPosition = pos;
        LocalRotation = rot;
        LocalScale = scl;
    }

    /// <summary>Refresh component list from the engine.</summary>
    public void RefreshComponents()
    {
        Components.Clear();
        int count = ComponentAPI.GComponent_GetCount(Handle);
        for (int i = 0; i < count; i++)
        {
            var sb = new StringBuilder(128);
            if (ComponentAPI.GComponent_GetTypeNameAt(Handle, i, sb, sb.Capacity) >= 0)
            {
                ulong hash = 0;
                ComponentAPI.GComponent_GetTypeHashAt(Handle, i, out hash);
                Components.Add(new ComponentModel(Handle, ShortTypeName(sb.ToString()), hash));
            }
        }
    }

    /// <summary>Strips the "gryce_engine::components::" namespace prefix from a type name.</summary>
    internal static string ShortTypeName(string fullName)
    {
        if (string.IsNullOrEmpty(fullName)) return fullName;
        int idx = fullName.LastIndexOf("::", StringComparison.Ordinal);
        return idx >= 0 ? fullName.Substring(idx + 2) : fullName;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
