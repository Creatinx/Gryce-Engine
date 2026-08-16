using System.Windows;

namespace GryceEngine.Editor.Services;

/// <summary>
/// 统一处理"即将丢弃未保存场景"的确认：新建场景、加载场景、切换项目、
/// 关闭编辑器前调用 CheckCanDiscard()，返回 true 表示可以继续（用户已保存
/// 或选择放弃）。播放中也会先停止播放再询问，避免静默丢弃。
/// </summary>
public static class UnsavedChangesGuard
{
    /// <summary>true = 可以继续（未修改、已保存，或用户选择放弃）。</summary>
    public static bool CheckCanDiscard(Window? owner = null)
    {
        var engine = App.Engine;
        if (engine == null || !engine.IsInitialized || !engine.IsSceneDirty) return true;

        // 播放中的场景修改未落盘，先停止播放（引擎会恢复/保留编辑态场景）。
        if (engine.IsPlaying)
        {
            App.EditorVM?.Stop();
        }

        var loc = LocalizationService.Instance;
        var ownerWindow = owner ?? Application.Current.MainWindow;
        MessageBoxResult result = ownerWindow != null
            ? MessageBox.Show(ownerWindow,
                loc.T("confirm.unsaved_message"),
                loc.T("confirm.unsaved_title"),
                MessageBoxButton.YesNoCancel,
                MessageBoxImage.Warning)
            : MessageBox.Show(
                loc.T("confirm.unsaved_message"),
                loc.T("confirm.unsaved_title"),
                MessageBoxButton.YesNoCancel,
                MessageBoxImage.Warning);

        if (result == MessageBoxResult.Yes)
        {
            App.EditorVM?.SaveScene();
            // 保存失败（场景仍为 dirty）则视为取消，避免丢数据。
            return !engine.IsSceneDirty;
        }
        if (result == MessageBoxResult.No) return true;
        return false;
    }
}
