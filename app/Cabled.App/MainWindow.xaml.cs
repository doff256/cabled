using System.Runtime.InteropServices.WindowsRuntime;
using System.Text.Json;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;

namespace Cabled.App;

public sealed partial class MainWindow : Window
{
    private readonly NativeEngine _engine;
    private readonly DispatcherQueueTimer _previewTimer;
    private readonly DispatcherQueueTimer _statusTimer;
    private WriteableBitmap? _bitmap;
    private byte[] _frame = new byte[960 * 540 * 4];
    private ulong _frameSeq;
    private string _state = "";

    public MainWindow(string? tcpTarget)
    {
        InitializeComponent();
        AppWindow.Resize(new SizeInt32(1100, 760));
        Title = $"Cabled {NativeEngine.Version}";

        _engine = new NativeEngine(new
        {
            tcpTarget = tcpTarget ?? "",
            previewMaxWidth = 960,
        });
        _engine.Start();

        _previewTimer = DispatcherQueue.CreateTimer();
        _previewTimer.Interval = TimeSpan.FromMilliseconds(16);
        _previewTimer.Tick += (_, _) => UpdatePreview();
        _previewTimer.Start();

        _statusTimer = DispatcherQueue.CreateTimer();
        _statusTimer.Interval = TimeSpan.FromMilliseconds(250);
        _statusTimer.Tick += (_, _) => UpdateStatus();
        _statusTimer.Start();

        // Save work while minimised: no preview conversion when nobody can see it.
        AppWindow.Changed += (_, e) =>
        {
            if (e.DidPresenterChange || e.DidSizeChange)
                _engine.SetPreviewEnabled(AppWindow.Presenter is not Microsoft.UI.Windowing.OverlappedPresenter p ||
                                          p.State != Microsoft.UI.Windowing.OverlappedPresenterState.Minimized);
        };
        Closed += (_, _) =>
        {
            _previewTimer.Stop();
            _statusTimer.Stop();
            _engine.Dispose();  // stops the engine; the camera falls back to its own placeholder
        };
    }

    private void UpdatePreview()
    {
        if (!_engine.TryCopyPreview(ref _frame, ref _frameSeq, out int w, out int h)) return;
        if (_bitmap is null || _bitmap.PixelWidth != w || _bitmap.PixelHeight != h)
        {
            _bitmap = new WriteableBitmap(w, h);
            Preview.Source = _bitmap;
        }
        using (var stream = _bitmap.PixelBuffer.AsStream())
            stream.Write(_frame, 0, w * h * 4);
        _bitmap.Invalidate();
    }

    private void UpdateStatus()
    {
        using JsonDocument? doc = _engine.Status();
        if (doc is null) return;
        var s = doc.RootElement;
        string state = s.GetProperty("state").GetString() ?? "";
        Headline.Text = s.GetProperty("headline").GetString();
        Detail.Text = s.GetProperty("detail").GetString();
        Detail.Visibility = string.IsNullOrEmpty(Detail.Text) ? Visibility.Collapsed : Visibility.Visible;

        StatusDot.Fill = new SolidColorBrush(state switch
        {
            "streaming" => Colors.LimeGreen,
            "paused" or "connecting" or "waiting-for-companion" => Colors.Orange,
            "no-service" or "incompatible" or "error" => Colors.IndianRed,
            _ => Colors.Gray,
        });

        var device = s.GetProperty("device");
        string model = device.GetProperty("model").GetString() ?? "";
        string system = device.GetProperty("system").GetString() ?? "";
        DeviceText.Text = model.Length > 0 ? $"{model} · {system} · {s.GetProperty("transport").GetString()?.ToUpperInvariant()}" : "";

        var v = s.GetProperty("video");
        bool live = state is "streaming" or "paused";
        StatsText.Text = live
            ? $"{v.GetProperty("width").GetUInt32()}×{v.GetProperty("height").GetUInt32()} · " +
              $"{v.GetProperty("fps").GetDouble():0.0} fps · {v.GetProperty("mbps").GetDouble():0.0} Mbit/s · " +
              $"{(v.GetProperty("hardwareDecode").GetBoolean() ? "GPU" : "CPU")} decode {v.GetProperty("decodeMs").GetDouble():0.0} ms · " +
              $"capture→decoded {v.GetProperty("latencyMs").GetDouble():0.0} ms"
            : "";
        var vcam = s.GetProperty("vcam");
        VcamText.Text = "Cabled Camera: " + (vcam.GetProperty("inUse").GetBoolean()
            ? "in use · " + vcam.GetProperty("state").GetString()
            : "not in use");

        if (state != _state)
        {
            _state = state;
            Preview.Opacity = state == "streaming" ? 1.0 : 0.25;
            PreviewHint.Visibility = state == "streaming" ? Visibility.Collapsed : Visibility.Visible;
            PreviewHint.Text = _bitmap is null ? "No video yet" : "";
        }
    }
}
