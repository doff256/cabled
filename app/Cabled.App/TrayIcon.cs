using System.Runtime.InteropServices;

namespace Cabled.App;

/// <summary>
/// Notification-area icon (D-20) with plain Win32 (Shell_NotifyIcon), no extra dependency. It
/// borrows the main window's HWND for its messages through a window subclass, so it keeps working
/// while the window is hidden. Left click opens the window; right click shows Open / Quit.
/// </summary>
internal sealed unsafe partial class TrayIcon : IDisposable
{
    private const uint WmTray = 0x8000 + 17;  // WM_APP + 17
    private const uint NimAdd = 0, NimModify = 1, NimDelete = 2;
    private const uint NifMessage = 1, NifIcon = 2, NifTip = 4;
    private const uint WmLButtonUp = 0x0202, WmRButtonUp = 0x0205;
    private const uint MfString = 0, MfSeparator = 0x800, TpmReturnCmd = 0x100, TpmRightButton = 2;
    private const uint ImageIcon = 1, LrLoadFromFile = 0x10;
    private const nuint SubclassId = 0xCAB1ED;

    private readonly nint _hwnd;
    private readonly nint _icon;
    private readonly uint _taskbarCreated;
    private readonly SubclassProc _proc;  // kept alive: Windows holds a pointer to it
    private string _tip;
    private bool _added;

    public event Action? OpenRequested;
    public event Action? QuitRequested;

    private delegate nint SubclassProc(nint hwnd, uint msg, nint wParam, nint lParam, nuint id, nuint refData);

    public TrayIcon(nint hwnd, string tip, string iconPath)
    {
        _hwnd = hwnd;
        _tip = tip;
        int size = GetSystemMetrics(49 /* SM_CXSMICON */);
        _icon = File.Exists(iconPath) ? LoadImageW(0, iconPath, ImageIcon, size, size, LrLoadFromFile) : 0;
        if (_icon == 0) _icon = LoadIconW(0, 32512 /* IDI_APPLICATION */);
        _taskbarCreated = RegisterWindowMessageW("TaskbarCreated");
        _proc = WndProc;
        SetWindowSubclass(hwnd, Marshal.GetFunctionPointerForDelegate(_proc), SubclassId, 0);
        Add();
    }

    public string Tip
    {
        get => _tip;
        set
        {
            if (value == _tip) return;
            _tip = value;
            var data = Data(NifTip);
            Shell_NotifyIconW(NimModify, &data);
        }
    }

    private NotifyIconData Data(uint flags)
    {
        var d = new NotifyIconData
        {
            cbSize = (uint)sizeof(NotifyIconData),
            hWnd = _hwnd,
            uID = 1,
            uFlags = flags,
            uCallbackMessage = WmTray,
            hIcon = _icon,
        };
        var tip = _tip.AsSpan(0, Math.Min(_tip.Length, 127));
        for (int i = 0; i < tip.Length; i++) d.szTip[i] = tip[i];
        return d;
    }

    private void Add()
    {
        var data = Data(NifMessage | NifIcon | NifTip);
        _added = Shell_NotifyIconW(NimAdd, &data) != 0;
    }

    private nint WndProc(nint hwnd, uint msg, nint wParam, nint lParam, nuint id, nuint refData)
    {
        if (msg == WmTray)
        {
            switch ((uint)(lParam.ToInt64() & 0xFFFF))
            {
                case WmLButtonUp:
                    OpenRequested?.Invoke();
                    return 0;
                case WmRButtonUp:
                    ShowMenu();
                    return 0;
            }
        }
        else if (msg == _taskbarCreated && _taskbarCreated != 0)
        {
            Add();  // Explorer restarted: the icon has to be added again
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    private void ShowMenu()
    {
        nint menu = CreatePopupMenu();
        AppendMenuW(menu, MfString, 1, "Open Cabled");
        AppendMenuW(menu, MfSeparator, 0, null);
        AppendMenuW(menu, MfString, 2, "Quit Cabled");
        GetCursorPos(out var pt);
        SetForegroundWindow(_hwnd);  // so the menu closes when clicking elsewhere
        int cmd = TrackPopupMenu(menu, TpmReturnCmd | TpmRightButton, pt.X, pt.Y, 0, _hwnd, 0);
        DestroyMenu(menu);
        if (cmd == 1) OpenRequested?.Invoke();
        else if (cmd == 2) QuitRequested?.Invoke();
    }

    public void Dispose()
    {
        if (_added)
        {
            var data = Data(0);
            Shell_NotifyIconW(NimDelete, &data);
            _added = false;
        }
        RemoveWindowSubclass(_hwnd, Marshal.GetFunctionPointerForDelegate(_proc), SubclassId);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NotifyIconData
    {
        public uint cbSize;
        public nint hWnd;
        public uint uID;
        public uint uFlags;
        public uint uCallbackMessage;
        public nint hIcon;
        public fixed char szTip[128];
        public uint dwState;
        public uint dwStateMask;
        public fixed char szInfo[256];
        public uint uVersion;
        public fixed char szInfoTitle[64];
        public uint dwInfoFlags;
        public Guid guidItem;
        public nint hBalloonIcon;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X, Y;
    }

    [LibraryImport("shell32.dll")] private static partial int Shell_NotifyIconW(uint message, NotifyIconData* data);
    [LibraryImport("user32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint LoadImageW(nint instance, string name, uint type, int cx, int cy, uint flags);
    [LibraryImport("user32.dll")] private static partial nint LoadIconW(nint instance, nint name);
    [LibraryImport("user32.dll")] private static partial int GetSystemMetrics(int index);
    [LibraryImport("user32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint RegisterWindowMessageW(string name);
    [LibraryImport("comctl32.dll")]
    private static partial int SetWindowSubclass(nint hwnd, nint proc, nuint id, nuint refData);
    [LibraryImport("comctl32.dll")]
    private static partial int RemoveWindowSubclass(nint hwnd, nint proc, nuint id);
    [LibraryImport("comctl32.dll")]
    private static partial nint DefSubclassProc(nint hwnd, uint msg, nint wParam, nint lParam);
    [LibraryImport("user32.dll")] private static partial nint CreatePopupMenu();
    [LibraryImport("user32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int AppendMenuW(nint menu, uint flags, nuint id, string? text);
    [LibraryImport("user32.dll")] private static partial int DestroyMenu(nint menu);
    [LibraryImport("user32.dll")] private static partial int GetCursorPos(out Point point);
    [LibraryImport("user32.dll")] private static partial int SetForegroundWindow(nint hwnd);
    [LibraryImport("user32.dll")]
    private static partial int TrackPopupMenu(nint menu, uint flags, int x, int y, int reserved, nint hwnd, nint rect);
}
