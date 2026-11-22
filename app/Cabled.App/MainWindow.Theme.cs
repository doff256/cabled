using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.UI;

namespace Cabled.App;

/// <summary>
/// Mangrove Glass plumbing for the main window (DESIGN.md "Window"): the merged title bar, token
/// lookups for code-set colours, and the status pill whose dot breathes while video is live.
/// </summary>
public sealed partial class MainWindow
{
    private Storyboard? _breathe;
    private string _pillKey = "";
    private static readonly bool AnimationsOn = new Windows.UI.ViewManagement.UISettings().AnimationsEnabled;

    internal static Brush TokenBrush(string token) => (Brush)Application.Current.Resources[token + "Brush"];
    private static Color TokenColor(string token) => (Color)Application.Current.Resources[token + "Color"];

    private void SetupChrome()
    {
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        ColourCaptionButtons();
        Root.ActualThemeChanged += (_, _) => ColourCaptionButtons();
    }

    // The window buttons are drawn by Windows, so they get the token colours by hand.
    private void ColourCaptionButtons()
    {
        var bar = AppWindow.TitleBar;
        bar.ButtonBackgroundColor = Colors.Transparent;
        bar.ButtonInactiveBackgroundColor = Colors.Transparent;
        bar.ButtonForegroundColor = TokenColor("Ink");
        bar.ButtonInactiveForegroundColor = TokenColor("InkFaint");
        bar.ButtonHoverBackgroundColor = TokenColor("Surface300");
        bar.ButtonHoverForegroundColor = TokenColor("Ink");
        bar.ButtonPressedBackgroundColor = TokenColor("Surface000");
        bar.ButtonPressedForegroundColor = TokenColor("Ink");
    }

    /// <summary>
    /// The pill says the state in one word with a dot: tide = ready, mangrove-glow = live
    /// (breathing), clay = needs you, rose = error, ink-faint = nothing connected.
    /// </summary>
    private void SetStatusPill(string state)
    {
        (string word, string token) = state switch
        {
            "streaming" => ("LIVE", "MangroveGlow"),
            "idle" => ("CONNECTED", "Tide"),
            "connecting" => ("CONNECTING", "InkMuted"),
            "paused" => ("PAUSED", "Clay"),
            "waiting-for-companion" => ("OPEN THE APP", "Clay"),
            "no-service" => ("SETUP NEEDED", "Clay"),
            "no-device" => ("NO IPHONE", "InkFaint"),
            "incompatible" or "error" => ("ERROR", "Rose"),
            _ => ("STARTING", "InkFaint"),
        };
        if (word + token == _pillKey) return;
        _pillKey = word + token;
        StatusWord.Text = word;
        StatusWord.Foreground = TokenBrush(token);  // mg-pill: the word takes the status colour too
        StatusDot.Fill = TokenBrush(token);
        Breathe(state == "streaming");
    }


    // Live things breathe slowly (dur-breathe 4800 ms per cycle); nothing loops under reduced motion.
    private void Breathe(bool on)
    {
        if (_breathe is null && on && AnimationsOn)
        {
            var fade = new DoubleAnimation
            {
                From = 1.0, To = 0.35, Duration = TimeSpan.FromMilliseconds(2400), AutoReverse = true,
                RepeatBehavior = RepeatBehavior.Forever,
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut },
            };
            Storyboard.SetTarget(fade, StatusDot);
            Storyboard.SetTargetProperty(fade, "Opacity");
            _breathe = new Storyboard { Children = { fade } };
        }
        if (on && AnimationsOn) _breathe?.Begin();
        else { _breathe?.Stop(); StatusDot.Opacity = 1; }
    }

    // Precision that changes a decision (DESIGN.md "Numbers"): whole fps and ms, one decimal under 10.
    private static string Num(double v) => v < 10 ? v.ToString("0.0") : v.ToString("0");
}
