using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace NGine;

// Einstieg für den C++-Host. Alle Methoden mit [UnmanagedCallersOnly] ruft C++ über
// Funktionszeiger auf, die hostfxr liefert. Ausnahmen dürfen nicht nach C++ durchschlagen; jede
// Methode fängt sie und meldet einen Fehlercode.
public static unsafe class Host
{
    private static NativeApi api;
    private static bool initialized;
    private static ScriptContext? context;
    private static readonly List<IScript> scripts = new();

    internal static NativeApi* Api => initialized ? (NativeApi*)Unsafe.AsPointer(ref api) : null;

    [UnmanagedCallersOnly]
    public static int Initialize(NativeApi* native, int size)
    {
        if (size != sizeof(NativeApi))
        {
            return -1;
        }
        api = *native;
        initialized = true;
        Log.Info($"C#-Laufzeit bereit: .NET {Environment.Version}");
        return 0;
    }

    // Lädt eine Assembly mit Skripten in einen entladbaren Kontext und erzeugt jedes Skript.
    // Liefert die Anzahl der Skripte oder einen negativen Fehlercode.
    [UnmanagedCallersOnly]
    public static int LoadScripts(byte* path, int length)
    {
        try
        {
            if (context != null)
            {
                Log.Error("Es sind schon Skripte geladen");
                return -2;
            }
            string file = System.Text.Encoding.UTF8.GetString(path, length);
            int count = Load(file);
            return count;
        }
        catch (Exception e)
        {
            Log.Error($"Skripte lassen sich nicht laden: {e.GetType().Name}: {e.Message}");
            return -1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int Load(string file)
    {
        var loading = new ScriptContext();
        try
        {
            // Aus einem Strom laden statt aus der Datei: Die Datei bleibt frei und lässt sich
            // überschreiben, während das Spiel läuft (Hot Reload).
            using var stream = new MemoryStream(File.ReadAllBytes(file));
            Assembly assembly = loading.LoadFromStream(stream);
            var created = new List<IScript>();
            foreach (Type type in assembly.GetTypes())
            {
                if (!type.IsAbstract && typeof(IScript).IsAssignableFrom(type))
                {
                    created.Add((IScript)Activator.CreateInstance(type)!);
                }
            }
            scripts.AddRange(created);
            context = loading;
        }
        catch
        {
            // Halb geladene Skripte nicht behalten; der Kontext wird wieder frei.
            loading.Unload();
            throw;
        }
        Log.Info($"{scripts.Count} Skript(e) aus {Path.GetFileName(file)} geladen");
        return scripts.Count;
    }

    [UnmanagedCallersOnly]
    public static int Start()
    {
        return Guard(() =>
        {
            foreach (IScript script in scripts)
            {
                script.Start();
            }
        });
    }

    [UnmanagedCallersOnly]
    public static int Update(float deltaTime, Position* positions, int count)
    {
        try
        {
            var frame = new Frame(deltaTime, new Span<Position>(positions, count));
            foreach (IScript script in scripts)
            {
                script.Update(frame);
            }
            return 0;
        }
        catch (Exception e)
        {
            Log.Error($"Fehler im Skript: {e}");
            return -1;
        }
    }

    // Stoppt und entlädt die Skripte. Liefert 1, wenn der Kontext wirklich freigegeben wurde,
    // 0, wenn noch Verweise ihn festhalten, und -1 bei einem Fehler.
    [UnmanagedCallersOnly]
    public static int UnloadScripts()
    {
        try
        {
            if (context == null)
            {
                return 1;
            }
            WeakReference weak = StopAndUnload();
            // Entladen ist kooperativ: Der Kontext verschwindet erst, wenn der GC keine Verweise
            // mehr findet. Einige Durchläufe reichen normalerweise.
            for (int i = 0; weak.IsAlive && i < 10; ++i)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
            bool collected = !weak.IsAlive;
            Log.Info(collected ? "Skripte entladen" : "Skripte entladen, aber noch nicht freigegeben");
            return collected ? 1 : 0;
        }
        catch (Exception e)
        {
            Log.Error($"Skripte lassen sich nicht entladen: {e}");
            return -1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference StopAndUnload()
    {
        foreach (IScript script in scripts)
        {
            script.Stop();
        }
        scripts.Clear();
        var weak = new WeakReference(context);
        context!.Unload();
        context = null;
        return weak;
    }

    // --- Welt und Bindings (R4.4) ----------------------------------------------------------------

    // Die Engine hat eine andere Welt gebunden (oder keine); Typen werden neu nachgeschlagen.
    [UnmanagedCallersOnly]
    public static void WorldChanged()
    {
        ++World.Generation;
    }

    // Meldet Größe und Offsets aller erzeugten Structs, wie C# sie sieht.
    [UnmanagedCallersOnly]
    public static int DescribeLayout(delegate* unmanaged<byte*, int, byte*, int, long, void> report)
    {
        try
        {
            GeneratedBindings.DescribeLayout(new LayoutWriter(report));
            return 0;
        }
        catch (Exception e)
        {
            Log.Error($"Anordnung der Bindings lässt sich nicht melden: {e}");
            return -1;
        }
    }

    // Ruft eine statische Methode ohne Parameter aus den geladenen Skripten auf, z. B.
    // "Tests.Scripts.WorldTests.MoveAll". 0: gut, -1: Fehler im Skript, -2: nicht gefunden.
    [UnmanagedCallersOnly]
    public static int Invoke(byte* name, int length)
    {
        try
        {
            string full = System.Text.Encoding.UTF8.GetString(name, length);
            int dot = full.LastIndexOf('.');
            if (context == null || dot <= 0)
            {
                Log.Error($"{full}: keine Skripte geladen oder kein Name der Form Typ.Methode");
                return -2;
            }
            string typeName = full[..dot];
            string methodName = full[(dot + 1)..];
            foreach (Assembly assembly in context.Assemblies)
            {
                Type? type = assembly.GetType(typeName);
                MethodInfo? method = type?.GetMethod(methodName,
                    BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static, Type.EmptyTypes);
                if (method != null)
                {
                    method.Invoke(null, null);
                    return 0;
                }
            }
            Log.Error($"{full} gibt es in den Skripten nicht");
            return -2;
        }
        catch (TargetInvocationException e) when (e.InnerException != null)
        {
            Log.Error($"Fehler im Skript: {e.InnerException}");
            return -1;
        }
        catch (Exception e)
        {
            Log.Error($"Fehler im Skript: {e}");
            return -1;
        }
    }

    // --- Prüfen und Messen des Aufrufwegs -------------------------------------------------------

    [UnmanagedCallersOnly]
    public static int Add(int a, int b) => a + b;

    // Ruft count-mal Add in C++ auf und liefert die Dauer in Nanosekunden; die Summe landet in
    // result, damit nichts wegoptimiert wird.
    [UnmanagedCallersOnly]
    public static long BenchmarkNativeCalls(int count, int* result)
    {
        int sum = 0;
        long start = Stopwatch.GetTimestamp();
        for (int i = 0; i < count; ++i)
        {
            sum = api.Add(sum, 1);
        }
        long elapsed = Stopwatch.GetTimestamp() - start;
        *result = sum;
        return (long)(elapsed * (1_000_000_000.0 / Stopwatch.Frequency));
    }

    private static int Guard(Action action)
    {
        try
        {
            action();
            return 0;
        }
        catch (Exception e)
        {
            Log.Error($"Fehler im Skript: {e}");
            return -1;
        }
    }
}

// Entladbarer Ladekontext für Spielskripte. NGine.Scripting selbst kommt aus dem Standardkontext,
// damit IScript in beiden Kontexten derselbe Typ ist.
internal sealed class ScriptContext : AssemblyLoadContext
{
    public ScriptContext() : base("Skripte", isCollectible: true) { }

    protected override Assembly? Load(AssemblyName name) => null;
}
