using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DisplayMaster.Services;
using Microsoft.UI.Dispatching;

namespace DisplayMaster.ViewModels;

public sealed partial class DeviceViewModel : ObservableObject
{
    public int Id { get; }

    [ObservableProperty] public partial string Name { get; set; } = "";
    [ObservableProperty] public partial string Model { get; set; } = "";
    [ObservableProperty] public partial bool IsUsb { get; set; }
    [ObservableProperty] public partial bool Streaming { get; set; }
    [ObservableProperty] public partial bool HasPen { get; set; }
    [ObservableProperty] public partial string ModeText { get; set; } = "";
    [ObservableProperty] public partial string VideoText { get; set; } = "";
    [ObservableProperty] public partial string EncoderText { get; set; } = "";
    [ObservableProperty] public partial string FpsText { get; set; } = "–";
    [ObservableProperty] public partial string BitrateText { get; set; } = "–";
    [ObservableProperty] public partial string EncodeText { get; set; } = "–";
    [ObservableProperty] public partial string DecodeText { get; set; } = "–";
    [ObservableProperty] public partial string DeviceGlyph { get; set; } = Glyphs.Tablet;
    [ObservableProperty] public partial bool IsExtend { get; set; }
    [ObservableProperty] public partial Choice<string>? Position { get; set; }

    public static IReadOnlyList<Choice<string>> Positions { get; } =
    [
        new("right", Loc.S("Position_Right")), new("left", Loc.S("Position_Left")),
        new("above", Loc.S("Position_Above")), new("below", Loc.S("Position_Below")),
    ];

    /// <summary>The user picked another position (not raised for updates from the engine).</summary>
    public Action<DeviceViewModel, string>? PositionPicked { get; set; }
    private bool _applying;

    partial void OnPositionChanged(Choice<string>? value)
    {
        if (!_applying && value is not null) PositionPicked?.Invoke(this, value.Value);
    }

    /// <summary>Wi-Fi video over UDP (with loss repair) rather than on the control connection.</summary>
    [ObservableProperty] public partial bool IsUdp { get; set; }

    public string TransportText => IsUsb ? "USB" : IsUdp ? Loc.S("Transport_WifiUdp") : Loc.S("Transport_Wifi");
    public string TransportGlyph => IsUsb ? Glyphs.Usb : Glyphs.Wifi;

    public DeviceViewModel(int id) => Id = id;

    partial void OnIsUdpChanged(bool value) => OnPropertyChanged(nameof(TransportText));

    partial void OnIsUsbChanged(bool value)
    {
        OnPropertyChanged(nameof(TransportText));
        OnPropertyChanged(nameof(TransportGlyph));
    }

    public void Update(SessionInfo s)
    {
        Name = s.Name;
        Model = s.Model;
        IsUsb = s.Transport == "usb";
        IsExtend = s.Mode == "extend";
        _applying = true;
        Position = Positions.FirstOrDefault(p => p.Value == s.Position) ?? Positions[0];
        _applying = false;
        IsUdp = s.Udp;
        Streaming = s.Streaming;
        HasPen = s.Pen;
        DeviceGlyph = s.Width > 0 && Math.Max(s.Width, s.Height) / (double)Math.Min(s.Width, s.Height) > 1.9
            ? Glyphs.Phone
            : Glyphs.Tablet;
        ModeText = s.Mode switch { "extend" => Loc.S("Mode_Extend"), "mirror" => Loc.S("Mode_Mirror"), _ => Loc.S("Mode_Tablet") };
        VideoText = s.Streaming ? $"{s.Width} × {s.Height} · {s.Fps} Hz · {s.Codec}" : Loc.S("Video_Starting");
        EncoderText = s.Streaming ? Loc.F("Video_Encoder", s.Encoder, s.Gpu) : "";
        FpsText = s.Streaming ? $"{s.SentFps:0}" : "–";
        BitrateText = s.Streaming ? $"{s.Mbps:0.0}" : "–";
        EncodeText = s.Streaming ? $"{s.EncodeMs:0.0}" : "–";
        DecodeText = s.DecodeMs > 0 ? $"{s.DecodeMs:0.0}" : "–";
    }
}

public static class Glyphs
{
    // Segoe Fluent Icons code points.
    public const string Tablet = "";
    public const string Phone = "";
    public const string Usb = "";
    public const string Wifi = "";
}

public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private readonly HostClient _host = new();
    private readonly DispatcherQueueTimer _timer;
    private bool _polling;

    public ObservableCollection<DeviceViewModel> Devices { get; } = new();
    public PenViewModel Pen { get; }
    public UpdateViewModel Update { get; }
    public SettingsViewModel Settings { get; }
    public ObservableCollection<TrustedDevice> TrustedDevices { get; } = new();
    private readonly HashSet<int> _announcedApprovals = new();

    /// <summary>A Wi-Fi device asks to connect; the app should ask the user.</summary>
    public event Action<PendingDevice>? ApprovalRequested;
    /// <summary>A request went away (answered elsewhere, device gave up, timed out).</summary>
    public event Action<int>? ApprovalResolved;

    [ObservableProperty] public partial bool HostReachable { get; set; }
    [ObservableProperty] public partial string HostName { get; set; } = Environment.MachineName;
    [ObservableProperty] public partial bool WifiEnabled { get; set; }
    [ObservableProperty] public partial bool Elevated { get; set; } = true;
    [ObservableProperty] public partial bool VddInstalled { get; set; } = true;
    [ObservableProperty] public partial bool AdbAvailable { get; set; } = true;
    [ObservableProperty] public partial bool UsbPromptPending { get; set; }
    [ObservableProperty] public partial string AddressesText { get; set; } = "";
    [ObservableProperty] public partial string UsbDevicesText { get; set; } = "";
    [ObservableProperty] public partial int Port { get; set; } = 47800;
    /// <summary>Where a phone on the network downloads the Android app (QR code); empty if unavailable.</summary>
    [ObservableProperty] public partial string ApkUrl { get; set; } = "";

    public bool HasApkUrl => ApkUrl.Length > 0;
    partial void OnApkUrlChanged(string value) => OnPropertyChanged(nameof(HasApkUrl));

    public bool HasDevices => Devices.Count > 0;
    public bool NoDevices => Devices.Count == 0;
    public string DeviceCountText => Devices.Count switch
    {
        0 => Loc.S("Devices_None"),
        1 => Loc.S("Devices_One"),
        var n => Loc.F("Devices_Many", n),
    };

    /// <summary>Raised after each poll with the number of connected devices (tray tooltip).</summary>
    public event Action<int>? DevicesChanged;
    /// <summary>A device connected / went away (not raised for devices already there at app start).</summary>
    public event Action<DeviceViewModel>? DeviceConnected;
    public event Action<string>? DeviceDisconnected;
    private bool _synced;

    public MainViewModel(DispatcherQueue dispatcher)
    {
        Devices.CollectionChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(HasDevices));
            OnPropertyChanged(nameof(NoDevices));
            OnPropertyChanged(nameof(DeviceCountText));
        };
        Pen = new PenViewModel(_host, dispatcher);
        Update = new UpdateViewModel(dispatcher);
        Settings = new SettingsViewModel(_host, dispatcher);
        _timer = dispatcher.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += async (_, _) => await PollAsync();
        _timer.Start();
        _ = PollAsync();
    }

    private async Task PollAsync()
    {
        if (_polling) return;
        _polling = true;
        try
        {
            var status = await _host.GetStatusAsync();
            HostReachable = status is { Ok: true };
            _applyingHostState = true;
            if (status?.Host is { } h)
            {
                HostName = h.Name;
                Port = h.Port;
                // Don't fight the user while a toggle is in flight.
                if (!_wifiChanging) WifiEnabled = h.Wifi;
                Elevated = h.Elevated;
                VddInstalled = h.Vdd;
                AdbAvailable = h.Adb;
                UsbPromptPending = h.AdbUnauthorized.Count > 0;
                // The engine lists the best address first; one is all a device needs.
                AddressesText = h.Addresses.Count > 0 ? h.Addresses[0] : Loc.S("Network_None");
                ApkUrl = h.Wifi && h.Addresses.Count > 0 && !string.IsNullOrEmpty(h.ApkPath)
                    ? $"http://{h.Addresses[0]}:{h.Port}{h.ApkPath}"
                    : "";
                UsbDevicesText = h.AdbReady.Count > 0 ? string.Join(", ", h.AdbReady) : "None";
                if (h.Pen is { } pen) Pen.Load(pen);
                if (h.Stream is { } stream) Settings.Load(stream);
            }
            _applyingHostState = false;
            SyncDevices(status?.Sessions ?? new List<SessionInfo>());
            SyncTrusted(status?.Trusted ?? new List<TrustedDevice>());
            var pending = status?.Pending ?? new List<PendingDevice>();
            foreach (var p in pending)
                if (_announcedApprovals.Add(p.Id)) ApprovalRequested?.Invoke(p);
            foreach (var gone in _announcedApprovals.Where(id => pending.All(p => p.Id != id)).ToList())
            {
                _announcedApprovals.Remove(gone);
                ApprovalResolved?.Invoke(gone);
            }
        }
        finally
        {
            _applyingHostState = false;
            _polling = false;
        }
    }

    private void SyncDevices(List<SessionInfo> sessions)
    {
        for (var i = Devices.Count - 1; i >= 0; i--)
            if (sessions.All(s => s.Id != Devices[i].Id))
            {
                var name = Devices[i].Name;
                Devices.RemoveAt(i);
                DeviceDisconnected?.Invoke(name);
            }
        foreach (var s in sessions)
        {
            var vm = Devices.FirstOrDefault(d => d.Id == s.Id);
            var added = vm is null;
            if (vm is null)
            {
                vm = new DeviceViewModel(s.Id) { PositionPicked = (d, pos) => _ = _host.SetPositionAsync(d.Id, pos) };
                Devices.Add(vm);
            }
            vm.Update(s);
            if (added && _synced) DeviceConnected?.Invoke(vm);
        }
        _synced = true;
        DevicesChanged?.Invoke(Devices.Count);
    }

    private void SyncTrusted(List<TrustedDevice> trusted)
    {
        if (trusted.SequenceEqual(TrustedDevices)) return;
        TrustedDevices.Clear();
        foreach (var t in trusted) TrustedDevices.Add(t);
        OnPropertyChanged(nameof(HasTrustedDevices));
    }

    public bool HasTrustedDevices => TrustedDevices.Count > 0;

    public Task AnswerApprovalAsync(int id, bool allow, bool remember) => _host.ApproveAsync(id, allow, remember);

    [RelayCommand]
    private async Task ForgetDeviceAsync(TrustedDevice? device)
    {
        if (device is null) return;
        await _host.ForgetDeviceAsync(device.DeviceId);
        await PollAsync();
    }

    [RelayCommand]
    private async Task DisconnectAsync(DeviceViewModel? device)
    {
        if (device is null) return;
        await _host.DisconnectAsync(device.Id);
        await PollAsync();
    }

    private bool _wifiChanging;
    private bool _applyingHostState;  // true while copying host state into properties

    partial void OnWifiEnabledChanged(bool value)
    {
        if (_applyingHostState) return;  // value came from the host, not the user
        _wifiChanging = true;
        _ = Task.Run(async () =>
        {
            await _host.SetWifiAsync(value);
            _wifiChanging = false;
        });
    }

    [RelayCommand]
    private async Task RestartAsAdminAsync()
    {
        // Ask the running engine to exit, then start it elevated.
        await _host.ShutdownHostAsync();
        for (var i = 0; i < 30 && System.Diagnostics.Process.GetProcessesByName("DisplayMasterHost").Length > 0; i++)
            await Task.Delay(100);
        HostExecutable.LaunchElevated();
        await Task.Delay(1500);
        await PollAsync();
    }

    public Task ShutdownHostAsync() => _host.ShutdownHostAsync();

    public void Dispose()
    {
        _timer.Stop();
        _host.Dispose();
    }
}
