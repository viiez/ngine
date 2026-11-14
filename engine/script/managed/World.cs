using System;
using System.Runtime.CompilerServices;
using System.Text;

namespace NGine;

// Die Welt, an die die Engine die Skripte gebunden hat.
public static unsafe class World
{
    // Zählt Wechsel der gebundenen Welt; Typen werden danach neu nachgeschlagen.
    internal static int Generation;

    // Legt eine Entity an. Sie lebt erst nach den Skripten; bis dahin taugt der Griff nur für
    // weitere Befehle (Set, Parent, Name).
    public static Entity Create(string name = "")
    {
        byte[] bytes = Encoding.UTF8.GetBytes(name);
        fixed (byte* text = bytes)
        {
            return new Entity(Entity.Api->Create(text, bytes.Length));
        }
    }

    // Erste Entity mit diesem Namen (kleinste Kennung); Entity.None, wenn es keine gibt.
    public static Entity Find(string name)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(name);
        fixed (byte* text = bytes)
        {
            return new Entity(Entity.Api->Find(text, bytes.Length));
        }
    }

    internal static int Chunks(ReadOnlySpan<int> types, out NativeChunk* chunks)
    {
        NativeChunk* result = null;
        int count;
        fixed (int* data = types)
        {
            count = Entity.Api->Query(data, types.Length, &result);
        }
        if (count < 0)
        {
            throw new InvalidOperationException("Abfrage über unbekannte Komponenten");
        }
        chunks = result;
        return count;
    }

    internal static void RequireTrivial<T>() where T : unmanaged, IComponent
    {
        if (!T.Trivial)
        {
            throw new InvalidOperationException($"{T.TypeName} enthält Texte und taugt nicht für Abfragen");
        }
    }

    // Alle Entities mit diesen Komponenten, ohne Kopie:
    //   foreach (var row in World.Query<T1>()) { row.Item1.X += 1; }
    public static Query<T1> Query<T1>()
        where T1 : unmanaged, IComponent
    {
        return new Query<T1>();
    }

    // Alle Entities mit diesen Komponenten, ohne Kopie:
    //   foreach (var row in World.Query<T1, T2>()) { row.Item1.X += 1; }
    public static Query<T1, T2> Query<T1, T2>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
    {
        return new Query<T1, T2>();
    }

    // Alle Entities mit diesen Komponenten, ohne Kopie:
    //   foreach (var row in World.Query<T1, T2, T3>()) { row.Item1.X += 1; }
    public static Query<T1, T2, T3> Query<T1, T2, T3>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
    {
        return new Query<T1, T2, T3>();
    }

    // Alle Entities mit diesen Komponenten, ohne Kopie:
    //   foreach (var row in World.Query<T1, T2, T3, T4>()) { row.Item1.X += 1; }
    public static Query<T1, T2, T3, T4> Query<T1, T2, T3, T4>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
    {
        return new Query<T1, T2, T3, T4>();
    }
}

// Eine Zeile einer Abfrage: die Entity und Referenzen auf ihre Komponenten.
public readonly ref struct Row<T1>
    where T1 : unmanaged, IComponent
{
    public readonly Entity Entity;
    public readonly ref T1 Item1;

    internal Row(Entity entity, ref T1 item1)
    {
        Entity = entity;
        Item1 = ref item1;
    }
}

public readonly unsafe struct Query<T1>
    where T1 : unmanaged, IComponent
{
    public Enumerator GetEnumerator()
    {
        World.RequireTrivial<T1>();
        ReadOnlySpan<int> types = [ComponentType<T1>.Index];
        int count = World.Chunks(types, out NativeChunk* chunks);
        return new Enumerator(chunks, count);
    }

    // Anzahl der Entities.
    public int Count()
    {
        int total = 0;
        foreach (var _ in this)
        {
            ++total;
        }
        return total;
    }

    public ref struct Enumerator
    {
        private readonly NativeChunk* chunks;
        private readonly int count;
        private int chunk;
        private int row;

        internal Enumerator(NativeChunk* chunks, int count)
        {
            this.chunks = chunks;
            this.count = count;
            chunk = 0;
            row = -1;
        }

        public bool MoveNext()
        {
            while (chunk < count)
            {
                if (++row < chunks[chunk].Count)
                {
                    return true;
                }
                ++chunk;
                row = -1;
            }
            return false;
        }

        public Row<T1> Current
        {
            get
            {
                NativeChunk* c = chunks + chunk;
                return new Row<T1>(new Entity(c->Entities[row]), ref Unsafe.AsRef<T1>((byte*)c->Columns[0] + (long)row * sizeof(T1)));
            }
        }
    }
}

// Eine Zeile einer Abfrage: die Entity und Referenzen auf ihre Komponenten.
public readonly ref struct Row<T1, T2>
    where T1 : unmanaged, IComponent
    where T2 : unmanaged, IComponent
{
    public readonly Entity Entity;
    public readonly ref T1 Item1;
    public readonly ref T2 Item2;

    internal Row(Entity entity, ref T1 item1, ref T2 item2)
    {
        Entity = entity;
        Item1 = ref item1;
        Item2 = ref item2;
    }
}

public readonly unsafe struct Query<T1, T2>
    where T1 : unmanaged, IComponent
    where T2 : unmanaged, IComponent
{
    public Enumerator GetEnumerator()
    {
        World.RequireTrivial<T1>();
        World.RequireTrivial<T2>();
        ReadOnlySpan<int> types = [ComponentType<T1>.Index, ComponentType<T2>.Index];
        int count = World.Chunks(types, out NativeChunk* chunks);
        return new Enumerator(chunks, count);
    }

    // Anzahl der Entities.
    public int Count()
    {
        int total = 0;
        foreach (var _ in this)
        {
            ++total;
        }
        return total;
    }

    public ref struct Enumerator
    {
        private readonly NativeChunk* chunks;
        private readonly int count;
        private int chunk;
        private int row;

        internal Enumerator(NativeChunk* chunks, int count)
        {
            this.chunks = chunks;
            this.count = count;
            chunk = 0;
            row = -1;
        }

        public bool MoveNext()
        {
            while (chunk < count)
            {
                if (++row < chunks[chunk].Count)
                {
                    return true;
                }
                ++chunk;
                row = -1;
            }
            return false;
        }

        public Row<T1, T2> Current
        {
            get
            {
                NativeChunk* c = chunks + chunk;
                return new Row<T1, T2>(new Entity(c->Entities[row]), ref Unsafe.AsRef<T1>((byte*)c->Columns[0] + (long)row * sizeof(T1)), ref Unsafe.AsRef<T2>((byte*)c->Columns[1] + (long)row * sizeof(T2)));
            }
        }
    }
}

// Eine Zeile einer Abfrage: die Entity und Referenzen auf ihre Komponenten.
public readonly ref struct Row<T1, T2, T3>
    where T1 : unmanaged, IComponent
    where T2 : unmanaged, IComponent
    where T3 : unmanaged, IComponent
{
    public readonly Entity Entity;
    public readonly ref T1 Item1;
    public readonly ref T2 Item2;
    public readonly ref T3 Item3;

    internal Row(Entity entity, ref T1 item1, ref T2 item2, ref T3 item3)
    {
        Entity = entity;
        Item1 = ref item1;
        Item2 = ref item2;
        Item3 = ref item3;
    }
}

public readonly unsafe struct Query<T1, T2, T3>
    where T1 : unmanaged, IComponent
    where T2 : unmanaged, IComponent
    where T3 : unmanaged, IComponent
{
    public Enumerator GetEnumerator()
    {
        World.RequireTrivial<T1>();
        World.RequireTrivial<T2>();
        World.RequireTrivial<T3>();
        ReadOnlySpan<int> types = [ComponentType<T1>.Index, ComponentType<T2>.Index, ComponentType<T3>.Index];
        int count = World.Chunks(types, out NativeChunk* chunks);
        return new Enumerator(chunks, count);
    }

    // Anzahl der Entities.
    public int Count()
    {
        int total = 0;
        foreach (var _ in this)
        {
            ++total;
        }
        return total;
    }

    public ref struct Enumerator
    {
        private readonly NativeChunk* chunks;
        private readonly int count;
        private int chunk;
        private int row;

        internal Enumerator(NativeChunk* chunks, int count)
        {
            this.chunks = chunks;
            this.count = count;
            chunk = 0;
            row = -1;
        }

        public bool MoveNext()
        {
            while (chunk < count)
            {
                if (++row < chunks[chunk].Count)
                {
                    return true;
                }
                ++chunk;
                row = -1;
            }
            return false;
        }

        public Row<T1, T2, T3> Current
        {
            get
            {
                NativeChunk* c = chunks + chunk;
                return new Row<T1, T2, T3>(new Entity(c->Entities[row]), ref Unsafe.AsRef<T1>((byte*)c->Columns[0] + (long)row * sizeof(T1)), ref Unsafe.AsRef<T2>((byte*)c->Columns[1] + (long)row * sizeof(T2)), ref Unsafe.AsRef<T3>((byte*)c->Columns[2] + (long)row * sizeof(T3)));
            }
        }
    }
}

// Eine Zeile einer Abfrage: die Entity und Referenzen auf ihre Komponenten.
public readonly ref struct Row<T1, T2, T3, T4>
    where T1 : unmanaged, IComponent
    where T2 : unmanaged, IComponent
    where T3 : unmanaged, IComponent
    where T4 : unmanaged, IComponent
{
    public readonly Entity Entity;
    public readonly ref T1 Item1;
    public readonly ref T2 Item2;
    public readonly ref T3 Item3;
    public readonly ref T4 Item4;

    internal Row(Entity entity, ref T1 item1, ref T2 item2, ref T3 item3, ref T4 item4)
    {
        Entity = entity;
        Item1 = ref item1;
        Item2 = ref item2;
        Item3 = ref item3;
        Item4 = ref item4;
    }
}

public readonly unsafe struct Query<T1, T2, T3, T4>
    where T1 : unmanaged, IComponent
    where T2 : unmanaged, IComponent
    where T3 : unmanaged, IComponent
    where T4 : unmanaged, IComponent
{
    public Enumerator GetEnumerator()
    {
        World.RequireTrivial<T1>();
        World.RequireTrivial<T2>();
        World.RequireTrivial<T3>();
        World.RequireTrivial<T4>();
        ReadOnlySpan<int> types = [ComponentType<T1>.Index, ComponentType<T2>.Index, ComponentType<T3>.Index, ComponentType<T4>.Index];
        int count = World.Chunks(types, out NativeChunk* chunks);
        return new Enumerator(chunks, count);
    }

    // Anzahl der Entities.
    public int Count()
    {
        int total = 0;
        foreach (var _ in this)
        {
            ++total;
        }
        return total;
    }

    public ref struct Enumerator
    {
        private readonly NativeChunk* chunks;
        private readonly int count;
        private int chunk;
        private int row;

        internal Enumerator(NativeChunk* chunks, int count)
        {
            this.chunks = chunks;
            this.count = count;
            chunk = 0;
            row = -1;
        }

        public bool MoveNext()
        {
            while (chunk < count)
            {
                if (++row < chunks[chunk].Count)
                {
                    return true;
                }
                ++chunk;
                row = -1;
            }
            return false;
        }

        public Row<T1, T2, T3, T4> Current
        {
            get
            {
                NativeChunk* c = chunks + chunk;
                return new Row<T1, T2, T3, T4>(new Entity(c->Entities[row]), ref Unsafe.AsRef<T1>((byte*)c->Columns[0] + (long)row * sizeof(T1)), ref Unsafe.AsRef<T2>((byte*)c->Columns[1] + (long)row * sizeof(T2)), ref Unsafe.AsRef<T3>((byte*)c->Columns[2] + (long)row * sizeof(T3)), ref Unsafe.AsRef<T4>((byte*)c->Columns[3] + (long)row * sizeof(T4)));
            }
        }
    }
}
