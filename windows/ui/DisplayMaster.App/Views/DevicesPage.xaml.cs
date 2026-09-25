using DisplayMaster.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace DisplayMaster.Views;

public sealed partial class DevicesPage : Page
{
    public MainViewModel ViewModel => App.ViewModel;

    public DevicesPage()
    {
        InitializeComponent();
    }

    private void Disconnect_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is DeviceViewModel device)
            ViewModel.DisconnectCommand.Execute(device);
    }
}
