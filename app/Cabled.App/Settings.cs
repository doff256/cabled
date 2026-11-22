using System.Text.Json;
using System.Text.Json.Serialization;

namespace Cabled.App;

/// <summary>Everything about how the picture looks that a preset captures (R9). UI units, so a
/// preset round-trips exactly through the sliders.</summary>
public sealed record Look
{
    // Framing
    public bool Framing { get; init; } = true;
    public double FramingZoom { get; init; } = 50;  // TightnessSlider 0..100
    public int Speed { get; init; } = 1;            // 0 calm, 1 normal, 2 fast
    public bool Group { get; init; }
    // Camera
    public bool FocusAuto { get; init; } = true;
    public double Focus { get; init; }               // 0..100
    public bool ExposureAuto { get; init; } = true;
    public double Bias { get; init; }                // tenths of an EV
    public double Iso { get; init; } = 500;          // slider 0..1000 (log over the phone's range)
    public double Shutter { get; init; } = 500;
    public bool WhiteBalanceAuto { get; init; } = true;
    public double Temperature { get; init; } = 5000;
    public double Tint { get; init; }
    // Image
    public double Brightness { get; init; }          // -100..100
    public double Contrast { get; init; }            // -100..100
    public double Saturation { get; init; }          // -100..100
    public double Sharpness { get; init; }           // 0..100
    public bool Mirror { get; init; }
    public int Rotation { get; init; }               // 0 auto, 1 = 0°, 2 = 90°, 3 = 180°, 4 = 270°
}

public sealed class Preset
{
    public string Name { get; set; } = "";
    public Look Look { get; set; } = new();
}

/// <summary>Per-iPhone settings (different phones have different lenses): devices\&lt;id&gt;.json.</summary>
public sealed class DeviceSettings
{
    public int Version { get; set; } = 1;
    public Look Current { get; set; } = new();
    public string? Camera { get; set; }
    public int Capture { get; set; }  // CaptureCombo index, 0 = auto
    public int Fps { get; set; }      // FpsCombo index, 0 = auto
    public List<Preset> Presets { get; set; } = new();
}

/// <summary>App-wide settings: settings.json.</summary>
public sealed class AppSettings
{
    public int Version { get; set; } = 1;
    public bool KeepRunningInTray { get; set; } = true;
    public string? LastDevice { get; set; }
}

/// <summary>%APPDATA%\Cabled storage (windows-app.md, Settings storage). Versioned; a file that
/// can't be read is renamed to .bad and defaults are used, so a bad file never stops the app.</summary>
public static class SettingsStore
{
    public const int CurrentVersion = 1;

    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
    };

    public static string Folder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Cabled");

    private static string AppPath => Path.Combine(Folder, "settings.json");

    private static string DevicePath(string deviceId)
    {
        // Device ids are UDIDs (hex and dashes); anything else is reduced to safe characters.
        var safe = new string(deviceId.Select(c => char.IsLetterOrDigit(c) || c == '-' ? c : '_').ToArray());
        return Path.Combine(Folder, "devices", safe + ".json");
    }

    public static AppSettings LoadApp() => Load<AppSettings>(AppPath) ?? new AppSettings();
    public static void SaveApp(AppSettings s) => Save(AppPath, s);

    public static DeviceSettings LoadDevice(string deviceId) => Load<DeviceSettings>(DevicePath(deviceId)) ?? new DeviceSettings();
    public static void SaveDevice(string deviceId, DeviceSettings s) => Save(DevicePath(deviceId), s);

    private static T? Load<T>(string path) where T : class
    {
        if (!File.Exists(path)) return null;
        try
        {
            var value = JsonSerializer.Deserialize<T>(File.ReadAllText(path), Options);
            // Migration hook: versions above ours come from a newer Cabled; read what we understand.
            return value;
        }
        catch (Exception)
        {
            try { File.Move(path, path + ".bad", overwrite: true); } catch (IOException) { }
            return null;
        }
    }

    private static void Save<T>(string path, T value)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            string tmp = path + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(value, Options));
            File.Move(tmp, path, overwrite: true);  // never leave a half-written file behind
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
