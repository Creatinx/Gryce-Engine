// Polyfill for record struct / init-only setters on .NET Framework 4.8
namespace System.Runtime.CompilerServices
{
    internal static class IsExternalInit { }
}