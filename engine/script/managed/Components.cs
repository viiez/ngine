using System;
using System.Text;

namespace NGine;

// Eine Komponente der Engine in C#. Die Structs erzeugt ngine_bindgen aus der Reflection
// (Components.g.cs): gleiche Größe und gleiche Offsets wie in C++, Texte fehlen.
public interface IComponent
{
    // Name des Typs in der Reflection.
    static abstract string TypeName { get; }
    // Lässt sich byteweise kopieren (keine Texte). Nur solche Komponenten gibt Get als
    // veränderbare Referenz heraus.
    static abstract bool Trivial { get; }
}

// Index des Typs in der Registry der gebundenen Welt, einmal pro Welt nachgeschlagen.
internal static unsafe class ComponentType<T> where T : unmanaged, IComponent
{
    private static int index = -1;
    private static int generation = -1;

    public static int Index
    {
        get
        {
            if (generation != World.Generation)
            {
                index = Lookup(T.TypeName);
                generation = World.Generation;
            }
            if (index < 0)
            {
                throw new InvalidOperationException(
                    $"Die Komponente {T.TypeName} kennt die Welt nicht (oder es ist keine Welt gebunden)");
            }
            return index;
        }
    }

    private static int Lookup(string name)
    {
        var api = Host.Api;
        if (api == null)
        {
            return -1;
        }
        Span<byte> buffer = stackalloc byte[Encoding.UTF8.GetMaxByteCount(name.Length)];
        int length = Encoding.UTF8.GetBytes(name, buffer);
        fixed (byte* text = buffer)
        {
            return api->World.TypeIndex(text, length);
        }
    }
}

// Sammelt die Anordnung der Structs, wie C# sie sieht, für den Vergleich mit der Reflection.
public sealed unsafe class LayoutWriter
{
    private readonly delegate* unmanaged<byte*, int, byte*, int, long, void> report;
    private string current = "";

    internal LayoutWriter(delegate* unmanaged<byte*, int, byte*, int, long, void> report)
    {
        this.report = report;
    }

    public void Type(string name, int size)
    {
        current = name;
        Send(name, "", size);
    }

    public void Field(string name, long offset) => Send(current, name, offset);

    private void Send(string type, string field, long value)
    {
        byte[] t = Encoding.UTF8.GetBytes(type);
        byte[] f = Encoding.UTF8.GetBytes(field);
        fixed (byte* tp = t)
        fixed (byte* fp = f)
        {
            report(tp, t.Length, fp, f.Length, value);
        }
    }
}

// Einstieg in die erzeugte Beschreibung der Anordnung. Ohne erzeugte Datei (Build ohne
// ngine_bindgen) bleibt Describe leer.
internal static partial class GeneratedBindings
{
    internal static void DescribeLayout(LayoutWriter w) => Describe(w);

    static partial void Describe(LayoutWriter w);
}
