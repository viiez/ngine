using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
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
    internal static ScriptApi* ScriptApi => &Api->Script;

    // Skript-Klassen der geladenen Assembly, nach Namen sortiert (feste Reihenfolge der Aufrufe).
    private static readonly List<ScriptClass> classes = new();
    private static int tick;

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
            var found = new List<ScriptClass>();
            foreach (Type type in assembly.GetTypes())
            {
                if (!type.IsAbstract && typeof(IScript).IsAssignableFrom(type))
                {
                    created.Add((IScript)Activator.CreateInstance(type)!);
                }
                if (!type.IsAbstract && type.IsSubclassOf(typeof(Script)))
                {
                    found.Add(ScriptClass.Create(type));
                }
            }
            scripts.AddRange(created);
            found.Sort((a, b) => string.CompareOrdinal(a.Name, b.Name));
            classes.AddRange(found);
            context = loading;
        }
        catch
        {
            // Halb geladene Skripte nicht behalten; der Kontext wird wieder frei.
            loading.Unload();
            throw;
        }
        Log.Info($"{scripts.Count + classes.Count} Skript(e) aus {Path.GetFileName(file)} geladen");
        return scripts.Count + classes.Count;
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
        StopInstances();
        classes.Clear();
        var weak = new WeakReference(context);
        context!.Unload();
        context = null;
        return weak;
    }

    // --- Skript-Komponenten (R4.4) --------------------------------------------------------------

    // Meldet alle Skript-Klassen als Komponententypen an. Liefert die Zahl der angemeldeten.
    [UnmanagedCallersOnly]
    public static int RegisterScripts()
    {
        try
        {
            int registered = 0;
            foreach (ScriptClass cls in classes)
            {
                if (cls.Register())
                {
                    ++registered;
                }
                else
                {
                    Log.Error($"Skript {cls.Name} lässt sich nicht als Komponente anmelden");
                }
            }
            return registered;
        }
        catch (Exception e)
        {
            Log.Error($"Skripte lassen sich nicht anmelden: {e}");
            return -1;
        }
    }

    // Ruft FixedUpdate (phase 0) oder Update (phase 1) aller Skript-Komponenten der gebundenen
    // Welt auf; neue bekommen vorher Start, verschwundene Stop. Liefert die Zahl der Fehler.
    [UnmanagedCallersOnly]
    public static int RunScripts(int phase, float delta)
    {
        int errors = 0;
        try
        {
            Time.Delta = delta;
            if (phase == 0)
            {
                Time.Elapsed += delta;
            }
            ++tick;
            var order = new List<Script>();
            foreach (ScriptClass cls in classes)
            {
                if (cls.TypeIndex < 0)
                {
                    continue;
                }
                order.Clear();
                ReadOnlySpan<int> types = [cls.TypeIndex];
                int count = World.Chunks(types, out NativeChunk* chunks);
                for (int c = 0; c < count; ++c)
                {
                    for (int row = 0; row < chunks[c].Count; ++row)
                    {
                        ulong entity = chunks[c].Entities[row];
                        if (!cls.Instances.TryGetValue(entity, out Script? script))
                        {
                            script = (Script)Activator.CreateInstance(cls.Type)!;
                            script.Entity = new Entity(entity);
                            cls.Load(script, (byte*)chunks[c].Columns[0] + (long)row * SizeOf(cls));
                            cls.Instances.Add(entity, script);
                        }
                        script.seen = tick;
                        order.Add(script);
                    }
                }
                // Verschwundene zuerst stoppen, in fester Reihenfolge.
                foreach (ulong gone in cls.Instances.Where(p => p.Value.seen != tick).Select(p => p.Key).OrderBy(k => k).ToList())
                {
                    errors += Call(cls.Instances[gone], s => s.Stop());
                    cls.Instances.Remove(gone);
                }
                foreach (Script script in order)
                {
                    if (!script.started)
                    {
                        script.started = true;
                        errors += Call(script, s => s.Start());
                    }
                    errors += phase == 0 ? Call(script, s => s.FixedUpdate()) : Call(script, s => s.Update());
                }
            }
            return errors;
        }
        catch (Exception e)
        {
            Log.Error($"Fehler beim Ausführen der Skripte: {e}");
            return errors + 1;
        }
    }

    // Stellt Ereignisse der Physik den Skripten der beteiligten Entities zu.
    [UnmanagedCallersOnly]
    public static int DeliverEvents(NativeEvent* events, int count)
    {
        int errors = 0;
        try
        {
            if (count <= 0)
            {
                return 0;
            }
            var byEntity = new Dictionary<ulong, List<Script>>();
            foreach (ScriptClass cls in classes)
            {
                foreach (var (entity, script) in cls.Instances)
                {
                    if (!byEntity.TryGetValue(entity, out var list))
                    {
                        byEntity[entity] = list = new List<Script>();
                    }
                    list.Add(script);
                }
            }
            if (byEntity.Count == 0)
            {
                return 0;
            }
            foreach (var list in byEntity.Values)
            {
                list.Sort((a, b) => string.CompareOrdinal(a.GetType().Name, b.GetType().Name));
            }
            void Each(ulong entity, Action<Script> action)
            {
                if (byEntity.TryGetValue(entity, out var list))
                {
                    foreach (Script script in list)
                    {
                        errors += Call(script, action);
                    }
                }
            }
            for (int i = 0; i < count; ++i)
            {
                NativeEvent e = events[i];
                var a = new Entity(e.A);
                var b = new Entity(e.B);
                switch (e.Kind)
                {
                    case EventKind.ContactBegan:
                        Each(e.A, s => s.OnContactBegan(new Contact { Other = b, Point = e.Point, Normal = e.Normal }));
                        Each(e.B, s => s.OnContactBegan(new Contact { Other = a, Point = e.Point, Normal = -e.Normal }));
                        break;
                    case EventKind.ContactEnded:
                        Each(e.A, s => s.OnContactEnded(b));
                        Each(e.B, s => s.OnContactEnded(a));
                        break;
                    case EventKind.TriggerEntered:
                        Each(e.A, s => s.OnTriggerEntered(b));
                        Each(e.B, s => s.OnTriggerEntered(a));
                        break;
                    case EventKind.TriggerLeft:
                        Each(e.A, s => s.OnTriggerLeft(b));
                        Each(e.B, s => s.OnTriggerLeft(a));
                        break;
                    case EventKind.Landed:
                        Each(e.A, s => s.OnLanded(e.Value, e.Material));
                        break;
                    case EventKind.Step:
                        Each(e.A, s => s.OnStep(e.Point, e.Material, e.Value != 0));
                        break;
                }
            }
            return errors;
        }
        catch (Exception e)
        {
            Log.Error($"Ereignisse lassen sich nicht zustellen: {e}");
            return errors + 1;
        }
    }

    // Schreibt die Felder aller Instanzen in ihre Komponenten (vor dem Speichern, Snapshot, ...).
    [UnmanagedCallersOnly]
    public static int StoreFields() => Guard(() => ForEachInstance((cls, script, data) => cls.Store(script, data), write: true));

    // Liest die Felder aller Instanzen aus ihren Komponenten (nach Änderungen im Inspector, Undo).
    [UnmanagedCallersOnly]
    public static int LoadFields() => Guard(() => ForEachInstance((cls, script, data) => cls.Load(script, data), write: false));

    // Stoppt alle Instanzen und vergisst sie; beim nächsten Lauf entstehen sie neu.
    [UnmanagedCallersOnly]
    public static int ResetInstances() => Guard(StopInstances);

    private static void StopInstances()
    {
        foreach (ScriptClass cls in classes)
        {
            foreach (ulong entity in cls.Instances.Keys.OrderBy(k => k).ToList())
            {
                Call(cls.Instances[entity], s => s.Stop());
            }
            cls.Instances.Clear();
        }
        Time.Elapsed = 0;
    }

    private delegate void InstanceAction(ScriptClass cls, Script script, void* data);

    private static void ForEachInstance(InstanceAction action, bool write)
    {
        foreach (ScriptClass cls in classes)
        {
            if (cls.TypeIndex < 0)
            {
                continue;
            }
            foreach (var (entity, script) in cls.Instances)
            {
                void* data = Api->World.Get(entity, cls.TypeIndex, write ? 1 : 0);
                if (data != null)
                {
                    action(cls, script, data);
                }
            }
        }
    }

    private static long SizeOf(ScriptClass cls) => Api->Script.FieldOffset(cls.TypeIndex, -1);

    // Ruft eine Methode eines Skripts auf; ein Fehler darin hält die übrigen nicht auf.
    private static int Call(Script script, Action<Script> action)
    {
        try
        {
            action(script);
            return 0;
        }
        catch (Exception e)
        {
            Log.Error($"Fehler in {script.GetType().Name} an {script.Entity}: {e}");
            return 1;
        }
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
