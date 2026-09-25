using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace DisplayMaster.Services;

// Mirrors Host::status_json() in windows/host/src/session/host.cpp.
public sealed record StatusResponse(
    bool Ok, string? Version, HostInfo? Host, List<SessionInfo>? Sessions,
    List<PendingDevice>? Pending, List<TrustedDevice>? Trusted);

/// <summary>A Wi-Fi device waiting for the user to allow it.</summary>
public sealed record PendingDevice(int Id, string Name, string Model, string Address);

public sealed record TrustedDevice(string DeviceId, string Name);

public sealed record HostInfo(
    string Name, int Port, bool Wifi, bool Elevated, bool Vdd, bool Adb,
    List<string> Addresses, List<string> AdbReady, List<string> AdbUnauthorized, PenCurve? Pen);

public sealed record PenCurve(double Min, double Max, double Gamma);

public sealed record MonitorRect(int X, int Y, int W, int H);

public sealed record SessionInfo(
    int Id, string Name, string Model, string Transport, bool Streaming, string Mode, string Codec,
    int Width, int Height, int Fps, int BitrateKbps, string Encoder, string Gpu, string Monitor, bool Pen,
    double SentFps, double Mbps, double EncodeMs, double DecodeMs, int Dropped, MonitorRect Rect);

/// <summary>
/// Talks to DisplayMasterHost.exe over \\.\pipe\DisplayMaster.Control (one JSON request,
/// one JSON response). Reconnects transparently; starts the host if it isn't running.
/// </summary>
public sealed class HostClient : IDisposable
{
    private const string PipeName = "DisplayMaster.Control";
    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = true,
        NumberHandling = JsonNumberHandling.AllowReadingFromString,
    };

    private readonly SemaphoreSlim _lock = new(1, 1);
    private NamedPipeClientStream? _pipe;
    private DateTime _lastLaunchAttempt = DateTime.MinValue;

    public async Task<StatusResponse?> GetStatusAsync(CancellationToken ct = default)
    {
        var reply = await RequestAsync("{\"cmd\":\"status\"}", ct);
        return reply is null ? null : JsonSerializer.Deserialize<StatusResponse>(reply, Json);
    }

    public Task DisconnectAsync(int sessionId) => RequestAsync($"{{\"cmd\":\"disconnect\",\"id\":{sessionId}}}");

    public Task SetWifiAsync(bool enabled) =>
        RequestAsync($"{{\"cmd\":\"set_wifi\",\"enabled\":{(enabled ? "true" : "false")}}}");

    public Task ShutdownHostAsync() => RequestAsync("{\"cmd\":\"shutdown\"}");

    public Task ApproveAsync(int id, bool allow, bool remember) =>
        RequestAsync($"{{\"cmd\":\"approve\",\"id\":{id},\"allow\":{Bool(allow)},\"remember\":{Bool(remember)}}}");

    public Task ForgetDeviceAsync(string deviceId) =>
        RequestAsync($"{{\"cmd\":\"forget_device\",\"device_id\":{JsonSerializer.Serialize(deviceId)}}}");

    private static string Bool(bool b) => b ? "true" : "false";

    public Task SetPenAsync(double min, double max, double gamma) =>
        RequestAsync(string.Create(System.Globalization.CultureInfo.InvariantCulture,
            $"{{\"cmd\":\"set_pen\",\"min\":{min:0.###},\"max\":{max:0.###},\"gamma\":{gamma:0.###}}}"));

    private async Task<string?> RequestAsync(string request, CancellationToken ct = default)
    {
        await _lock.WaitAsync(ct);
        try
        {
            for (var attempt = 0; attempt < 2; attempt++)
            {
                try
                {
                    var pipe = await EnsureConnectedAsync(ct);
                    if (pipe is null) return null;
                    var bytes = Encoding.UTF8.GetBytes(request);
                    await pipe.WriteAsync(bytes, ct);
                    return await ReadMessageAsync(pipe, ct);
                }
                catch (IOException)
                {
                    DropConnection();  // host restarted: reconnect once
                }
            }
            return null;
        }
        catch (OperationCanceledException)
        {
            return null;
        }
        finally
        {
            _lock.Release();
        }
    }

    private static async Task<string> ReadMessageAsync(NamedPipeClientStream pipe, CancellationToken ct)
    {
        var buffer = new byte[64 * 1024];
        using var ms = new MemoryStream();
        do
        {
            var n = await pipe.ReadAsync(buffer, ct);
            if (n == 0) throw new IOException("pipe closed");
            ms.Write(buffer, 0, n);
        } while (!pipe.IsMessageComplete);
        return Encoding.UTF8.GetString(ms.ToArray());
    }

    private async Task<NamedPipeClientStream?> EnsureConnectedAsync(CancellationToken ct)
    {
        if (_pipe is { IsConnected: true }) return _pipe;
        DropConnection();
        var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        try
        {
            await pipe.ConnectAsync(300, ct);
            pipe.ReadMode = PipeTransmissionMode.Message;
            _pipe = pipe;
            return pipe;
        }
        catch (TimeoutException)
        {
            pipe.Dispose();
            TryLaunchHost();
            return null;
        }
    }

    private void DropConnection()
    {
        _pipe?.Dispose();
        _pipe = null;
    }

    /// <summary>Starts the host engine if nobody else did (installed builds use a logon task).</summary>
    private void TryLaunchHost()
    {
        if (DateTime.UtcNow - _lastLaunchAttempt < TimeSpan.FromSeconds(10)) return;
        _lastLaunchAttempt = DateTime.UtcNow;
        if (Process.GetProcessesByName("DisplayMasterHost").Length > 0) return;
        if (HostExecutable.RunTask()) return;  // installed: elevated, no UAC prompt
        var exe = HostExecutable.Find();
        if (exe is null) return;
        try
        {
            Process.Start(new ProcessStartInfo(exe)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = Path.GetDirectoryName(exe)!,
            });
        }
        catch (Exception e)
        {
            Debug.WriteLine($"Host launch failed: {e.Message}");
        }
    }

    public void Dispose()
    {
        DropConnection();
        _lock.Dispose();
    }
}

public static class HostExecutable
{
    /// <summary>Next to the app when installed; the CMake output folder during development.</summary>
    public static string? Find()
    {
        var candidates = new List<string> { Path.Combine(AppContext.BaseDirectory, "DisplayMasterHost.exe") };
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (var i = 0; i < 12 && dir is not null; i++, dir = dir.Parent)
        {
            foreach (var config in new[] { "Release", "Debug" })
                candidates.Add(Path.Combine(dir.FullName, "build", "windows-x64", "windows", "host", config, "DisplayMasterHost.exe"));
        }
        return candidates.FirstOrDefault(File.Exists);
    }

    /// <summary>Scheduled task created by install.ps1: runs the engine elevated at logon.</summary>
    public const string TaskName = @"\DisplayMaster\Engine";

    /// <summary>Starts the engine through the installed logon task. False if there is no task.</summary>
    public static bool RunTask()
    {
        try
        {
            using var p = Process.Start(new ProcessStartInfo("schtasks.exe", $"/Run /TN \"{TaskName}\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            });
            p!.WaitForExit(5000);
            return p.ExitCode == 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Start the engine elevated (task if installed, otherwise a UAC prompt).</summary>
    public static bool LaunchElevated()
    {
        if (RunTask()) return true;
        var exe = Find();
        if (exe is null) return false;
        try
        {
            Process.Start(new ProcessStartInfo(exe)
            {
                UseShellExecute = true,
                Verb = "runas",
                WindowStyle = ProcessWindowStyle.Hidden,
                WorkingDirectory = Path.GetDirectoryName(exe)!,
            });
            return true;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return false;  // user declined the UAC prompt
        }
    }
}
