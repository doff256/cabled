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
    // R8: side panel
    private WriteableBitmap? _overviewBitmap;
    private byte[] _overview = new byte[320 * 180 * 4];
    private ulong _overviewSeq;
    private int _tick;
    private readonly DispatcherQueueTimer _controlsTimer, _focusMarkTimer;
    private readonly Dictionary<string, object> _pendingControls = new();
    private double[] _isoRange = { 25, 3200 }, _shutterRange = { 1.0 / 8000, 1.0 / 30 };
    private double _cropX, _cropY, _cropW = 1, _cropH = 1;
    private bool _dragging;
    private long _dragEndedAt;
    private double _grabDx, _grabDy;
    // R9: settings, presets, tray
    private const string ReleasesUrl = "https://github.com/doff256/cabled/releases";
    private readonly AppSettings _app = SettingsStore.LoadApp();
    private string? _deviceId;
    private DeviceSettings? _device;
    private readonly DispatcherQueueTimer _saveTimer;
    private bool _applying;  // true while a saved look is being put into the controls
    private TrayIcon? _tray;
    private bool _quitting;
    // R11
    private readonly LogBook _logBook = new();
    private DiagnosticsWindow? _diagnostics;

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

        // ---- R8 side panel ----
        _controlsTimer = DispatcherQueue.CreateTimer();
        _controlsTimer.Interval = TimeSpan.FromMilliseconds(60);  // slider drags: send the latest, not every step
        _controlsTimer.IsRepeating = false;
        _controlsTimer.Tick += (_, _) => FlushControls();

        TightnessSlider.ValueChanged += (_, _) => ApplyFramingOptions();
        SpeedCombo.SelectionChanged += (_, _) => ApplyFramingOptions();
        GroupCheck.Click += (_, _) => ApplyFramingOptions();

        FocusAutoSwitch.Toggled += (_, _) =>
        {
            FocusSlider.IsEnabled = !FocusAutoSwitch.IsOn;
            QueueControl("focus", FocusAutoSwitch.IsOn
                ? new { mode = "auto" }
                : new { mode = "manual", position = FocusSlider.Value / 100 });
        };
        FocusSlider.ValueChanged += (_, _) =>
        {
            if (!FocusAutoSwitch.IsOn) QueueControl("focus", new { mode = "manual", position = FocusSlider.Value / 100 });
        };

        ExposureAutoSwitch.Toggled += (_, _) =>
        {
            ExposureAutoPanel.Visibility = ExposureAutoSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
            ExposureManualPanel.Visibility = ExposureAutoSwitch.IsOn ? Visibility.Collapsed : Visibility.Visible;
            SendExposure();
        };
        BiasSlider.ValueChanged += (_, _) =>
        {
            BiasText.Text = $"Brightness {BiasSlider.Value / 10:+0.0;-0.0;0.0} EV";
            SendExposure();
        };
        IsoSlider.ValueChanged += (_, _) => SendExposure();
        ShutterSlider.ValueChanged += (_, _) => SendExposure();

        WbAutoSwitch.Toggled += (_, _) =>
        {
            WbManualPanel.Visibility = WbAutoSwitch.IsOn ? Visibility.Collapsed : Visibility.Visible;
            SendWhiteBalance();
        };
        TempSlider.ValueChanged += (_, _) => SendWhiteBalance();
        TintSlider.ValueChanged += (_, _) => SendWhiteBalance();

        CaptureCombo.SelectionChanged += (_, _) => ApplyCapture();
        FpsCombo.SelectionChanged += (_, _) => ApplyCapture();

        _focusMarkTimer = DispatcherQueue.CreateTimer();
        _focusMarkTimer.Interval = TimeSpan.FromMilliseconds(1200);
        _focusMarkTimer.IsRepeating = false;
        _focusMarkTimer.Tick += (_, _) => FocusMark.Opacity = 0;
        PreviewHost.PointerPressed += (_, e) =>
        {
            // Click -> point within the displayed picture (Uniform letterboxing) -> focus/expose there.
            if (_bitmap is null || PreviewHost.ActualWidth <= 0) return;
            var p = e.GetCurrentPoint(PreviewHost).Position;
            double scale = Math.Min(PreviewHost.ActualWidth / _bitmap.PixelWidth,
                                    PreviewHost.ActualHeight / _bitmap.PixelHeight);
            double w = _bitmap.PixelWidth * scale, h = _bitmap.PixelHeight * scale;
            double x = (p.X - (PreviewHost.ActualWidth - w) / 2) / w, y = (p.Y - (PreviewHost.ActualHeight - h) / 2) / h;
            if (x is < 0 or > 1 || y is < 0 or > 1) return;
            _engine.TapFocus(x, y);
            Canvas.SetLeft(FocusMark, p.X - FocusMark.Width / 2);
            Canvas.SetTop(FocusMark, p.Y - FocusMark.Height / 2);
            FocusMark.Opacity = 1;
            _focusMarkTimer.Stop();
            _focusMarkTimer.Start();
        };
        OverviewHost.SizeChanged += (_, _) => LayoutCropBox();

        // Box in a box: drag the box (or click anywhere) to put the shot there. The box follows the
        // pointer at once; the engine's crop glides after it and keeps following the face.
        OverviewHost.PointerPressed += (_, e) =>
        {
            if (!OverviewRect(out double left, out double top, out double w, out double h)) return;
            var p = e.GetCurrentPoint(OverviewHost).Position;
            double nx = (p.X - left) / w, ny = (p.Y - top) / h;
            bool onBox = nx >= _cropX && nx <= _cropX + _cropW && ny >= _cropY && ny <= _cropY + _cropH;
            // Grab the box where it was clicked; a click outside it centres the box there.
            _grabDx = onBox ? nx - (_cropX + _cropW / 2) : 0;
            _grabDy = onBox ? ny - (_cropY + _cropH / 2) : 0;
            _dragging = true;
            OverviewHost.CapturePointer(e.Pointer);
            DragBoxTo(nx, ny);
            e.Handled = true;
        };
        OverviewHost.PointerMoved += (_, e) =>
        {
            if (!_dragging || !OverviewRect(out double left, out double top, out double w, out double h)) return;
            var p = e.GetCurrentPoint(OverviewHost).Position;
            DragBoxTo((p.X - left) / w, (p.Y - top) / h);
        };
        void EndDrag()
        {
            if (!_dragging) return;
            _dragging = false;
            _dragEndedAt = Environment.TickCount64;
        }
        OverviewHost.PointerReleased += (_, e) => { EndDrag(); OverviewHost.ReleasePointerCapture(e.Pointer); };
        OverviewHost.PointerCaptureLost += (_, _) => EndDrag();
        OverviewHost.DoubleTapped += (_, _) => ResetNudge();
        NudgeResetButton.Click += (_, _) => ResetNudge();

        // ---- R9: pages, image, presets, app settings, tray ----
        _saveTimer = DispatcherQueue.CreateTimer();
        _saveTimer.Interval = TimeSpan.FromSeconds(1);
        _saveTimer.IsRepeating = false;
        _saveTimer.Tick += (_, _) => SaveDevice();
        WireR9();
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
        // Closing keeps Cabled running in the tray (D-20), so apps keep their camera and a phone
        // plugged in later is picked up. Quit from the tray menu ends it.
        AppWindow.Closing += (_, e) =>
        {
            if (_quitting || !_app.KeepRunningInTray || _tray is null) return;
            e.Cancel = true;
            HideToTray();
        };
        Closed += (_, _) =>
        {
            _previewTimer.Stop();
            _statusTimer.Stop();
            SaveDevice();
            _diagnostics?.Close();
            _tray?.Dispose();
            _engine.Dispose();  // stops the engine; the camera falls back to its own placeholder
        };
    }

    public void ShowFromTray()
    {
        AppWindow.Show();
        Activate();
        _engine.SetPreviewEnabled(true);
    }

    private void HideToTray()
    {
        AppWindow.Hide();
        _engine.SetPreviewEnabled(false);  // nobody is looking: skip the preview conversion
    }

    private void Quit()
    {
        _quitting = true;
        Close();
    }

    private void UpdatePreview()
    {
        // The overview thumbnail updates at ~15 fps; plenty for seeing where the crop is.
        if (++_tick % 4 == 0 && _engine.TryCopyOverview(ref _overview, ref _overviewSeq, out int ow, out int oh))
        {
            if (_overviewBitmap is null || _overviewBitmap.PixelWidth != ow || _overviewBitmap.PixelHeight != oh)
            {
                _overviewBitmap = new WriteableBitmap(ow, oh);
                OverviewImage.Source = _overviewBitmap;
                LayoutCropBox();
            }
            using (var stream = _overviewBitmap.PixelBuffer.AsStream())
                stream.Write(_overview, 0, ow * oh * 4);
            _overviewBitmap.Invalidate();
        }

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
        string log = _engine.TakeLog();
        if (log.Length > 0) _logBook.Add(log);
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
        DeviceText.Text = model.Length > 0 ? $"{DeviceNames.Friendly(model)} · iOS {system} · {s.GetProperty("transport").GetString()?.ToUpperInvariant()}" : "";
        // R13 / windows-app.md: say when heat is limiting the picture, so a softer image has a reason.
        if (s.TryGetProperty("phone", out var phoneEl) && phoneEl.TryGetProperty("thermalLimit", out var tl) &&
            tl.GetString() is { Length: > 0 } limit)
        {
            Detail.Text = (string.IsNullOrEmpty(Detail.Text) ? "" : Detail.Text + " ") +
                          (limit == "critical" ? "Your iPhone is very hot: reduced to 720p30 until it cools down."
                                               : "Your iPhone is hot: reduced to 1080p30 until it cools down.");
            Detail.Visibility = Visibility.Visible;
        }
        // R-22: free signing runs out after 7 days. Warn while there's still time to refresh.
        if (device.TryGetProperty("signingExpiry", out var exp) && DateTimeOffset.TryParse(exp.GetString(), out var expiry))
        {
            double days = (expiry - DateTimeOffset.Now).TotalDays;
            if (days < 2)
            {
                Detail.Text = (string.IsNullOrEmpty(Detail.Text) ? "" : Detail.Text + " ") +
                              (days <= 0 ? "The Cabled app on your iPhone has expired: refresh it in SideStore."
                                         : $"The Cabled app on your iPhone expires in {Math.Max(1, (int)Math.Ceiling(days * 24))} hours: refresh it in SideStore.");
                Detail.Visibility = Visibility.Visible;
            }
        }

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
        UpdateSidePanel(s);
        string deviceId = device.GetProperty("id").GetString() ?? "";
        if (deviceId.Length > 0 && deviceId != _deviceId) LoadDevice(deviceId);
        if (_tray is not null) _tray.Tip = $"Cabled: {Headline.Text}";
        UpdateOnboarding(state, Headline.Text ?? "", s.GetProperty("detail").GetString() ?? "");

        if (state != _state)
        {
            _state = state;
            Preview.Opacity = state == "streaming" ? 1.0 : 0.25;
            PreviewHint.Visibility = state == "streaming" ? Visibility.Collapsed : Visibility.Visible;
            PreviewHint.Text = _bitmap is null ? "No video yet" : "";
        }
    }

    // ---- Side panel (R8) ----

    private void ApplyFramingOptions()
    {
        double speed = SpeedCombo.SelectedIndex switch { 0 => 0.6, 2 => 1.6, _ => 1.0 };
        _engine.SetFramingOptions(TightnessSlider.Value / 100, speed, GroupCheck.IsChecked == true);
    }

    private void QueueControl(string key, object value)
    {
        _pendingControls[key] = value;
        _controlsTimer.Stop();
        _controlsTimer.Start();
    }

    private void FlushControls()
    {
        if (_pendingControls.Count == 0) return;
        _engine.SetControls(new Dictionary<string, object>(_pendingControls));
        _pendingControls.Clear();
    }

    private static double LogLerp(double[] range, double t) =>
        Math.Exp(Math.Log(range[0]) + (Math.Log(range[1]) - Math.Log(range[0])) * Math.Clamp(t, 0, 1));

    private void SendExposure()
    {
        if (ExposureAutoSwitch.IsOn)
        {
            QueueControl("exposure", new { mode = "auto", bias = BiasSlider.Value / 10 });
            return;
        }
        double iso = LogLerp(_isoRange, IsoSlider.Value / 1000);
        double shutter = LogLerp(_shutterRange, ShutterSlider.Value / 1000);
        IsoText.Text = $"ISO {iso:0}";
        ShutterText.Text = shutter >= 0.1 ? $"Shutter {shutter:0.0} s" : $"Shutter 1/{1 / shutter:0} s";
        QueueControl("exposure", new { mode = "manual", iso, shutter });
    }

    private void SendWhiteBalance()
    {
        TempText.Text = $"{TempSlider.Value:0} K";
        TintText.Text = $"Tint {TintSlider.Value:+0;-0;0}";
        QueueControl("whiteBalance", WbAutoSwitch.IsOn
            ? new { mode = "auto" }
            : new { mode = "manual", temperature = TempSlider.Value, tint = TintSlider.Value });
    }

    private void ApplyCapture()
    {
        (int w, int h) = CaptureCombo.SelectedIndex switch { 1 => (1280, 720), 2 => (1920, 1080), 3 => (3840, 2160), _ => (0, 0) };
        double fps = FpsCombo.SelectedIndex switch { 1 => 30, 2 => 60, 3 => 120, _ => 0 };
        _engine.SetCapture(w, h, fps);
    }

    private void UpdateSidePanel(JsonElement s)
    {
        var framing = s.GetProperty("framing");
        // While dragging (and just after, while the crop glides to the drop point) the box shows the
        // pointer, not the engine, so it doesn't snap back.
        if (!_dragging && Environment.TickCount64 - _dragEndedAt > 700 && framing.TryGetProperty("crop", out var crop))
        {
            _cropX = crop.GetProperty("x").GetDouble();
            _cropY = crop.GetProperty("y").GetDouble();
            _cropW = crop.GetProperty("w").GetDouble();
            _cropH = crop.GetProperty("h").GetDouble();
            LayoutCropBox();
        }
        if (framing.TryGetProperty("zoom", out var zoomEl) && framing.TryGetProperty("reachableZoom", out var reachEl))
        {
            double wanted = zoomEl.GetDouble(), reachable = reachEl.GetDouble();
            bool limited = FramingSwitch.IsOn && wanted > reachable + 0.05;
            FramingZoomText.Text = $"{Math.Min(wanted, reachable):0.0}×";
            FramingLimitText.Visibility = limited ? Visibility.Visible : Visibility.Collapsed;
            if (limited)
                FramingLimitText.Text = $"Limited to {reachable:0.0}× at this capture size so it stays sharp. " +
                                        "Set Capture to Auto or 4K to get closer.";
        }

        // Live camera readouts and the ranges the manual sliders map onto.
        var controls = s.GetProperty("controls");
        if (controls.TryGetProperty("exposure", out var exposure))
        {
            if (exposure.TryGetProperty("isoRange", out var ir) && ir.GetArrayLength() == 2)
                _isoRange = new[] { Math.Max(1, ir[0].GetDouble()), Math.Max(2, ir[1].GetDouble()) };
            if (exposure.TryGetProperty("shutterRange", out var sr) && sr.GetArrayLength() == 2)
                _shutterRange = new[] { Math.Max(1e-6, sr[0].GetDouble()), Math.Max(2e-6, sr[1].GetDouble()) };
            double shutter = exposure.GetProperty("shutter").GetDouble();
            ExposureLive.Text = $"Now: ISO {exposure.GetProperty("iso").GetDouble():0} · " +
                                (shutter >= 0.1 ? $"{shutter:0.0} s" : $"1/{1 / Math.Max(shutter, 1e-6):0} s");
        }
        if (controls.TryGetProperty("focus", out var focus))
        {
            FocusAutoSwitch.IsEnabled = !focus.TryGetProperty("manualSupported", out var ms) || ms.GetBoolean() ||
                                        !FocusAutoSwitch.IsOn;
            if (FocusAutoSwitch.IsOn && focus.TryGetProperty("position", out var pos))
                FocusSlider.Value = pos.GetDouble() * 100;  // show where autofocus is
        }
    }

    // Where the thumbnail is drawn inside OverviewHost (Uniform letterboxing).
    private bool OverviewRect(out double left, out double top, out double w, out double h)
    {
        left = top = w = h = 0;
        if (_overviewBitmap is null || OverviewHost.ActualWidth <= 0 || OverviewHost.ActualHeight <= 0) return false;
        double scale = Math.Min(OverviewHost.ActualWidth / _overviewBitmap.PixelWidth,
                                OverviewHost.ActualHeight / _overviewBitmap.PixelHeight);
        w = _overviewBitmap.PixelWidth * scale;
        h = _overviewBitmap.PixelHeight * scale;
        left = (OverviewHost.ActualWidth - w) / 2;
        top = (OverviewHost.ActualHeight - h) / 2;
        return true;
    }

    // Pointer at (nx, ny) of the whole view: box centre there (minus where it was grabbed), kept inside.
    private void DragBoxTo(double nx, double ny)
    {
        double cx = Math.Clamp(nx - _grabDx, _cropW / 2, 1 - _cropW / 2);
        double cy = Math.Clamp(ny - _grabDy, _cropH / 2, 1 - _cropH / 2);
        _cropX = cx - _cropW / 2;
        _cropY = cy - _cropH / 2;
        LayoutCropBox();
        _engine.MoveFraming(cx, cy);
        NudgeResetButton.Visibility = Visibility.Visible;
    }

    private void ResetNudge()
    {
        _engine.ResetFramingNudge();
        NudgeResetButton.Visibility = Visibility.Collapsed;
    }

    // Box in a box: outline the part of the camera view being sent, over the overview thumbnail.
    private void LayoutCropBox()
    {
        if (!OverviewRect(out double left, out double top, out double w, out double h)) return;
        Canvas.SetLeft(CropBox, left + _cropX * w);
        Canvas.SetTop(CropBox, top + _cropY * h);
        CropBox.Width = Math.Max(2, _cropW * w);
        CropBox.Height = Math.Max(2, _cropH * h);
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
                    RememberCamera(id);
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
