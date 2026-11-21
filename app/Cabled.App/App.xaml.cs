using Microsoft.UI.Xaml;

namespace Cabled.App;

public partial class App : Application
{
    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // Dev option: `Cabled.exe --tcp 127.0.0.1:47800` talks to cabled-fakephone instead of USB.
        string[] argv = Environment.GetCommandLineArgs();
        string? tcpTarget = null;
        for (int i = 1; i < argv.Length - 1; i++)
            if (argv[i] == "--tcp") tcpTarget = argv[i + 1];
        // `--debug-faces` draws the phone's detected faces into the preview (R7 checks).
        bool debugFaces = argv.Contains("--debug-faces");

        _window = new MainWindow(tcpTarget, debugFaces);
        _window.Activate();
    }
}
