using System;
using System.Buffers;
using System.Text;

namespace NGine;

// Log der Engine für Skripte. Schreibt über die Engine, damit C#-Ausgaben in denselben Sinks
// landen wie die aus C++ (Konsole, später Editor).
public static unsafe class Log
{
    public enum Level { Trace, Debug, Info, Warn, Error, Fatal }

    public static void Trace(string message) => Write(Level.Trace, message);
    public static void Debug(string message) => Write(Level.Debug, message);
    public static void Info(string message) => Write(Level.Info, message);
    public static void Warn(string message) => Write(Level.Warn, message);
    public static void Error(string message) => Write(Level.Error, message);

    public static void Write(Level level, string message)
    {
        var api = Host.Api;
        if (api == null || api->Log == null)
        {
            return;
        }
        // Kurze Texte auf dem Stack, lange aus dem Pool: kein Müll für den GC pro Logzeile.
        int max = Encoding.UTF8.GetMaxByteCount(message.Length);
        byte[]? rented = null;
        Span<byte> buffer = max <= 512 ? stackalloc byte[max] : (rented = ArrayPool<byte>.Shared.Rent(max));
        int length = Encoding.UTF8.GetBytes(message, buffer);
        fixed (byte* text = buffer)
        {
            api->Log((int)level, text, length);
        }
        if (rented != null)
        {
            ArrayPool<byte>.Shared.Return(rented);
        }
    }
}
