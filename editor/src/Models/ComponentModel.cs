using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text;

namespace GryceEngine.Editor.Models;

/// <summary>Represents a component attached to an entity, with its properties.</summary>
public class ComponentModel : INotifyPropertyChanged
{
    private bool _isExpanded = true;
    private bool _enabled = true;

    public GEntityHandle EntityHandle { get; }
    public string TypeName { get; }
    public ulong TypeHash { get; }
    public ObservableCollection<PropertyModel> Properties { get; } = new();

    /// <summary>Localized display name for the Inspector header.</summary>
    public string DisplayName
    {
        get
        {
            string key = "inspector.type." + TypeName;
            string localized = LocalizationService.Instance.T(key);
            return localized != key ? localized : TypeName;
        }
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

    public ComponentModel(GEntityHandle entityHandle, string typeName, ulong typeHash)
    {
        EntityHandle = entityHandle;
        TypeName = typeName;
        TypeHash = typeHash;
    }

    /// <summary>Refresh property list from the engine.</summary>
    public void RefreshProperties()
    {
        Properties.Clear();
        int count = ComponentAPI.GComponent_GetPropertyCount(EntityHandle, TypeHash);
        for (int i = 0; i < count; i++)
        {
            var sb = new StringBuilder(128);
            int propType = 0, propSize = 0;
            if (ComponentAPI.GComponent_GetPropertyInfo(EntityHandle, TypeHash, i, sb, sb.Capacity, out propType, out propSize) >= 0)
            {
                var prop = new PropertyModel(sb.ToString(), (PropertyType)propType, propSize);
                prop.ReadFromEngine(EntityHandle, TypeHash);
                Properties.Add(prop);
            }
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}

/// <summary>Property type enum matching the C API field types.</summary>
public enum PropertyType
{
    Int = 0, Float = 1, Double = 2, Bool = 3, String = 4,
    Vector2f = 5, Vector3f = 6, Vector3i = 7, Vector4f = 8,
    Quaternionf = 9, Color = 10, Enum = 11
}

/// <summary>Represents a single property on a component.</summary>
public class PropertyModel : INotifyPropertyChanged
{
    public string Name { get; }
    public PropertyType PropType { get; }
    public int Size { get; }

    // Value holders
    private float _floatValue;
    private string _stringValue = string.Empty;
    private bool _boolValue;
    private int _intValue;

    public float FloatValue
    {
        get => _floatValue;
        set { _floatValue = value; OnPropertyChanged(); }
    }

    public string StringValue
    {
        get => _stringValue;
        set { _stringValue = value; OnPropertyChanged(); }
    }

    public bool BoolValue
    {
        get => _boolValue;
        set { _boolValue = value; OnPropertyChanged(); }
    }

    public int IntValue
    {
        get => _intValue;
        set { _intValue = value; OnPropertyChanged(); }
    }

    public PropertyModel(string name, PropertyType propType, int size)
    {
        Name = name;
        PropType = propType;
        Size = size;
    }

    /// <summary>Read property value from the engine.</summary>
    public unsafe void ReadFromEngine(GEntityHandle entity, ulong typeHash)
    {
        fixed (float* pf = &_floatValue)
        fixed (int* pi = &_intValue)
        fixed (bool* pb = &_boolValue)
        {
            nint ptr = PropType switch
            {
                PropertyType.Float or PropertyType.Double => (nint)pf,
                PropertyType.Int or PropertyType.Enum => (nint)pi,
                PropertyType.Bool => (nint)pb,
                _ => (nint)pf // default to float buffer
            };
            ComponentAPI.GComponent_GetProperty(entity, typeHash, Name, ptr, Size);
        }
    }

    /// <summary>Write property value back to the engine.</summary>
    public unsafe void WriteToEngine(GEntityHandle entity, ulong typeHash)
    {
        fixed (float* pf = &_floatValue)
        fixed (int* pi = &_intValue)
        fixed (bool* pb = &_boolValue)
        {
            nint ptr = PropType switch
            {
                PropertyType.Float or PropertyType.Double => (nint)pf,
                PropertyType.Int or PropertyType.Enum => (nint)pi,
                PropertyType.Bool => (nint)pb,
                _ => (nint)pf
            };
            ComponentAPI.GComponent_SetProperty(entity, typeHash, Name, ptr, Size);
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
