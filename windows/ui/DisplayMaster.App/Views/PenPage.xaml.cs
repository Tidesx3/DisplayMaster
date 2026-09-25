using DisplayMaster.ViewModels;
using Microsoft.UI.Xaml.Controls;

namespace DisplayMaster.Views;

public sealed partial class PenPage : Page
{
    public PenViewModel Pen => App.ViewModel.Pen;

    public PenPage()
    {
        InitializeComponent();
    }
}
