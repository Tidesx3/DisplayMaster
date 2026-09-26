using DisplayMaster.ViewModels;
using Microsoft.UI.Xaml.Controls;

namespace DisplayMaster.Views;

public sealed partial class SettingsPage : Page
{
    public SettingsViewModel Settings => App.ViewModel.Settings;
    public UpdateViewModel Update => App.ViewModel.Update;

    public SettingsPage()
    {
        InitializeComponent();
    }
}
