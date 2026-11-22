using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace Cabled.App;

public partial class App : Application
{
    // One Cabled per user session: a second launch (Start menu while it's in the tray) just shows
    // the running one.
    private const string InstanceMutexName = @"Local\Cabled.App.Instance";
    private const string ShowEventName = @"Local\Cabled.App.Show";

    private MainWindow? _window;
    private Mutex? _instanceMutex;
    private EventWaitHandle? _showEvent;

    public App()
    {
        InitializeComponent();
        // A XAML or startup error otherwise ends the process with no trace; keep the reason.
        UnhandledException += (_, e) => WriteCrash(e.Exception);
    }

    internal static void WriteCrash(Exception ex)
    {
        try
        {
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Cabled");
            Directory.CreateDirectory(dir);
            File.AppendAllText(Path.Combine(dir, "crash.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {ex}\r\n\r\n");
        }
        catch { /* nothing more we can do */ }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _instanceMutex = new Mutex(initiallyOwned: true, InstanceMutexName, out bool firstInstance);
        _showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEventName);
        if (!firstInstance)
        {
            _showEvent.Set();  // wake the running copy
            Exit();
            return;
        }

        // Dev option: `Cabled.exe --tcp 127.0.0.1:47800` talks to cabled-fakephone instead of USB.
        string[] argv = Environment.GetCommandLineArgs();
        string? tcpTarget = null;
        for (int i = 1; i < argv.Length - 1; i++)
            if (argv[i] == "--tcp") tcpTarget = argv[i + 1];
        // `--debug-faces` draws the phone's detected faces into the preview (R7 checks).
        bool debugFaces = argv.Contains("--debug-faces");
        // `--tray`: started at login (D-20); stay in the notification area until opened.
        bool startInTray = argv.Contains("--tray");

        try { _window = new MainWindow(tcpTarget, debugFaces); }
        catch (Exception ex) { WriteCrash(ex); throw; }
        if (startInTray) _window.StartInTray();
        else _window.Activate();

        var dispatcher = DispatcherQueue.GetForCurrentThread();
        var showEvent = _showEvent;
        var thread = new Thread(() =>
        {
            while (true)
            {
                showEvent.WaitOne();
                dispatcher.TryEnqueue(() => _window?.ShowFromTray());
            }
        }) { IsBackground = true, Name = "cabled.show" };
        thread.Start();
    }
}
