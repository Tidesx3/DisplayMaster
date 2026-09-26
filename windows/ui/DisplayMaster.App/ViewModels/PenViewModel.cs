using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DisplayMaster.Services;
using Microsoft.UI.Dispatching;
using Windows.Foundation;

namespace DisplayMaster.ViewModels;

/// <summary>
/// Pen pressure shaping (same math as dm::PressureCurve in protocol/include/dm/input_math.h):
/// output = clamp((p - min) / (max - min))^gamma.
/// </summary>
public sealed partial class PenViewModel : ObservableObject
{
    public const double GraphSize = 220;
    private const double MaxFeel = 100;  // slider range -100..100 -> gamma 2.5^(-1..1)

    private readonly HostClient _host;
    private readonly DispatcherQueueTimer _debounce;
    private bool _loaded;

    /// <summary>Ignore pressure below this (0..0.5).</summary>
    [ObservableProperty] public partial double MinPercent { get; set; }
    /// <summary>Full pressure at or above this (0.5..1).</summary>
    [ObservableProperty] public partial double MaxPercent { get; set; } = 100;
    /// <summary>-100 = softest, 0 = linear, 100 = firmest.</summary>
    [ObservableProperty] public partial double Feel { get; set; }
    /// <summary>
    /// Graph points in <see cref="GraphSize"/> units. A plain array, not a PointCollection:
    /// a PointCollection can belong to only one Polyline, and the page is recreated on every
    /// visit while the view model lives on, so each page builds its own collection.
    /// </summary>
    [ObservableProperty] public partial Point[] CurvePoints { get; set; } = [];

    public double Gamma => Math.Pow(2.5, Feel / MaxFeel);
    public string FeelText => Math.Abs(Feel) < 5 ? Loc.S("Feel_Linear") : Feel < 0 ? Loc.S("Feel_Softer") : Loc.S("Feel_Firmer");

    public PenViewModel(HostClient host, DispatcherQueue dispatcher)
    {
        _host = host;
        _debounce = dispatcher.CreateTimer();
        _debounce.Interval = TimeSpan.FromMilliseconds(300);
        _debounce.IsRepeating = false;
        _debounce.Tick += async (_, _) => await _host.SetPenAsync(MinPercent / 100, MaxPercent / 100, Gamma);
        UpdateCurve();
    }

    /// <summary>Takes the engine's saved curve once; afterwards the sliders are the source of truth.</summary>
    public void Load(PenCurve curve)
    {
        if (_loaded) return;
        _loaded = true;
        _suppressSend = true;
        MinPercent = Math.Round(curve.Min * 100);
        MaxPercent = Math.Round(curve.Max * 100);
        Feel = Math.Round(Math.Log(Math.Max(0.2, curve.Gamma)) / Math.Log(2.5) * MaxFeel);
        _suppressSend = false;
        UpdateCurve();
    }

    private bool _suppressSend;

    partial void OnMinPercentChanged(double value) => Changed();
    partial void OnMaxPercentChanged(double value) => Changed();
    partial void OnFeelChanged(double value)
    {
        OnPropertyChanged(nameof(FeelText));
        Changed();
    }

    private void Changed()
    {
        UpdateCurve();
        if (_suppressSend) return;
        _debounce.Stop();
        _debounce.Start();
    }

    private void UpdateCurve()
    {
        double min = MinPercent / 100, max = Math.Max(min + 0.05, MaxPercent / 100), gamma = Gamma;
        var points = new Point[65];
        for (var i = 0; i < points.Length; i++)
        {
            var p = i / 64.0;
            var t = Math.Clamp((p - min) / (max - min), 0, 1);
            var output = Math.Pow(t, gamma);
            points[i] = new Point(p * GraphSize, (1 - output) * GraphSize);
        }
        CurvePoints = points;
    }

    [RelayCommand]
    private void Reset()
    {
        MinPercent = 0;
        MaxPercent = 100;
        Feel = 0;
    }
}
