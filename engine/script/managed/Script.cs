using System;

namespace NGine;

// Daten eines Frames für Skripte. Positions zeigt direkt auf Speicher der Engine (keine Kopie).
public readonly ref struct Frame
{
    public readonly float DeltaTime;
    public readonly Span<Position> Positions;

    public Frame(float deltaTime, Span<Position> positions)
    {
        DeltaTime = deltaTime;
        Positions = positions;
    }
}

// Ein Skript im Durchstich D2. Die richtige Schnittstelle (Skript-Komponenten, aus der Reflection
// erzeugte API) folgt in R4 (SCR-02, SCR-03).
public interface IScript
{
    void Start() { }
    void Update(in Frame frame) { }
    void Stop() { }
}

// Ein Skript, das an einer Entity hängt (SCR-03, R4.4). Jede Klasse, die davon erbt, wird beim
// Laden ein eigener Komponententyp mit ihrem Namen; ihre öffentlichen Felder sind die Felder der
// Komponente: im Inspector, in Szenen und Prefabs. Private Felder sind Laufzeitzustand und gehen
// beim Neuladen verloren.
//
// Erlaubte Feldtypen: bool, ganze Zahlen, float, double, string, Enums und Structs der Engine
// (Vec3, DVec3, Quat, ...). Attribute: [Range], [Color], [Asset], [HideInInspector] fehlt bewusst:
// was nicht gespeichert werden soll, ist privat.
public abstract class Script
{
    // Die Entity, an der das Skript hängt.
    public Entity Entity { get; internal set; }

    // Einmal vor dem ersten FixedUpdate bzw. Update.
    public virtual void Start() { }
    // Im festen Schritt vor der Physik (Time.Delta ist der feste Schritt).
    public virtual void FixedUpdate() { }
    // Einmal pro Bild (Time.Delta ist die Bildzeit).
    public virtual void Update() { }
    // Wenn die Komponente oder die Entity verschwindet, beim Stoppen und vor dem Neuladen.
    public virtual void Stop() { }

    // Bequemer Zugriff auf Komponenten der eigenen Entity.
    protected ref T Get<T>() where T : unmanaged, IComponent => ref Entity.Get<T>();
    protected ref readonly T Read<T>() where T : unmanaged, IComponent => ref Entity.Read<T>();

    internal bool started;
    internal int seen;
}

// Zeit des laufenden Aufrufs.
public static class Time
{
    // Sekunden seit dem letzten Aufruf derselben Art (fester Schritt oder Bild).
    public static float Delta { get; internal set; }
    // Sekunden simulierter Zeit seit dem Start der Skripte.
    public static double Elapsed { get; internal set; }
}

// Gültiger Bereich eines Zahlenfeldes im Inspector.
[AttributeUsage(AttributeTargets.Field)]
public sealed class RangeAttribute : Attribute
{
    public double Min { get; }
    public double Max { get; }
    public double Step { get; init; }
    public RangeAttribute(double min, double max) { Min = min; Max = max; }
}

// Vec3 oder Vec4 als Farbe (Farbwähler im Inspector).
[AttributeUsage(AttributeTargets.Field)]
public sealed class ColorAttribute : Attribute { }

// ulong mit der Kennung eines Assets.
[AttributeUsage(AttributeTargets.Field)]
public sealed class AssetAttribute : Attribute { }
