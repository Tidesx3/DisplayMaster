using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DisplayMaster.Services;
using Microsoft.UI.Dispatching;
using Microsoft.Win32;

namespace DisplayMaster.ViewModels;

public sealed record Choice<T>(T Value, string Label)
{
    public override string ToString() => Label;
}

/// <summary>
/// Picture settings (kept by the engine in host.ini, applied to running streams), sign-in
/// autostart (HKCU Run) and app info.
/// </summary>
public sealed partial class SettingsViewModel : ObservableObject
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string RunValue = "DisplayMaster";

    private readonly HostClient _host;
    private readonly DispatcherQueueTimer _debounce;
    private bool _loaded, _applying;

    public IReadOnlyList<Choice<int>> FrameRates { get; } =
    [
        new(120, Loc.F("Fps_UpTo", 120)), new(90, Loc.F("Fps_UpTo", 90)), new(60, Loc.F("Fps_UpTo", 60)), new(30, Loc.S("Fps_30")),
    ];

    public IReadOnlyList<Choice<double>> Scales { get; } =
    [
        new(1.0, Loc.S("Scale_Sharp")), new(0.75, Loc.S("Scale_Balanced")), new(0.5, Loc.S("Scale_Fast")),
    ];

    public IReadOnlyList<Choice<string>> Codecs { get; } =
    [
        new("auto", Loc.S("Codec_Auto")), new("hevc", "HEVC (H.265)"), new("h264", Loc.S("Codec_H264")), new("av1", Loc.S("Codec_Av1")),
    ];

    [ObservableProperty] public partial bool AutoBitrate { get; set; } = true;
    /// <summary>Fixed bitrate in Mbit/s when <see cref="AutoBitrate"/> is off.</summary>
    [ObservableProperty] public partial double BitrateMbps { get; set; } = 40;
    [ObservableProperty] public partial Choice<int>? FrameRate { get; set; }
    [ObservableProperty] public partial Choice<double>? Scale { get; set; }
    [ObservableProperty] public partial Choice<string>? Codec { get; set; }
    /// <summary>Engine started with picture flags: they win, the controls are read-only.</summary>
    [ObservableProperty] public partial bool Locked { get; set; }

    [ObservableProperty] public partial bool StartWithWindows { get; set; }
    /// <summary>Windows notification when a device connects or disconnects.</summary>
    [ObservableProperty] public partial bool NotifyDevices { get; set; } = true;

    public bool CanEdit => !Locked;
    public string BitrateText => $"{BitrateMbps:0} Mbit/s";
    public string VersionText => Loc.F("About_Version", UpdateService.CurrentVersion.ToString(3));

    public SettingsViewModel(HostClient host, DispatcherQueue dispatcher)
    {
        _host = host;
        // The timer first: setting a property below counts as a change and would use it.
        _debounce = dispatcher.CreateTimer();
        _debounce.Interval = TimeSpan.FromMilliseconds(400);
        _debounce.IsRepeating = false;
        _debounce.Tick += async (_, _) => await _host.SetStreamAsync(
            AutoBitrate ? 0 : (int)Math.Round(BitrateMbps * 1000), FrameRate?.Value ?? 120, Scale?.Value ?? 1.0,
            Codec?.Value ?? "auto");
        // Defaults until the engine reports its saved values: not changes to send.
        _applying = true;
        FrameRate = FrameRates[0];
        Scale = Scales[0];
        Codec = Codecs[0];
        StartWithWindows = ReadAutostart();
        NotifyDevices = ReadAppSetting("NotifyDevices", 1) != 0;
        _applying = false;
    }

    /// <summary>Takes the engine's saved values once; afterwards the controls are the source of truth.</summary>
    public void Load(StreamSettings s)
    {
        Locked = s.Locked;
        OnPropertyChanged(nameof(CanEdit));
        if (_loaded && !s.Locked) return;
        _loaded = true;
        _applying = true;
        AutoBitrate = s.BitrateKbps == 0;
        if (s.BitrateKbps > 0) BitrateMbps = Math.Clamp(s.BitrateKbps / 1000.0, 5, 150);
        FrameRate = FrameRates.MinBy(f => Math.Abs(f.Value - s.MaxFps));
        Scale = Scales.MinBy(c => Math.Abs(c.Value - s.Scale));
        Codec = Codecs.FirstOrDefault(c => c.Value == s.Codec) ?? Codecs[0];
        _applying = false;
    }

    partial void OnAutoBitrateChanged(bool value) => Changed();
    partial void OnBitrateMbpsChanged(double value)
    {
        OnPropertyChanged(nameof(BitrateText));
        Changed();
    }
    partial void OnFrameRateChanged(Choice<int>? value) => Changed();
    partial void OnScaleChanged(Choice<double>? value) => Changed();
    partial void OnCodecChanged(Choice<string>? value) => Changed();

    private void Changed()
    {
        if (_applying || Locked) return;
        _debounce.Stop();
        _debounce.Start();
    }

    // ---------------------------------------------------------------- sign-in autostart

    partial void OnStartWithWindowsChanged(bool value)
    {
        if (_applying) return;
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKey);
            if (value) key.SetValue(RunValue, $"\"{Environment.ProcessPath}\" --tray");
            else key.DeleteValue(RunValue, throwOnMissingValue: false);
        }
        catch (Exception e)
        {
            Debug.WriteLine($"Autostart change failed: {e.Message}");
        }
    }

    partial void OnNotifyDevicesChanged(bool value)
    {
        if (!_applying) WriteAppSetting("NotifyDevices", value ? 1 : 0);
    }

    // App-only preferences (the engine keeps its own in host.ini).
    private const string AppKey = @"Software\DisplayMaster\App";

    private static int ReadAppSetting(string name, int fallback)
    {
        using var key = Registry.CurrentUser.OpenSubKey(AppKey);
        return key?.GetValue(name) is int v ? v : fallback;
    }

    private static void WriteAppSetting(string name, int value)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(AppKey);
            key.SetValue(name, value, RegistryValueKind.DWord);
        }
        catch (Exception e)
        {
            Debug.WriteLine($"Saving {name} failed: {e.Message}");
        }
    }

    private static bool ReadAutostart()
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKey);
        return key?.GetValue(RunValue) is string;
    }

    // ---------------------------------------------------------------- about

    [RelayCommand]
    private static void OpenLogFolder()
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DisplayMaster");
        Directory.CreateDirectory(dir);
        Process.Start(new ProcessStartInfo(dir) { UseShellExecute = true });
    }

    [RelayCommand]
    private static void OpenProjectPage() =>
        Process.Start(new ProcessStartInfo($"https://github.com/{UpdateService.Repository}") { UseShellExecute = true });
}
