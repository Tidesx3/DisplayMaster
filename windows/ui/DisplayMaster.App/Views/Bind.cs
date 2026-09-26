using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using QRCoder;

namespace DisplayMaster.Views;

/// <summary>Small x:Bind helper functions.</summary>
public static class Bind
{
    public static bool Not(bool value) => !value;
    public static Visibility VisibleIf(bool value) => value ? Visibility.Visible : Visibility.Collapsed;
    public static Visibility CollapsedIf(bool value) => value ? Visibility.Collapsed : Visibility.Visible;

    /// <summary>QR code image for `text` (a fresh image per call: each page owns its own).</summary>
    public static ImageSource? QrCode(string text)
    {
        if (string.IsNullOrEmpty(text)) return null;
        using var generator = new QRCodeGenerator();
        using var data = generator.CreateQrCode(text, QRCodeGenerator.ECCLevel.M);
        var png = new PngByteQRCode(data).GetGraphic(8);
        var image = new BitmapImage();
        image.SetSource(new MemoryStream(png).AsRandomAccessStream());  // the image owns the stream
        return image;
    }
}
