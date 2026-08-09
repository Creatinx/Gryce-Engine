using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;
using GryceEngine.Editor.Services;

namespace GryceEngine.Editor.ViewModels;

/// <summary>项目设置（渲染后端、管线质量）ViewModel，风格与 Settings 一致。</summary>
public class ProjectSettingsViewModel : INotifyPropertyChanged
{
    public string[] RenderApis => new[]
    {
        LocalizationService.Instance.T("project_settings.api_vulkan"),
        LocalizationService.Instance.T("project_settings.api_opengl")
    };

    public string EditorBackendInfo =>
        LocalizationService.Instance.T("project_settings.editor_backend_info");

    public string BackendRuntimeNote =>
        LocalizationService.Instance.T("project_settings.backend_runtime_note");

    public string[] ToneMapModes { get; } = { "None", "Reinhard", "ACES" };

    private int _renderApiIndex;
    public int RenderApiIndex
    {
        get => _renderApiIndex;
        set { _renderApiIndex = value; OnPropertyChanged(); }
    }

    private bool _hdr = true;
    public bool Hdr { get => _hdr; set { _hdr = value; OnPropertyChanged(); } }

    private int _toneMapIndex;
    public int ToneMapIndex
    {
        get => _toneMapIndex;
        set { _toneMapIndex = value; OnPropertyChanged(); }
    }

    private float _exposure = 1.0f;
    public float Exposure { get => _exposure; set { _exposure = value; OnPropertyChanged(); } }

    private bool _shadowEnabled = true;
    public bool ShadowEnabled { get => _shadowEnabled; set { _shadowEnabled = value; OnPropertyChanged(); } }

    private int _shadowMapSize = 2048;
    public int ShadowMapSize { get => _shadowMapSize; set { _shadowMapSize = value; OnPropertyChanged(); } }

    private float _ambientR = 0.15f;
    public float AmbientR { get => _ambientR; set { _ambientR = value; OnPropertyChanged(); } }

    private float _ambientG = 0.15f;
    public float AmbientG { get => _ambientG; set { _ambientG = value; OnPropertyChanged(); } }

    private float _ambientB = 0.15f;
    public float AmbientB { get => _ambientB; set { _ambientB = value; OnPropertyChanged(); } }

    private float _iblIntensity = 1.0f;
    public float IblIntensity { get => _iblIntensity; set { _iblIntensity = value; OnPropertyChanged(); } }

    public ICommand ApplyCommand { get; }
    public ICommand CancelCommand { get; }
    public ICommand ResetCommand { get; }

    public ProjectSettingsViewModel()
    {
        var s = ProjectSettingsService.Load();
        _renderApiIndex = string.Equals(s.RenderApi, "opengl", System.StringComparison.OrdinalIgnoreCase) ? 1 : 0;
        _hdr = s.Hdr;
        int mode = s.ToneMapMode;
        _toneMapIndex = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
        _exposure = s.Exposure;
        _shadowEnabled = s.ShadowEnabled;
        _shadowMapSize = s.ShadowMapSize;
        _ambientR = s.AmbientR;
        _ambientG = s.AmbientG;
        _ambientB = s.AmbientB;
        _iblIntensity = s.IblIntensity;

        ApplyCommand = new RelayCommand(Apply);
        CancelCommand = new RelayCommand(Cancel);
        ResetCommand = new RelayCommand(ResetToDefaults);
    }

    public void Apply()
    {
        var s = new ProjectSettingsService.ProjectSettings
        {
            RenderApi = RenderApiIndex == 0 ? "vulkan" : "opengl",
            Hdr = Hdr,
            ToneMapMode = ToneMapIndex,
            Exposure = Exposure,
            ShadowEnabled = ShadowEnabled,
            ShadowMapSize = ShadowMapSize,
            AmbientR = AmbientR,
            AmbientG = AmbientG,
            AmbientB = AmbientB,
            IblIntensity = IblIntensity
        };
        ProjectSettingsService.Save(s);
        ProjectSettingsService.Apply(s);
        ApplyRequested?.Invoke();
    }

    private void Cancel() => CancelRequested?.Invoke();

    private void ResetToDefaults()
    {
        RenderApiIndex = 0;
        Hdr = true;
        ToneMapIndex = 1;
        Exposure = 1.0f;
        ShadowEnabled = true;
        ShadowMapSize = 2048;
        AmbientR = 0.15f;
        AmbientG = 0.15f;
        AmbientB = 0.15f;
        IblIntensity = 1.0f;
    }

    public event System.Action? ApplyRequested;
    public event System.Action? CancelRequested;

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
