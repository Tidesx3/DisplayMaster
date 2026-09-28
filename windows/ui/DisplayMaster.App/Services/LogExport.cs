using System.Diagnostics;
using System.Globalization;
using System.Text;
using Microsoft.Win32;

namespace DisplayMaster.Services;

/// <summary>
/// Writes what a bug report needs into one text file on the desktop: PC/GPU/driver info, the
/// monitor list, recent display-driver events (GPU resets, hardware errors), the settings and the
/// end of the engine and app logs. The pairing key and the list of paired devices stay out.
/// </summary>
public static class LogExport
{
    // Enough for the last few sessions while staying small enough to paste or attach.
    private const int EngineLogLines = 5000;
    private const int CrashLogLines = 300;

    /// <summary>Writes the report and returns its path.</summary>
    public static Task<string> CreateAsync() => Task.Run(() =>
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DisplayMaster");
        var sb = new StringBuilder();
        Section(sb, "System", SystemInfo());
        Section(sb, "Display driver events, last 14 days", DriverEvents());
        Section(sb, "Settings (host.ini)", ReadLines(Path.Combine(dir, "host.ini")).DefaultIfEmpty("(none)"));
        // host.old.log holds what came before host.log (the engine rotates it at 4 MB).
        var engine = ReadLines(Path.Combine(dir, "host.old.log")).Concat(ReadLines(Path.Combine(dir, "host.log")));
        Section(sb, $"Engine log (last {EngineLogLines} lines)", engine.TakeLast(EngineLogLines).DefaultIfEmpty("(empty)"));
        Section(sb, "App crashes", ReadLines(Path.Combine(dir, "app-crash.log")).TakeLast(CrashLogLines).DefaultIfEmpty("(none)"));

        var path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
            $"DisplayMaster-logs-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
        File.WriteAllText(path, sb.ToString(), new UTF8Encoding(false));
        return path;
    });

    private static void Section(StringBuilder sb, string title, IEnumerable<string> lines)
    {
        sb.AppendLine($"===== {title} =====");
        foreach (var line in lines) sb.AppendLine(line);
        sb.AppendLine();
    }

    private static IEnumerable<string> ReadLines(string path)
    {
        if (!File.Exists(path)) return [];
        // The engine keeps host.log open for writing.
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var reader = new StreamReader(stream, Encoding.UTF8);
        var lines = new List<string>();
        while (reader.ReadLine() is { } line) lines.Add(line);
        return lines;
    }

    // ---------------------------------------------------------------- system

    private static IEnumerable<string> SystemInfo()
    {
        yield return $"DisplayMaster {UpdateService.CurrentVersion.ToString(3)}, exported {DateTime.Now:yyyy-MM-dd HH:mm:ss zzz}";
        using (var nt = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
            yield return $"Windows {nt?.GetValue("DisplayVersion")} build {nt?.GetValue("CurrentBuild")}.{nt?.GetValue("UBR")}";
        using (var bios = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\BIOS"))
            yield return $"PC: {bios?.GetValue("SystemManufacturer")} {bios?.GetValue("SystemProductName")}, BIOS {bios?.GetValue("BIOSVersion")}";
        using (var cpu = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0"))
            yield return $"CPU: {(cpu?.GetValue("ProcessorNameString") as string)?.Trim()}";
        using (var ci = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\CI\Policy"))
            yield return "Smart App Control: " + (ci?.GetValue("VerifiedAndReputablePolicyState") switch
            {
                0 => "off", 1 => "on", 2 => "evaluation", _ => "unknown",
            });

        yield return "";
        yield return "Display adapters (incl. virtual and inactive ones):";
        // Remote Desktop adds one identical adapter per session: list repeats once.
        foreach (var group in DisplayAdapters().GroupBy(a => a))
            yield return group.Count() > 1 ? $"  {group.Key} (x{group.Count()})" : "  " + group.Key;

        yield return "";
        yield return "Monitors (DisplayMasterHost --list-monitors):";
        var engine = HostExecutable.Find();
        yield return engine is not null ? Run(engine, "--list-monitors", Encoding.UTF8).TrimEnd() : "(engine not found)";
    }

    private static List<string> DisplayAdapters()
    {
        var result = new List<string>();
        // The display class key: one numbered subkey per adapter driver.
        using var cls = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}");
        foreach (var sub in cls?.GetSubKeyNames().Where(n => n.All(char.IsAsciiDigit)) ?? [])
        {
            try
            {
                using var a = cls!.OpenSubKey(sub);
                result.Add($"{a?.GetValue("DriverDesc")}: driver {a?.GetValue("DriverVersion")} from {a?.GetValue("DriverDate")} ({a?.GetValue("ProviderName")})");
            }
            catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException)
            {
                // Not readable without admin rights: skip.
            }
        }
        return result;
    }

    // ---------------------------------------------------------------- driver events

    private static IEnumerable<string> DriverEvents()
    {
        const string LastTwoWeeks = "TimeCreated[timediff(@SystemTime) <= 1209600000]";
        yield return "-- Display driver stopped responding and recovered (System, Display 4101) --";
        yield return Events("System", $"*[System[Provider[@Name='Display'] and EventID=4101 and {LastTwoWeeks}]]");
        yield return "-- GPU hangs and other live kernel reports (Application, LiveKernelEvent) --";
        var reports = Events("Application",
                $"*[System[Provider[@Name='Windows Error Reporting'] and EventID=1001 and {LastTwoWeeks}]]", 100)
            .Split("Event[", StringSplitOptions.RemoveEmptyEntries)
            .Where(e => e.Contains("LiveKernelEvent"))
            .Select(e => "Event[" + e.TrimEnd())
            .ToList();
        yield return reports.Count > 0 ? string.Join(Environment.NewLine, reports) : "(none)";
        yield return "-- Hardware errors (System, WHEA-Logger) --";
        yield return Events("System", $"*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger'] and {LastTwoWeeks}]]");
    }

    private static string Events(string log, string query, int count = 30) =>
        Run("wevtutil.exe", $"qe {log} \"/q:{query}\" /f:text /rd:true /c:{count}", AnsiEncoding()).TrimEnd() is { Length: > 0 } s
            ? s
            : "(none)";

    // Redirected, wevtutil writes in the ANSI code page (and pads some fields with NULs).
    private static Encoding AnsiEncoding()
    {
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        return Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.ANSICodePage);
    }

    private static string Run(string exe, string args, Encoding encoding)
    {
        try
        {
            using var p = Process.Start(new ProcessStartInfo(exe, args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = encoding,
                StandardErrorEncoding = encoding,
            })!;
            var output = p.StandardOutput.ReadToEndAsync();
            var errors = p.StandardError.ReadToEndAsync();
            if (!p.WaitForExit(15000))
            {
                p.Kill();
                return $"({Path.GetFileName(exe)} timed out)";
            }
            return (output.Result + errors.Result).Replace("\0", "");
        }
        catch (Exception e)
        {
            return $"({Path.GetFileName(exe)} failed: {e.Message})";
        }
    }
}
