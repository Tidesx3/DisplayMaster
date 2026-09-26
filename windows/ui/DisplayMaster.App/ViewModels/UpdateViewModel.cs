using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DisplayMaster.Services;
using Microsoft.UI.Dispatching;

namespace DisplayMaster.ViewModels;

/// <summary>Checks GitHub for a newer release shortly after start and then twice a day.</summary>
public sealed partial class UpdateViewModel : ObservableObject
{
    private readonly DispatcherQueueTimer _timer;
    private UpdateInfo? _update;

    [ObservableProperty] public partial bool IsAvailable { get; set; }
    [ObservableProperty] public partial string Message { get; set; } = "";
    [ObservableProperty] public partial bool IsBusy { get; set; }
    [ObservableProperty] public partial double Progress { get; set; }
    [ObservableProperty] public partial string Error { get; set; } = "";

    public string NotesUrl => _update?.NotesUrl ?? UpdateService.ReleasesPage;

    /// <summary>The installer is running: the app should quit so it can be replaced.</summary>
    public event Action? InstallerStarted;

    public UpdateViewModel(DispatcherQueue dispatcher)
    {
        _timer = dispatcher.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(10);  // first check: once the app has settled
        _timer.Tick += async (_, _) =>
        {
            _timer.Interval = TimeSpan.FromHours(12);
            await CheckAsync();
        };
        _timer.Start();
    }

    private async Task CheckAsync()
    {
        if (IsBusy) return;
        var update = await UpdateService.CheckAsync();
        if (update is null) return;
        _update = update;
        Message = $"DisplayMaster {update.Version.ToString(3)} is available (you have {UpdateService.CurrentVersion.ToString(3)}).";
        OnPropertyChanged(nameof(NotesUrl));
        IsAvailable = true;
    }

    [RelayCommand]
    private async Task InstallAsync()
    {
        if (_update is null || IsBusy) return;
        IsBusy = true;
        Error = "";
        try
        {
            var path = await UpdateService.DownloadAsync(_update, new Progress<double>(p => Progress = p * 100));
            if (UpdateService.RunInstaller(path))
                InstallerStarted?.Invoke();
            else
                Error = "The update needs administrator permission.";
        }
        catch (Exception e)
        {
            Error = $"Update failed: {e.Message}";
        }
        finally
        {
            IsBusy = false;
            Progress = 0;
        }
    }

    [RelayCommand]
    private void Dismiss() => IsAvailable = false;
}
