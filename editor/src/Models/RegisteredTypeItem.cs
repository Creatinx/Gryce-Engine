namespace GryceEngine.Editor.Models;

/// <summary>Represents a registered component type that can be added to an entity.</summary>
public class RegisteredTypeItem
{
    public string TypeName { get; }
    public ulong TypeHash { get; }

    public RegisteredTypeItem(string typeName, ulong typeHash)
    {
        TypeName = typeName;
        TypeHash = typeHash;
    }

    public override string ToString() => TypeName;
}