using CommunityToolkit.Mvvm.Input;
using DisplayMaster.ViewModels;
using H.NotifyIcon;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;

namespace DisplayMaster;

public partial class App : Application
{
    private const string InstanceMutexName = "DisplayMaster.App.SingleInstance";
    private const string ShowEventName = "DisplayMaster.App.Show";

    private MainWindow? _window;
    private TaskbarIcon? _tray;
    private Mutex? _instanceMutex;
    private EventWaitHandle? _showEvent;
    private bool _quitting;
    private bool _trayHintShown;
    private readonly Dictionary<int, ContentDialog> _approvalDialogs = new();

    public static MainViewModel ViewModel { get; private set; } = null!;

    public App()
    {
        InitializeComponent();
        // A crash otherwise only shows up as a generic XAML error code in the event log.
        UnhandledException += (_, e) => LogCrash(e.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, e) => LogCrash(e.ExceptionObject as Exception);
    }

    private static void LogCrash(Exception? e)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DisplayMaster");
            Directory.CreateDirectory(dir);
            File.AppendAllText(Path.Combine(dir, "app-crash.log"),
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} DisplayMaster {Services.UpdateService.CurrentVersion}{Environment.NewLine}{e}{Environment.NewLine}{Environment.NewLine}");
        }
        catch
        {
            // Nothing sensible left to do while crashing.
        }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        if (Environment.GetCommandLineArgs().Contains("--smoke-test"))
        {
            SmokeTest();
            return;
        }
        // Second launch: ask the running instance to show itself, then exit.
        _instanceMutex = new Mutex(true, InstanceMutexName, out var first);
        _showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEventName);
        if (!first)
        {
            _showEvent.Set();
            Exit();
            return;
        }
        var dispatcher = DispatcherQueue.GetForCurrentThread();
        ThreadPool.RegisterWaitForSingleObject(_showEvent, (_, _) => dispatcher.TryEnqueue(ShowWindow), null, -1, false);

        ViewModel = new MainViewModel(dispatcher);
        _window = new MainWindow();
        _window.AppWindow.Closing += (_, e) =>
        {
            if (_quitting) return;
            // Closing the window keeps DisplayMaster running in the tray.
            e.Cancel = true;
            HideToTray();
        };
        // Minimizing does the same: the window leaves the taskbar and lives in the tray.
        _window.AppWindow.Changed += (sender, e) =>
        {
            if (e.DidPresenterChange || sender.Presenter is not OverlappedPresenter p) return;
            if (p.State == OverlappedPresenterState.Minimized && sender.IsVisible) HideToTray();
        };
        CreateTrayIcon();
        // The installer replaces this app and the engine, then starts the app again.
        ViewModel.Update.InstallerStarted += () => _ = QuitAsync();
        ViewModel.ApprovalRequested += device => _ = AskApprovalAsync(device);
        ViewModel.ApprovalResolved += id =>
        {
            if (_approvalDialogs.Remove(id, out var dialog)) dialog.Hide();
        };
        ViewModel.DevicesChanged += count =>
        {
            if (_tray is not null)
                _tray.ToolTipText = count == 0 ? "DisplayMaster" : $"DisplayMaster – {count} device{(count == 1 ? "" : "s")} connected";
        };

        // Started at logon with --tray: stay in the notification area.
        if (!Environment.GetCommandLineArgs().Contains("--tray")) _window.Activate();
    }

    /// <summary>
    /// tools\package.ps1 runs the published app with --smoke-test before releasing: builds the
    /// view models and every page without showing a window, starting the engine or taking the
    /// single-instance lock, then exits 0. A startup crash fails the release instead of users.
    /// </summary>
    private void SmokeTest()
    {
        Services.HostClient.LaunchEngine = false;
        var dispatcher = DispatcherQueue.GetForCurrentThread();
        ViewModel = new MainViewModel(dispatcher);
        _window = new MainWindow();
        dispatcher.TryEnqueue(async () =>
        {
            foreach (var page in new[] { "devices", "pen", "connection", "settings" })
            {
                _window.ShowPage(page);
                await Task.Delay(300);
            }
            Environment.Exit(0);
        });
    }

    private void CreateTrayIcon()
    {
        var menu = new MenuFlyout();
        var open = new MenuFlyoutItem { Text = "Open DisplayMaster", Icon = new FontIcon { Glyph = "" } };
        open.Command = new RelayCommand(ShowWindow);
        var quit = new MenuFlyoutItem { Text = "Quit", Icon = new FontIcon { Glyph = "" } };
        quit.Command = new AsyncRelayCommand(QuitAsync);
        menu.Items.Add(open);
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(quit);

        _tray = new TaskbarIcon
        {
            ToolTipText = "DisplayMaster",
            IconSource = new BitmapImage(new Uri(Path.Combine(AppContext.BaseDirectory, "Assets", "TrayIcon.ico"))),
            ContextFlyout = menu,
            ContextMenuMode = ContextMenuMode.PopupMenu,
            NoLeftClickDelay = true,
            LeftClickCommand = new RelayCommand(ShowWindow),
        };
        _tray.ForceCreate(enablesEfficiencyMode: false);
    }

    /// <summary>"Allow this device?" for unknown Wi-Fi devices (they could otherwise control the PC).</summary>
    private async Task AskApprovalAsync(Services.PendingDevice device)
    {
        ShowWindow();
        if (_window?.Content?.XamlRoot is not { } root) return;
        var remember = new CheckBox { Content = "Remember this device", IsChecked = true };
        var body = new StackPanel { Spacing = 12 };
        body.Children.Add(new TextBlock
        {
            Text = $"{device.Name} ({device.Model}) wants to use this PC as a display over Wi-Fi from {device.Address}. " +
                   "Allowed devices can see this screen and control the mouse and keyboard.",
            TextWrapping = TextWrapping.Wrap,
        });
        if (!string.IsNullOrEmpty(device.Code))
        {
            // Same code on both screens = nobody on the network is sitting in between.
            body.Children.Add(new TextBlock
            {
                Text = "Only allow it if the device shows the same code:",
                TextWrapping = TextWrapping.Wrap,
            });
            body.Children.Add(new TextBlock
            {
                Text = device.Code,
                FontSize = 34,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                FontFamily = new Microsoft.UI.Xaml.Media.FontFamily("Cascadia Mono, Consolas"),
                CharacterSpacing = 120,
                HorizontalAlignment = HorizontalAlignment.Center,
                IsTextSelectionEnabled = false,
            });
        }
        body.Children.Add(remember);
        var dialog = new ContentDialog
        {
            XamlRoot = root,
            Title = "Allow this device?",
            Content = body,
            PrimaryButtonText = "Allow",
            CloseButtonText = "Deny",
            DefaultButton = ContentDialogButton.Close,
        };
        _approvalDialogs[device.Id] = dialog;
        var result = await dialog.ShowAsync();
        // Hidden because the request vanished: nothing to answer.
        if (!_approvalDialogs.Remove(device.Id)) return;
        await ViewModel.AnswerApprovalAsync(device.Id, result == ContentDialogResult.Primary, remember.IsChecked == true);
    }

    private void HideToTray()
    {
        if (_window is null) return;
        _window.AppWindow.Hide();
        if (_trayHintShown || _tray is null) return;
        // Once per run, so it doesn't look like the app quit (Quit is in the tray menu).
        _trayHintShown = true;
        _tray.ShowNotification("DisplayMaster is still running",
            "Connected devices keep working. Open or quit DisplayMaster from its icon in the notification area.");
    }

    private void ShowWindow()
    {
        if (_window is null) return;
        _window.AppWindow.Show();
        if (_window.AppWindow.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized } p) p.Restore();
        _window.Activate();
    }

    private async Task QuitAsync()
    {
        _quitting = true;
        // The engine stops with the app, like closing any other display tool.
        await ViewModel.ShutdownHostAsync();
        ViewModel.Dispose();
        _tray?.Dispose();
        _window?.Close();
        _instanceMutex?.ReleaseMutex();
        Exit();
    }
}
