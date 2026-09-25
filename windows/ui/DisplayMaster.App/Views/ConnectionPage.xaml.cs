using DisplayMaster.ViewModels;
using Microsoft.UI.Xaml.Controls;

namespace DisplayMaster.Views;

public sealed partial class ConnectionPage : Page
{
    public MainViewModel ViewModel => App.ViewModel;

    public ConnectionPage()
    {
        InitializeComponent();
    }

    public static string YesNo(bool value, string yes, string no) => value ? yes : no;

    private void Forget_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if ((sender as Microsoft.UI.Xaml.FrameworkElement)?.Tag is Services.TrustedDevice device)
            ViewModel.ForgetDeviceCommand.Execute(device);
    }
}
