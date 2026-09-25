using Microsoft.UI.Xaml;

namespace DisplayMaster.Views;

/// <summary>Small x:Bind helper functions.</summary>
public static class Bind
{
    public static bool Not(bool value) => !value;
    public static Visibility VisibleIf(bool value) => value ? Visibility.Visible : Visibility.Collapsed;
    public static Visibility CollapsedIf(bool value) => value ? Visibility.Collapsed : Visibility.Visible;
}
