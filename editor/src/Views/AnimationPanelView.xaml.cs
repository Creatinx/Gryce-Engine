using GryceEngine.Editor.Models;
using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System;
using System.Collections.Generic;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

namespace GryceEngine.Editor.Views;

/// <summary>
/// Bottom animation panel: drives the SkinnedMeshRenderer playback state
/// (clip / play / pause / stop / loop / speed / seek) through the C API.
/// </summary>
public partial class AnimationPanelView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;
    private DispatcherTimer? _refreshTimer;
    private bool _syncing; // 防止回调写回引擎时互相触发
    private ComponentModel? _animator;
    private GEntityHandle _lastRefreshedEntity = GEntityHandle.Null;

    // Property models bound to SkinnedMeshRenderer reflection fields
    private PropertyModel? _propClipName;
    private PropertyModel? _propPlaying;
    private PropertyModel? _propLoop;
    private PropertyModel? _propSpeed;
    private PropertyModel? _propTime;

    public AnimationPanelView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        Loaded += (_, _) =>
        {
            _refreshTimer = new DispatcherTimer(DispatcherPriority.Background)
            {
                Interval = TimeSpan.FromMilliseconds(120)
            };
            _refreshTimer.Tick += (_, _) => RefreshTimeDisplay();
            _refreshTimer.Tick += (_, _) =>
            {
                // SelectedEntity 事件在 Inspector 刷新组件之前触发；
                // 延迟一拍再扫描，确保 Components 已填充。
                var sel = VM?.SelectedEntity;
                if (sel != null && _lastRefreshedEntity != sel.Handle)
                    RefreshAnimator();
            };
            _refreshTimer.Start();
        };
        Unloaded += (_, _) =>
        {
            _refreshTimer?.Stop();
            _refreshTimer = null;
        };
    }

    private void OnDataContextChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (VM != null)
        {
            VM.PropertyChanged += (_, args) =>
            {
                if (args.PropertyName == nameof(EditorViewModel.SelectedEntity))
                    _lastRefreshedEntity = GEntityHandle.Null;
            };
        }
    }

    private void RefreshAnimator()
    {
        _syncing = true;
        try
        {
            var sel = VM?.SelectedEntity;
            if (sel == null)
            {
                _lastRefreshedEntity = GEntityHandle.Null;
                _animator = null;
                NoAnimatorText.Visibility = Visibility.Visible;
                Controls.Visibility = Visibility.Collapsed;
                return;
            }
            _lastRefreshedEntity = sel.Handle;

            _animator = null;

            foreach (var comp in sel.Components)
            {
                if (comp.TypeName == "SkinnedMeshRenderer")
                {
                    _animator = comp;
                    break;
                }
            }

            if (_animator == null)
            {
                NoAnimatorText.Visibility = Visibility.Visible;
                Controls.Visibility = Visibility.Collapsed;
                return;
            }

            NoAnimatorText.Visibility = Visibility.Collapsed;
            Controls.Visibility = Visibility.Visible;

            _propClipName = CreateProp("clip_name");
            _propPlaying = CreateProp("playing");
            _propLoop = CreateProp("loop");
            _propSpeed = CreateProp("speed");
            _propTime = CreateProp("time");

            PopulateClips();
            RefreshTimeDisplay();
        }
        finally
        {
            _syncing = false;
        }
    }

    private PropertyModel? CreateProp(string name)
    {
        if (_animator == null) return null;
        var prop = new PropertyModel(name, PropertyType.Float, 4);
        // 用 GetPropertyInfo 修正类型
        int propType = 0, propSize = 0;
        var sb = new StringBuilder(128);
        if (ComponentAPI.GComponent_GetPropertyInfo(_animator.EntityHandle, _animator.TypeHash, 0,
                sb, sb.Capacity, out propType, out propSize) >= 0)
        {
            for (int i = 0; i < ComponentAPI.GComponent_GetPropertyCount(_animator.EntityHandle, _animator.TypeHash); i++)
            {
                sb.Clear();
                if (ComponentAPI.GComponent_GetPropertyInfo(_animator.EntityHandle, _animator.TypeHash, i,
                        sb, sb.Capacity, out propType, out propSize) >= 0 &&
                    sb.ToString() == name)
                {
                    prop = new PropertyModel(name, (PropertyType)propType, propSize);
                    break;
                }
            }
        }
        prop.ReadFromEngine(_animator.EntityHandle, _animator.TypeHash);
        return prop;
    }

    private void PopulateClips()
    {
        if (_animator == null) return;
        var handle = _animator.EntityHandle;
        var hash = _animator.TypeHash;

        var clips = new List<string>();
        int count = AnimatorAPI.GAnimator_GetClipCount(handle, hash);
        for (int i = 0; i < count; i++)
        {
            var sb = new StringBuilder(128);
            if (AnimatorAPI.GAnimator_GetClipName(handle, hash, i, sb, sb.Capacity) >= 0)
                clips.Add(sb.ToString());
        }

        string current = _propClipName?.StringValue ?? string.Empty;
        ClipCombo.Items.Clear();
        foreach (var c in clips) ClipCombo.Items.Add(c);
        if (clips.Count == 0)
        {
            ClipCombo.IsEnabled = false;
            PlayPauseButton.IsEnabled = false;
            StopButton.IsEnabled = false;
            StatusText.Text = LocalizationService.Instance.T("animation.no_clips");
            return;
        }

        ClipCombo.IsEnabled = true;
        PlayPauseButton.IsEnabled = true;
        StopButton.IsEnabled = true;
        StatusText.Text = string.Empty;

        int idx = clips.IndexOf(current);
        ClipCombo.SelectedIndex = idx >= 0 ? idx : 0;
        if (idx < 0) _propClipName?.WriteToEngine(handle, hash);

        // 时间轴范围 = 当前片段时长
        var dur = AnimatorAPI.GAnimator_GetClipDuration(handle, hash, ClipCombo.SelectedIndex);
        if (dur > 0) TimeSlider.Maximum = dur;
    }

    private void RefreshTimeDisplay()
    {
        if (_animator == null) return;
        if (_syncing) return;

        _syncing = true;
        try
        {
            _propTime?.ReadFromEngine(_animator.EntityHandle, _animator.TypeHash);
            _propPlaying?.ReadFromEngine(_animator.EntityHandle, _animator.TypeHash);
            if (_propTime != null)
            {
                TimeSlider.Value = Math.Min(_propTime.FloatValue, TimeSlider.Maximum);
                TimeLabel.Text = $"{_propTime.FloatValue:0.00}s / {TimeSlider.Maximum:0.00}s";
            }
            if (_propPlaying != null)
                PlayPauseButton.Content = _propPlaying.BoolValue ? "\uE769" : "\uE768";
        }
        finally
        {
            _syncing = false;
        }
    }

    private void OnClipSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || _animator == null || ClipCombo.SelectedIndex < 0) return;
        if (_propClipName == null) return;

        _propClipName.StringValue = ClipCombo.SelectedItem?.ToString() ?? string.Empty;
        _propClipName.WriteToEngine(_animator.EntityHandle, _animator.TypeHash);
        // 切换片段后回到 0 秒
        if (_propTime != null)
        {
            _propTime.FloatValue = 0;
            _propTime.WriteToEngine(_animator.EntityHandle, _animator.TypeHash);
        }
        var dur = AnimatorAPI.GAnimator_GetClipDuration(
            _animator.EntityHandle, _animator.TypeHash, ClipCombo.SelectedIndex);
        if (dur > 0) TimeSlider.Maximum = dur;
    }

    private void OnPlayPauseClick(object sender, RoutedEventArgs e)
    {
        if (_animator == null || _propPlaying == null) return;
        _propPlaying.BoolValue = !_propPlaying.BoolValue;
        _propPlaying.WriteToEngine(_animator.EntityHandle, _animator.TypeHash);
        PlayPauseButton.Content = _propPlaying.BoolValue ? "\uE769" : "\uE768";
    }

    private void OnStopClick(object sender, RoutedEventArgs e)
    {
        if (_animator == null || _propPlaying == null || _propTime == null) return;
        _propPlaying.BoolValue = false;
        _propPlaying.WriteToEngine(_animator.EntityHandle, _animator.TypeHash);
        _propTime.FloatValue = 0;
        _propTime.WriteToEngine(_animator.EntityHandle, _animator.TypeHash);
        PlayPauseButton.Content = "\uE768";
        TimeSlider.Value = 0;
        TimeLabel.Text = $"0.00s / {TimeSlider.Maximum:0.00}s";
    }

    private void OnLoopChanged(object sender, RoutedEventArgs e)
    {
        if (_syncing || _animator == null || _propLoop == null) return;
        _propLoop.BoolValue = LoopCheck.IsChecked == true;
        _propLoop.WriteToEngine(_animator.EntityHandle, _animator.TypeHash);
    }

    private void OnSpeedChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_syncing || _animator == null || _propSpeed == null) return;
        SpeedLabel.Text = $"{SpeedSlider.Value:0.00}x";
        _propSpeed.FloatValue = (float)SpeedSlider.Value;
        _propSpeed.WriteToEngine(_animator.EntityHandle, _animator.TypeHash);
    }

    private void OnTimeChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_syncing || _animator == null || _propTime == null) return;
        _propTime.FloatValue = (float)TimeSlider.Value;
        _propTime.WriteToEngine(_animator.EntityHandle, _animator.TypeHash);
    }
}
