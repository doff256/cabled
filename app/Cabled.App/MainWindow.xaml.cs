using System.Runtime.InteropServices.WindowsRuntime;
using System.Text.Json;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
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
    // Lens + zoom (R6)
    private string _camerasJson = "";
    private string _currentCamera = "";
    private double _zoomMin = 1, _zoomMax = 1;
    private bool _settingSlider;
    private DateTime _userZoomedAt = DateTime.MinValue;
    private DateTime _lastZoomSent = DateTime.MinValue;
    private int _engineFaces = -1;

    public MainWindow(string? tcpTarget, bool debugFaces = false)
    {
        InitializeComponent();
        AppWindow.Resize(new SizeInt32(1100, 760));
        Title = $"Cabled {NativeEngine.Version}";

        _engine = new NativeEngine(new
        {
            tcpTarget = tcpTarget ?? "",
            previewMaxWidth = 960,
            debugFaces,
        });
        FramingSwitch.Toggled += (_, _) => _engine.SetFraming(FramingSwitch.IsOn);
        _engine.Start();

        _previewTimer = DispatcherQueue.CreateTimer();
        _previewTimer.Interval = TimeSpan.FromMilliseconds(16);
        _previewTimer.Tick += (_, _) => UpdatePreview();
        _previewTimer.Start();

        _statusTimer = DispatcherQueue.CreateTimer();
        _statusTimer.Interval = TimeSpan.FromMilliseconds(250);
        _statusTimer.Tick += (_, _) => UpdateStatus();
        _statusTimer.Start();

        ZoomSlider.ValueChanged += (_, e) =>
        {
            if (_settingSlider) return;
            _userZoomedAt = DateTime.UtcNow;
            double zoom = SliderToZoom(e.NewValue);
            ZoomText.Text = $"{zoom:0.0}×";
            if (DateTime.UtcNow - _lastZoomSent < TimeSpan.FromMilliseconds(40)) return;  // ~25 updates/s
            _lastZoomSent = DateTime.UtcNow;
            _engine.SetZoom(zoom, smooth: false);
        };
        ZoomSlider.PointerCaptureLost += (_, _) => _engine.SetZoom(SliderToZoom(ZoomSlider.Value), smooth: false);

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
            "streaming" or "idle" => Colors.LimeGreen,
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
              $"capture→decoded {v.GetProperty("latencyMs").GetDouble():0.0} ms · " +
              $"asked phone for {s.GetProperty("link").GetProperty("config").GetString()} · " +
              $"link {s.GetProperty("link").GetProperty("mbps").GetDouble():0} Mbit/s"
            : state == "idle" ? $"Video off until an app opens Cabled Camera · link {s.GetProperty("link").GetProperty("mbps").GetDouble():0} Mbit/s"
            : "";
        var vcam = s.GetProperty("vcam");
        VcamText.Text = "Cabled Camera: " + (vcam.GetProperty("inUse").GetBoolean()
            ? "in use · " + vcam.GetProperty("state").GetString()
            : "not in use");

        _engineFaces = s.GetProperty("framing").GetProperty("faces").GetInt32();
        UpdateCameraControls(s.GetProperty("camera"), live || state == "idle");

        if (state != _state)
        {
            _state = state;
            Preview.Opacity = state == "streaming" ? 1.0 : 0.25;
            PreviewHint.Visibility = state == "streaming" ? Visibility.Collapsed : Visibility.Visible;
            PreviewHint.Text = _bitmap is null ? "No video yet" : "";
        }
    }

    // ---- Lens + zoom (R6) ----

    // The slider is logarithmic so 0.5× to 15× feels even: each step multiplies the zoom.
    private double SliderToZoom(double v) =>
        Math.Exp(Math.Log(_zoomMin) + (Math.Log(_zoomMax) - Math.Log(_zoomMin)) * v / ZoomSlider.Maximum);

    private double ZoomToSlider(double zoom) =>
        _zoomMax <= _zoomMin ? 0 :
        (Math.Log(Math.Clamp(zoom, _zoomMin, _zoomMax)) - Math.Log(_zoomMin)) /
        (Math.Log(_zoomMax) - Math.Log(_zoomMin)) * ZoomSlider.Maximum;

    private void UpdateCameraControls(JsonElement camera, bool connected)
    {
        var list = camera.GetProperty("list");
        CameraControls.Visibility = connected && list.GetArrayLength() > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (CameraControls.Visibility != Visibility.Visible) return;

        string current = camera.GetProperty("current").GetString() ?? "";
        string listJson = list.GetRawText();
        if (listJson != _camerasJson || current != _currentCamera)
        {
            _camerasJson = listJson;
            _currentCamera = current;
            LensPanel.Children.Clear();
            foreach (var c in list.EnumerateArray())
            {
                string id = c.GetProperty("id").GetString() ?? "";
                var button = new Microsoft.UI.Xaml.Controls.Primitives.ToggleButton
                {
                    Content = c.GetProperty("label").GetString(),
                    IsChecked = id == current,
                    Tag = id,
                };
                Microsoft.UI.Xaml.Controls.ToolTipService.SetToolTip(button, c.GetProperty("name").GetString());
                button.Click += (_, _) =>
                {
                    _engine.SetCamera(id);
                    foreach (var child in LensPanel.Children)
                        if (child is Microsoft.UI.Xaml.Controls.Primitives.ToggleButton t) t.IsChecked = (string)t.Tag == id;
                };
                LensPanel.Children.Add(button);

                if (id != current) continue;
                _zoomMin = Math.Max(0.1, c.GetProperty("minZoom").GetDouble());
                _zoomMax = Math.Max(_zoomMin, c.GetProperty("maxZoom").GetDouble());
                ZoomRow.Visibility = _zoomMax > _zoomMin * 1.01 ? Visibility.Visible : Visibility.Collapsed;
                PresetPanel.Children.Clear();
                foreach (var p in c.GetProperty("presets").EnumerateArray())
                {
                    double preset = p.GetDouble();
                    var b = new Microsoft.UI.Xaml.Controls.Button { Content = $"{preset:0.#}×", Padding = new Thickness(8, 2, 8, 2) };
                    b.Click += (_, _) =>
                    {
                        _userZoomedAt = DateTime.UtcNow;
                        _engine.SetZoom(preset, smooth: true);
                    };
                    PresetPanel.Children.Add(b);
                }
            }
        }

        // Follow the phone's zoom (ramps, reconnects) unless the user is dragging right now.
        double zoom = camera.GetProperty("zoom").GetDouble();
        if (DateTime.UtcNow - _userZoomedAt > TimeSpan.FromSeconds(1))
        {
            _settingSlider = true;
            ZoomSlider.Value = ZoomToSlider(zoom);
            _settingSlider = false;
            ZoomText.Text = $"{zoom:0.0}×";
        }
        if (_engineFaces >= 0) ToolTipService.SetToolTip(FramingSwitch, $"Faces in view: {_engineFaces}");
        string lens = camera.GetProperty("lens").GetString() ?? "";
        LensText.Text = lens switch
        {
            "ultraWide" => "Ultra wide lens",
            "telephoto" => "Telephoto lens",
            "front" => "Front camera",
            "" => "",
            _ => "Main lens",
        };
    }
}
