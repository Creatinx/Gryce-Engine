using System;
using System.Runtime.CompilerServices;

namespace GryceEngine.Editor
{
    /// <summary>Polyfill for BitConverter.TryWriteBytes on .NET Framework 4.8.</summary>
    internal static class BitConverterCompat
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static bool TryWriteBytes(Span<byte> destination, int value)
        {
            if (destination.Length < 4) return false;
            var bytes = BitConverter.GetBytes(value);
            bytes.CopyTo(destination);
            return true;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static bool TryWriteBytes(Span<byte> destination, float value)
        {
            if (destination.Length < 4) return false;
            var bytes = BitConverter.GetBytes(value);
            bytes.CopyTo(destination);
            return true;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static bool TryWriteBytes(Span<byte> destination, uint value)
        {
            if (destination.Length < 4) return false;
            var bytes = BitConverter.GetBytes(value);
            bytes.CopyTo(destination);
            return true;
        }
    }
}