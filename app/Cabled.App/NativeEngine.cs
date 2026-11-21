using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Cabled.App;

/// <summary>P/Invoke wrapper over cabled_core.dll (windows/src/core_api/cabled_core.h).</summary>
internal sealed partial class NativeEngine : IDisposable
{
    private nint _handle;
    private byte[] _statusBuffer = new byte[4096];

    public NativeEngine(object? config)
    {
        string? json = config is null ? null : JsonSerializer.Serialize(config);
        _handle = Native.cabled_engine_create(json);
        if (_handle == 0) throw new InvalidOperationException("cabled_engine_create failed (bad config?)");
    }

    public static string Version => Marshal.PtrToStringUTF8(Native.cabled_version()) ?? "?";

    public void Start() => Native.cabled_engine_start(_handle);
    public void Stop() => Native.cabled_engine_stop(_handle);
    public void SetPreviewEnabled(bool enabled) => Native.cabled_engine_set_preview_enabled(_handle, enabled ? 1 : 0);

    public unsafe JsonDocument? Status()
    {
        while (true)
        {
            int needed;
            fixed (byte* p = _statusBuffer)
                needed = Native.cabled_engine_status_json(_handle, p, _statusBuffer.Length);
            if (needed <= 0) return null;
            if (needed > _statusBuffer.Length) { _statusBuffer = new byte[needed * 2]; continue; }
            return JsonDocument.Parse(_statusBuffer.AsMemory(0, needed - 1));
        }
    }

    /// <summary>Copies the newest preview frame into <paramref name="buffer"/> (resized as needed).</summary>
    public unsafe bool TryCopyPreview(ref byte[] buffer, ref ulong seq, out int width, out int height)
    {
        int w, h;
        ulong s = seq;
        int rc;
        fixed (byte* p = buffer)
            rc = Native.cabled_engine_copy_preview(_handle, p, buffer.Length, &w, &h, &s);
        if (rc == -1)
        {
            buffer = new byte[w * h * 4];
            fixed (byte* p = buffer)
                rc = Native.cabled_engine_copy_preview(_handle, p, buffer.Length, &w, &h, &s);
        }
        width = w;
        height = h;
        if (rc != 1) return false;
        seq = s;
        return true;
    }

    public unsafe string TakeLog()
    {
        int needed = Native.cabled_engine_take_log(_handle, null, 0);
        if (needed <= 1) return "";
        var buffer = new byte[needed];
        fixed (byte* p = buffer)
            needed = Native.cabled_engine_take_log(_handle, p, buffer.Length);
        return needed <= buffer.Length ? Encoding.UTF8.GetString(buffer, 0, needed - 1) : "";
    }

    public void Dispose()
    {
        if (_handle != 0)
        {
            Native.cabled_engine_destroy(_handle);
            _handle = 0;
        }
    }

    private static unsafe partial class Native
    {
        private const string Dll = "cabled_core.dll";

        [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf8)]
        public static partial nint cabled_engine_create(string? configJson);

        [LibraryImport(Dll)] public static partial void cabled_engine_start(nint engine);
        [LibraryImport(Dll)] public static partial void cabled_engine_stop(nint engine);
        [LibraryImport(Dll)] public static partial void cabled_engine_destroy(nint engine);
        [LibraryImport(Dll)] public static partial int cabled_engine_status_json(nint engine, byte* buffer, int capacity);
        [LibraryImport(Dll)]
        public static partial int cabled_engine_copy_preview(nint engine, byte* buffer, int capacity, int* width,
                                                             int* height, ulong* seq);
        [LibraryImport(Dll)] public static partial void cabled_engine_set_preview_enabled(nint engine, int enabled);
        [LibraryImport(Dll)] public static partial int cabled_engine_take_log(nint engine, byte* buffer, int capacity);
        [LibraryImport(Dll)] public static partial nint cabled_version();
    }
}
