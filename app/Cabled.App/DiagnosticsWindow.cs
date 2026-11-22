using System.Text;
using System.Text.Json;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;
using Windows.Graphics;

namespace Cabled.App;

/// <summary>
/// Diagnostics (windows-app.md, R11): what every stage is doing right now, the PC and iPhone log,
/// and "Copy report", which puts a text report on the clipboard. Nothing is sent anywhere.
/// </summary>
internal sealed class DiagnosticsWindow : Window
{
    private readonly Func<string?> _statusJson;
    private readonly LogBook _log;
    private readonly TextBlock _stats = new() { FontFamily = (FontFamily)Application.Current.Resources["FontMono"], FontSize = 12, IsTextSelectionEnabled = true };
    private readonly TextBox _logBox = new()
    {
        IsReadOnly = true,
        AcceptsReturn = true,
        TextWrapping = TextWrapping.NoWrap,
        FontFamily = (FontFamily)Application.Current.Resources["FontMono"],
        FontSize = 12,
    };
    private readonly TextBlock _copied = new() { VerticalAlignment = VerticalAlignment.Center, Opacity = 0.7 };
    private readonly DispatcherQueueTimer _timer;
    private int _shownLogVersion = -1;

    public DiagnosticsWindow(Func<string?> statusJson, LogBook log)
    {
        _statusJson = statusJson;
        _log = log;
        Title = "Cabled diagnostics";
        AppWindow.Resize(new SizeInt32(900, 720));
        string icon = Path.Combine(AppContext.BaseDirectory, "Assets", "Cabled.ico");
        if (File.Exists(icon)) AppWindow.SetIcon(icon);
        SystemBackdrop = new MicaBackdrop();

        var copy = new Button { Content = "Copy report", Style = (Style)Application.Current.Resources["AccentButtonStyle"] };
        copy.Click += (_, _) =>
        {
            var package = new DataPackage();
            package.SetText(BuildReport());
            Clipboard.SetContent(package);
            _copied.Text = "Copied. Paste it wherever you're asking for help; nothing was sent.";
        };
        var logs = new Button { Content = "Open the camera log folder" };
        logs.Click += (_, _) =>
        {
            string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Cabled", "logs");
            if (Directory.Exists(folder))
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", $"\"{folder}\"") { UseShellExecute = true });
        };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { copy, logs, _copied } };

        var grid = new Grid { Padding = new Thickness(24), RowSpacing = 16 };
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(buttons);
        var statsBorder = new Border { Child = _stats };
        Grid.SetRow(statsBorder, 1);
        grid.Children.Add(statsBorder);
        var logTitle = new TextBlock { Text = "Log (PC and iPhone)", Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"] };
        Grid.SetRow(logTitle, 2);
        grid.Children.Add(logTitle);
        Grid.SetRow(_logBox, 3);
        grid.Children.Add(_logBox);
        Content = grid;

        _timer = DispatcherQueue.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += (_, _) => Refresh();
        _timer.Start();
        Closed += (_, _) => _timer.Stop();
        Refresh();
    }

    private void Refresh()
    {
        _stats.Text = DescribeStatus(_statusJson());
        if (_log.Version != _shownLogVersion)
        {
            _shownLogVersion = _log.Version;
            _logBox.Text = string.Join("\r\n", _log.Lines(500));
            _logBox.SelectionStart = _logBox.Text.Length;  // keep the newest line in view
        }
    }

    private static string DescribeStatus(string? json)
    {
        if (json is null) return "No status from the engine.";
        try
        {
            using var doc = JsonDocument.Parse(json);
            var s = doc.RootElement;
            var d = s.GetProperty("device");
            var v = s.GetProperty("video");
            var c = s.GetProperty("counters");
            var link = s.GetProperty("link");
            var phone = s.GetProperty("phone");
            var sb = new StringBuilder();
            sb.AppendLine($"State        {s.GetProperty("state").GetString()} · {s.GetProperty("headline").GetString()}");
            sb.AppendLine($"iPhone       {Str(d, "model")} · iOS {Str(d, "system")} · companion {Str(d, "companion")} · signing expires {Expiry(Str(d, "signingExpiry"))}");
            string limit = Str(phone, "thermalLimit");
            sb.AppendLine($"Phone        thermal {Str(phone, "thermal")}{(limit.Length > 0 ? $" (capture reduced for heat: {limit})" : "")} · battery {Battery(phone)}");
            sb.AppendLine($"USB link     {link.GetProperty("mbps").GetDouble():0} Mbit/s measured · asked phone for {Str(link, "config")}");
            sb.AppendLine($"Video in     {v.GetProperty("width").GetUInt32()}×{v.GetProperty("height").GetUInt32()} · {v.GetProperty("fps").GetDouble():0.0} fps · {v.GetProperty("mbps").GetDouble():0.0} Mbit/s");
            sb.AppendLine($"Decode       {(v.GetProperty("hardwareDecode").GetBoolean() ? "GPU (D3D11VA)" : "CPU (software fallback)")} · {v.GetProperty("decodeMs").GetDouble():0.0} ms per frame");
            sb.AppendLine($"Latency      capture → decoded {v.GetProperty("latencyMs").GetDouble():0.0} ms");
            string colour = s.TryGetProperty("controls", out var ctl) ? Str(ctl, "colorSpace") : "";
            sb.AppendLine($"Colour       camera {(colour.Length > 0 ? colour : "?")} (should be rec709) → BT.709 limited range to apps");
            sb.AppendLine($"Counters     {c.GetProperty("framesDecoded").GetUInt64()} frames · {c.GetProperty("keyframeRequests").GetUInt64()} keyframe requests (≈ drops) · {c.GetProperty("reconnects").GetUInt64()} reconnects" +
                          (s.TryGetProperty("decoderResets", out var dr) && dr.GetUInt64() > 0 ? $" · {dr.GetUInt64()} decoder rebuilds" : ""));
            var vcam = s.GetProperty("vcam");
            sb.AppendLine($"Camera out   {(vcam.GetProperty("inUse").GetBoolean() ? "in use" : "not in use")} · {Str(vcam, "state")}");
            return sb.ToString();
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException)
        {
            return "Status couldn't be read: " + e.Message;
        }
    }

    private static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() ?? "" : "";

    private static string Battery(JsonElement phone) =>
        phone.TryGetProperty("battery", out var b) && b.ValueKind == JsonValueKind.Number && b.GetDouble() >= 0
            ? $"{b.GetDouble() * 100:0}%" : "?";

    internal static string Expiry(string iso) =>
        DateTimeOffset.TryParse(iso, out var when) ? $"{when.LocalDateTime:g} ({(when - DateTimeOffset.Now).TotalDays:0.0} days)" : "n/a";

    private string BuildReport()
    {
        var sb = new StringBuilder();
        sb.AppendLine("Cabled diagnostics report");
        sb.AppendLine($"Created  {DateTimeOffset.Now:u}");
        sb.AppendLine($"App      {typeof(App).Assembly.GetName().Version?.ToString(3)} · engine {NativeEngine.Version}");
        sb.AppendLine($"Windows  {Environment.OSVersion.VersionString}");
        sb.AppendLine();
        sb.AppendLine(DescribeStatus(_statusJson()));
        string? json = _statusJson();
        if (json is not null)
        {
            sb.AppendLine("Raw status:");
            using var doc = JsonDocument.Parse(json);
            sb.AppendLine(JsonSerializer.Serialize(doc.RootElement, new JsonSerializerOptions { WriteIndented = true }));
        }
        sb.AppendLine();
        sb.AppendLine("Log (newest last):");
        foreach (string line in _log.Lines(400)) sb.AppendLine(line);
        return sb.ToString();
    }
}

/// <summary>The last couple of thousand log lines, with times, for diagnostics and reports.</summary>
internal sealed class LogBook
{
    private const int Max = 2000;
    private readonly Queue<string> _lines = new();
    public int Version { get; private set; }

    public void Add(string text)
    {
        foreach (string line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            _lines.Enqueue($"{DateTime.Now:HH:mm:ss.fff}  {line.TrimEnd('\r')}");
            if (_lines.Count > Max) _lines.Dequeue();
        }
        Version++;
    }

    public IEnumerable<string> Lines(int newest) => _lines.Skip(Math.Max(0, _lines.Count - newest));
}
