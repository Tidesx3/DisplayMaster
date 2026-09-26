using System.Diagnostics;
using System.Net.Http.Json;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace DisplayMaster.Services;

public sealed record UpdateInfo(Version Version, string SetupUrl, string SetupName, string Sha256, string NotesUrl);

/// <summary>
/// Updates come from the GitHub releases of <see cref="Repository"/>: the newest release's
/// DisplayMaster-Setup-*.exe asset is downloaded, checked against the SHA-256 digest GitHub
/// publishes for it, and run silently (it closes this app and starts it again when done).
/// </summary>
public static class UpdateService
{
    public const string Repository = "Tidesx3/DisplayMaster";
    public static readonly string ReleasesPage = $"https://github.com/{Repository}/releases";

    private static readonly HttpClient Http = CreateClient();
    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };

    public static Version CurrentVersion { get; } = ParseVersion(
        Assembly.GetExecutingAssembly().GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion)
        ?? new Version(0, 0, 0);

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        // GitHub's API rejects requests without a User-Agent.
        client.DefaultRequestHeaders.UserAgent.ParseAdd($"DisplayMaster/{Assembly.GetExecutingAssembly().GetName().Version}");
        client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return client;
    }

    /// <summary>"v1.2.3", "1.2.3+abc" -> 1.2.3; null if it isn't a version.</summary>
    public static Version? ParseVersion(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        var core = text.Trim().TrimStart('v', 'V').Split('+', '-')[0];
        return Version.TryParse(core, out var v) ? new Version(v.Major, v.Minor, Math.Max(0, v.Build)) : null;
    }

    /// <summary>The newest release if it is newer than this app, otherwise null. Never throws.</summary>
    public static async Task<UpdateInfo?> CheckAsync(CancellationToken ct = default)
    {
        try
        {
            return await CheckOrThrowAsync(ct);
        }
        catch (Exception e)
        {
            Debug.WriteLine($"Update check failed: {e.Message}");  // offline, rate limited, no releases yet
            return null;
        }
    }

    /// <summary>Like <see cref="CheckAsync"/>, but failures (offline, GitHub unreachable) throw.</summary>
    public static async Task<UpdateInfo?> CheckOrThrowAsync(CancellationToken ct = default)
    {
        {
            var release = await Http.GetFromJsonAsync<Release>(
                $"https://api.github.com/repos/{Repository}/releases/latest", Json, ct);
            if (release is null || release.Draft || release.Prerelease) return null;
            var version = ParseVersion(release.TagName);
            if (version is null || version <= CurrentVersion) return null;
            var setup = release.Assets?.FirstOrDefault(a =>
                a.Name.StartsWith("DisplayMaster-Setup-", StringComparison.OrdinalIgnoreCase) &&
                a.Name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase));
            // Without GitHub's digest the download can't be verified: don't offer it.
            if (setup?.Digest is not { } digest || !digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase))
                return null;
            return new UpdateInfo(version, setup.BrowserDownloadUrl, setup.Name, digest["sha256:".Length..].ToLowerInvariant(),
                                  release.HtmlUrl ?? ReleasesPage);
        }
    }

    /// <summary>Downloads and verifies the installer; returns its path.</summary>
    public static async Task<string> DownloadAsync(UpdateInfo update, IProgress<double> progress, CancellationToken ct = default)
    {
        var dir = Path.Combine(Path.GetTempPath(), "DisplayMaster-Update");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, Path.GetFileName(update.SetupName));

        using (var response = await Http.GetAsync(update.SetupUrl, HttpCompletionOption.ResponseHeadersRead, ct))
        {
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? 0;
            await using var source = await response.Content.ReadAsStreamAsync(ct);
            await using var file = File.Create(path);
            var buffer = new byte[256 * 1024];
            long done = 0;
            int n;
            while ((n = await source.ReadAsync(buffer, ct)) > 0)
            {
                await file.WriteAsync(buffer.AsMemory(0, n), ct);
                done += n;
                if (total > 0) progress.Report((double)done / total);
            }
        }

        await using (var file = File.OpenRead(path))
        {
            var hash = Convert.ToHexStringLower(await SHA256.HashDataAsync(file, ct));
            if (hash != update.Sha256)
            {
                File.Delete(path);
                throw new InvalidDataException("The downloaded installer doesn't match the published checksum.");
            }
        }
        return path;
    }

    /// <summary>
    /// Runs the installer silently (Windows asks for admin rights). It replaces the running
    /// app and engine and starts the app again. False if the user declined the prompt.
    /// </summary>
    public static bool RunInstaller(string path)
    {
        try
        {
            Process.Start(new ProcessStartInfo(path, "/SILENT /SUPPRESSMSGBOXES /NORESTART /relaunch=1")
            {
                UseShellExecute = true,  // lets the installer's manifest request elevation
            });
            return true;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return false;
        }
    }

    private sealed record Release(
        string? TagName, bool Draft, bool Prerelease, string? HtmlUrl, List<Asset>? Assets);

    private sealed record Asset(
        string Name, [property: JsonPropertyName("browser_download_url")] string BrowserDownloadUrl, string? Digest);
}
