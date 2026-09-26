using System.ComponentModel;
using DisplayMaster.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace DisplayMaster.Views;

public sealed partial class PenPage : Page
{
    public PenViewModel Pen => App.ViewModel.Pen;

    public PenPage()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            Pen.PropertyChanged += OnPenChanged;
            DrawCurve();
        };
        Unloaded += (_, _) => Pen.PropertyChanged -= OnPenChanged;
    }

    private void OnPenChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(PenViewModel.CurvePoints)) DrawCurve();
    }

    // A fresh collection per draw: a PointCollection cannot be shared between Polylines.
    private void DrawCurve()
    {
        var points = new PointCollection();
        foreach (var p in Pen.CurvePoints) points.Add(p);
        CurveLine.Points = points;
    }
}
