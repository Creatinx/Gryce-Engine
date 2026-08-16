using System;

namespace GryceEngine.Editor.Services;

/// <summary>
/// 全局"编辑进行中"标记：gizmo 拖拽、Inspector 输入框聚焦期间自动保存会
/// 推迟，避免把拖到一半的 Transform 或输入一半的属性固化进场景文件。
/// </summary>
public static class EditorInteractionState
{
    private static int _busyCount;

    public static bool IsBusy => _busyCount > 0;

    public static void BeginBusy()
    {
        _busyCount++;
    }

    public static void EndBusy()
    {
        if (_busyCount > 0) _busyCount--;
    }

    /// <summary>using 作用域包装，异常路径也能保证计数对称。</summary>
    public static IDisposable BusyScope()
    {
        BeginBusy();
        return new Scope();
    }

    private sealed class Scope : IDisposable
    {
        private bool _disposed;
        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            EndBusy();
        }
    }
}
