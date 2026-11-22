using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Cabled.App;

// R11: first-run setup. The steps follow the engine's live state, so the guide is never out of
// date: Apple service → iPhone plugged in (and trusted) → companion open → video ready.
public sealed partial class MainWindow
{
    private const string CompanionGuideUrl = "https://github.com/doff256/cabled#install-the-iphone-app";
    // Apple Devices in the Microsoft Store; without a Store (LTSC), iTunes from apple.com has the service too.
    private const string AppleDevicesStoreUrl = "ms-windows-store://pdp/?ProductId=9NP83LWLPZ9K";
    private const string ITunesUrl = "https://www.apple.com/itunes/";

    private void WireOnboarding()
    {
        AppleDevicesButton.Click += async (_, _) => await Windows.System.Launcher.LaunchUriAsync(new Uri(AppleDevicesStoreUrl));
        ITunesLink.Click += async (_, _) => await Windows.System.Launcher.LaunchUriAsync(new Uri(ITunesUrl));
        CompanionGuideLink.Click += async (_, _) => await Windows.System.Launcher.LaunchUriAsync(new Uri(CompanionGuideUrl));
        OnboardingFinishButton.Click += (_, _) => FinishOnboarding(applyAutostart: true);
        OnboardingSkipButton.Click += (_, _) => FinishOnboarding(applyAutostart: false);
        if (!_app.OnboardingDone) ShowOnboarding();
    }

    private void ShowOnboarding()
    {
        OnboardingAutostartCheck.IsChecked = !_app.OnboardingDone || Autostart.PointsHere;  // D-20: ticked by default
        Onboarding.Visibility = Visibility.Visible;
    }

    private void FinishOnboarding(bool applyAutostart)
    {
        if (applyAutostart)
        {
            try
            {
                Autostart.Set(OnboardingAutostartCheck.IsChecked == true);
                AutostartSwitch.IsOn = Autostart.PointsHere;
            }
            catch (Exception) { /* the App page shows the real state */ }
        }
        _app.OnboardingDone = true;
        SettingsStore.SaveApp(_app);
        Onboarding.Visibility = Visibility.Collapsed;
    }

    /// <summary>Called with each status update while the guide is open.</summary>
    private void UpdateOnboarding(string state, string headline, string detail)
    {
        if (Onboarding.Visibility != Visibility.Visible) return;
        // How far along: 0 = no Apple service, 1 = no iPhone, 2 = iPhone there but no companion yet, 3 = ready.
        int reached = state switch
        {
            "no-service" => 0,
            "no-device" => 1,
            "streaming" or "idle" or "paused" => 3,
            _ => 2,
        };
        string now = string.IsNullOrEmpty(detail) ? headline : $"{headline}. {detail}";

        SetStep(StepIcon1, StepDetail1, reached > 0, reached == 0,
                reached > 0 ? "Found." : "Windows needs Apple's iPhone service to talk to the phone. Get Apple Devices from the Microsoft Store (free), then come back.");
        StepActions1.Visibility = reached == 0 ? Visibility.Visible : Visibility.Collapsed;
        SetStep(StepIcon2, StepDetail2, reached > 1, reached == 1,
                reached > 1 ? "Connected." : "Use a USB cable. If the iPhone asks whether to trust this computer, tap Trust and enter your passcode.");
        SetStep(StepIcon3, StepDetail3, reached > 2, reached == 2,
                reached > 2 ? "Connected to the Cabled app." : reached == 2 ? now : "Install it with SideStore, then open it.");
        SetStep(StepIcon4, StepDetail4, reached > 2, false,
                reached > 2 ? "Done. Apps now list \"Cabled Camera\": pick it in Zoom, Teams, OBS or the Camera app." : "");
        OnboardingFinishButton.Content = reached > 2 ? "Finish" : "Finish anyway";
    }

    private static void SetStep(TextBlock icon, TextBlock detail, bool done, bool current, string text)
    {
        // Segoe Fluent Icons: CompletedSolid, a ring for "now", an empty circle for later.
        icon.Text = done ? "" : current ? "" : "";
        icon.Foreground = done
            ? (Brush)Application.Current.Resources["SystemFillColorSuccessBrush"]
            : current ? (Brush)Application.Current.Resources["AccentTextFillColorPrimaryBrush"]
            : (Brush)Application.Current.Resources["TextFillColorTertiaryBrush"];
        detail.Text = text;
        detail.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }
}
