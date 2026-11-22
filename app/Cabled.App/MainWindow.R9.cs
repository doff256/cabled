using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Cabled.App;

// R9: side-panel pages, image adjustments, presets, per-iPhone settings, tray and app settings.
public sealed partial class MainWindow
{
    /// <summary>Started at login with --tray: no window, so no preview work either.</summary>
    public void StartInTray() => _engine.SetPreviewEnabled(false);

    private void WireR9()
    {
        // Window + tray icon
        string iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "Cabled.ico");
        if (File.Exists(iconPath)) AppWindow.SetIcon(iconPath);
        _tray = new TrayIcon(WinRT.Interop.WindowNative.GetWindowHandle(this), "Cabled", iconPath);
        _tray.OpenRequested += ShowFromTray;
        _tray.QuitRequested += Quit;

        // Pages
        PageBar.SelectionChanged += (_, _) => ShowPage(PageBar.SelectedItem?.Tag as string);

        // Image
        foreach (var slider in new[] { BrightnessSlider, ContrastSlider, SaturationSlider, SharpnessSlider })
            slider.ValueChanged += (_, _) => ApplyImage();
        MirrorSwitch.Toggled += (_, _) => ApplyImage();
        RotationCombo.SelectionChanged += (_, _) => QueueControl("rotation", RotationCombo.SelectedIndex switch
        {
            1 => 0,
            2 => 90,
            3 => 180,
            4 => 270,
            _ => (object)"auto",
        });
        NeutralButton.Click += (_, _) =>
        {
            BrightnessSlider.Value = ContrastSlider.Value = SaturationSlider.Value = SharpnessSlider.Value = 0;
            MirrorSwitch.IsOn = false;
            RotationCombo.SelectedIndex = 0;
        };

        // Remember changes per iPhone (saved a second after the last change).
        foreach (var s in new[] { TightnessSlider, BiasSlider, IsoSlider, ShutterSlider, TempSlider, TintSlider,
                                  BrightnessSlider, ContrastSlider, SaturationSlider, SharpnessSlider })
            s.ValueChanged += (_, _) => ScheduleSave();
        FocusSlider.ValueChanged += (_, _) =>
        {
            if (!FocusAutoSwitch.IsOn) ScheduleSave();  // in auto it just shows where autofocus is
        };
        foreach (var t in new[] { FramingSwitch, FocusAutoSwitch, ExposureAutoSwitch, WbAutoSwitch, MirrorSwitch })
            t.Toggled += (_, _) => ScheduleSave();
        foreach (var c in new[] { SpeedCombo, CaptureCombo, FpsCombo, RotationCombo })
            c.SelectionChanged += (_, _) => ScheduleSave();
        GroupCheck.Click += (_, _) => ScheduleSave();

        // Presets
        SavePresetButton.Click += (_, _) => SavePreset();
        ApplyPresetButton.Click += (_, _) => ApplySelectedPreset();
        PresetList.DoubleTapped += (_, _) => ApplySelectedPreset();
        RenamePresetButton.Click += (_, _) => RenameSelectedPreset();
        DeletePresetButton.Click += (_, _) => DeleteSelectedPreset();
        PresetList.SelectionChanged += (_, _) =>
        {
            if (PresetList.SelectedItem is string name) PresetNameBox.Text = name;
            UpdatePresetButtons();
        };
        RefreshPresets();

        // App page (values set before subscribing, so loading them changes nothing)
        AutostartSwitch.IsOn = Autostart.PointsHere;
        AutostartSwitch.Toggled += (_, _) =>
        {
            try { Autostart.Set(AutostartSwitch.IsOn); }
            catch (Exception) { AutostartSwitch.IsOn = Autostart.PointsHere; }
        };
        TraySwitch.IsOn = _app.KeepRunningInTray;
        TraySwitch.Toggled += (_, _) =>
        {
            _app.KeepRunningInTray = TraySwitch.IsOn;
            SettingsStore.SaveApp(_app);
        };
        AboutText.Text = $"Cabled {typeof(App).Assembly.GetName().Version?.ToString(3)} · engine {NativeEngine.Version}";
        ReleasesLink.Click += async (_, _) => await Windows.System.Launcher.LaunchUriAsync(new Uri(ReleasesUrl));
        SettingsFolderLink.Click += (_, _) =>
        {
            Directory.CreateDirectory(SettingsStore.Folder);
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{SettingsStore.Folder}\"") { UseShellExecute = true });
        };

        // R11: diagnostics and the setup guide
        DiagnosticsButton.Click += (_, _) =>
        {
            if (_diagnostics is null)
            {
                _diagnostics = new DiagnosticsWindow(_engine.StatusJson, _logBook);
                _diagnostics.Closed += (_, _) => _diagnostics = null;
            }
            _diagnostics.Activate();
        };
        SetupAgainLink.Click += (_, _) => ShowOnboarding();
        WireOnboarding();
    }

    private void ShowPage(string? name)
    {
        foreach (var page in new FrameworkElement[] { FramingPage, CameraPage, ImagePage, PresetsPage, AppPage })
            page.Visibility = page.Name == name ? Visibility.Visible : Visibility.Collapsed;
    }

    private void ApplyImage()
    {
        BrightnessText.Text = $"{BrightnessSlider.Value:+0;-0;0}";
        ContrastText.Text = $"{ContrastSlider.Value:+0;-0;0}";
        SaturationText.Text = $"{SaturationSlider.Value:+0;-0;0}";
        SharpnessText.Text = $"{SharpnessSlider.Value:0}";
        _engine.SetImage(BrightnessSlider.Value / 100, 1 + ContrastSlider.Value / 100, 1 + SaturationSlider.Value / 100,
                         SharpnessSlider.Value / 100, MirrorSwitch.IsOn);
    }

    // ---- Looks: what the controls show <-> what gets saved ----

    private Look CurrentLook() => new()
    {
        Framing = FramingSwitch.IsOn,
        FramingZoom = TightnessSlider.Value,
        Speed = SpeedCombo.SelectedIndex,
        Group = GroupCheck.IsChecked == true,
        FocusAuto = FocusAutoSwitch.IsOn,
        Focus = FocusSlider.Value,
        ExposureAuto = ExposureAutoSwitch.IsOn,
        Bias = BiasSlider.Value,
        Iso = IsoSlider.Value,
        Shutter = ShutterSlider.Value,
        WhiteBalanceAuto = WbAutoSwitch.IsOn,
        Temperature = TempSlider.Value,
        Tint = TintSlider.Value,
        Brightness = BrightnessSlider.Value,
        Contrast = ContrastSlider.Value,
        Saturation = SaturationSlider.Value,
        Sharpness = SharpnessSlider.Value,
        Mirror = MirrorSwitch.IsOn,
        Rotation = RotationCombo.SelectedIndex,
    };

    /// <summary>Puts a look into the controls; their handlers send it to the engine. Manual values
    /// go in before the auto switches, so turning a switch off sends the saved value.</summary>
    private void ApplyLook(Look l)
    {
        _applying = true;
        try
        {
            FramingSwitch.IsOn = l.Framing;
            TightnessSlider.Value = l.FramingZoom;
            SpeedCombo.SelectedIndex = Math.Clamp(l.Speed, 0, 2);
            GroupCheck.IsChecked = l.Group;
            ApplyFramingOptions();  // a CheckBox set from code raises no Click

            FocusSlider.Value = l.Focus;
            FocusAutoSwitch.IsOn = l.FocusAuto;
            BiasSlider.Value = l.Bias;
            IsoSlider.Value = l.Iso;
            ShutterSlider.Value = l.Shutter;
            ExposureAutoSwitch.IsOn = l.ExposureAuto;
            TempSlider.Value = l.Temperature;
            TintSlider.Value = l.Tint;
            WbAutoSwitch.IsOn = l.WhiteBalanceAuto;

            BrightnessSlider.Value = l.Brightness;
            ContrastSlider.Value = l.Contrast;
            SaturationSlider.Value = l.Saturation;
            SharpnessSlider.Value = l.Sharpness;
            MirrorSwitch.IsOn = l.Mirror;
            RotationCombo.SelectedIndex = Math.Clamp(l.Rotation, 0, 4);
        }
        finally
        {
            _applying = false;
        }
        ApplyImage();
    }

    // ---- Per-iPhone settings ----

    private void LoadDevice(string id)
    {
        SaveDevice();  // the previous phone's changes first
        _deviceId = id;
        _device = SettingsStore.LoadDevice(id);
        _app.LastDevice = id;
        SettingsStore.SaveApp(_app);
        ApplyLook(_device.Current);
        CaptureCombo.SelectedIndex = Math.Clamp(_device.Capture, 0, 3);
        FpsCombo.SelectedIndex = Math.Clamp(_device.Fps, 0, 3);
        if (!string.IsNullOrEmpty(_device.Camera)) _engine.SetCamera(_device.Camera);
        RefreshPresets();
    }

    private void ScheduleSave()
    {
        if (_applying || _device is null) return;
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    private void SaveDevice()
    {
        _saveTimer.Stop();
        if (_deviceId is null || _device is null) return;
        _device.Current = CurrentLook();
        _device.Capture = CaptureCombo.SelectedIndex;
        _device.Fps = FpsCombo.SelectedIndex;
        _device.Version = SettingsStore.CurrentVersion;
        SettingsStore.SaveDevice(_deviceId, _device);
    }

    /// <summary>A lens button was clicked: that's this phone's camera from now on.</summary>
    private void RememberCamera(string id)
    {
        if (_device is null) return;
        _device.Camera = id;
        ScheduleSave();
    }

    // ---- Presets ----

    private void RefreshPresets(string? select = null)
    {
        PresetList.Items.Clear();
        SavePresetButton.IsEnabled = _device is not null;
        if (_device is null)
        {
            PresetEmptyText.Text = "Connect your iPhone to see its presets.";
            PresetEmptyText.Visibility = Visibility.Visible;
            UpdatePresetButtons();
            return;
        }
        foreach (var p in _device.Presets) PresetList.Items.Add(p.Name);
        PresetEmptyText.Text = "No presets yet.";
        PresetEmptyText.Visibility = _device.Presets.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (select is not null) PresetList.SelectedItem = select;
        UpdatePresetButtons();
    }

    private void UpdatePresetButtons()
    {
        bool selected = PresetList.SelectedItem is string;
        ApplyPresetButton.IsEnabled = RenamePresetButton.IsEnabled = DeletePresetButton.IsEnabled = selected;
    }

    private Preset? SelectedPreset() =>
        PresetList.SelectedItem is string name ? _device?.Presets.FirstOrDefault(p => p.Name == name) : null;

    private void SavePreset()
    {
        if (_device is null) return;
        string name = PresetNameBox.Text.Trim();
        if (name.Length == 0) name = $"Preset {_device.Presets.Count + 1}";
        var existing = _device.Presets.FirstOrDefault(p => p.Name == name);
        if (existing is not null) existing.Look = CurrentLook();  // same name: update it
        else _device.Presets.Add(new Preset { Name = name, Look = CurrentLook() });
        SaveDevice();
        RefreshPresets(name);
    }

    private void ApplySelectedPreset()
    {
        if (SelectedPreset() is not { } preset) return;
        ApplyLook(preset.Look);
        SaveDevice();
    }

    private void RenameSelectedPreset()
    {
        if (_device is null || SelectedPreset() is not { } preset) return;
        string name = PresetNameBox.Text.Trim();
        if (name.Length == 0 || name == preset.Name || _device.Presets.Any(p => p.Name == name)) return;
        preset.Name = name;
        SaveDevice();
        RefreshPresets(name);
    }

    private void DeleteSelectedPreset()
    {
        if (_device is null || SelectedPreset() is not { } preset) return;
        _device.Presets.Remove(preset);
        SaveDevice();
        PresetNameBox.Text = "";
        RefreshPresets();
    }
}
