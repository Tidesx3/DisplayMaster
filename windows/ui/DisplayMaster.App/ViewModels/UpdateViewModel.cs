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
    /// <summary>Result of "Check for updates" on the Settings page.</summary>
    [ObservableProperty] public partial string CheckResult { get; set; } = "";

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

    [RelayCommand]
    private async Task CheckNowAsync()
    {
        CheckResult = Loc.S("Update_Checking");
        try
        {
            var update = await UpdateService.CheckOrThrowAsync();
            if (update is null)
            {
                CheckResult = Loc.S("Update_Latest");
                return;
            }
            Show(update);
            CheckResult = Loc.F("Update_Found", update.Version.ToString(3));
        }
        catch (Exception)
        {
            CheckResult = Loc.S("Update_Offline");
        }
    }

    private async Task CheckAsync()
    {
        if (IsBusy) return;
        var update = await UpdateService.CheckAsync();
        if (update is null) return;
        Show(update);
    }

    private void Show(UpdateInfo update)
    {
        _update = update;
        Message = Loc.F("Update_Available", update.Version.ToString(3), UpdateService.CurrentVersion.ToString(3));
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
                Error = Loc.S("Update_NeedsAdmin");
        }
        catch (Exception e)
        {
            Error = Loc.F("Update_Failed", e.Message);
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
