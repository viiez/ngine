using System;
using System.Runtime.CompilerServices;
using System.Text;

namespace NGine;

// Griff auf eine Entity der gebundenen Welt (wie ngine::world::Entity, nur die Kennung).
//
// Get liefert eine Referenz direkt in den Speicher der Welt, ohne Kopie. Sie gilt bis zur
// nächsten Strukturänderung; Strukturänderungen aus Skripten (Create, neue Komponente, Remove,
// Destroy, Parent, Name) sammelt die Engine und führt sie erst nach den Skripten aus. Innerhalb
// eines Aufrufs bleibt eine Referenz deshalb gültig; über Aufrufe hinweg darf man sie nicht halten.
public readonly unsafe struct Entity : IEquatable<Entity>
{
    public readonly ulong Id;

    public Entity(ulong id) { Id = id; }

    public static readonly Entity None = default;

    // Lebt die Entity? Gerade erst angelegte Entities leben erst nach den Skripten.
    public bool Alive => Id != 0 && Api->Alive(Id) != 0;

    // Veränderbare Referenz; markiert die Komponente als geändert. Wirft, wenn sie fehlt.
    public ref T Get<T>() where T : unmanaged, IComponent
    {
        if (!T.Trivial)
        {
            throw new InvalidOperationException(
                $"{T.TypeName} enthält Texte und lässt sich nur lesen (Read) oder setzen (Set)");
        }
        void* data = Api->Get(Id, ComponentType<T>.Index, 1);
        if (data == null)
        {
            throw Missing<T>();
        }
        return ref Unsafe.AsRef<T>(data);
    }

    // Nur lesen; markiert nichts als geändert.
    public ref readonly T Read<T>() where T : unmanaged, IComponent
    {
        void* data = Api->Get(Id, ComponentType<T>.Index, 0);
        if (data == null)
        {
            throw Missing<T>();
        }
        return ref Unsafe.AsRef<T>(data);
    }

    public bool TryRead<T>(out T value) where T : unmanaged, IComponent
    {
        void* data = Api->Get(Id, ComponentType<T>.Index, 0);
        value = data != null ? *(T*)data : default;
        return data != null;
    }

    public bool Has<T>() where T : unmanaged, IComponent => Api->Has(Id, ComponentType<T>.Index) != 0;

    // Setzt die Komponente. Ist sie schon da, sofort; sonst nach den Skripten. Bei Typen mit
    // Texten bleiben die Texte, wie sie sind (bzw. leer).
    public Entity Set<T>(in T value) where T : unmanaged, IComponent
    {
        fixed (T* data = &value)
        {
            Check(Api->Set(Id, ComponentType<T>.Index, data), "setzen");
        }
        return this;
    }

    // Entfernt die Komponente nach den Skripten.
    public Entity Remove<T>() where T : unmanaged, IComponent
    {
        Check(Api->Remove(Id, ComponentType<T>.Index), "entfernen");
        return this;
    }

    // Zerstört die Entity nach den Skripten.
    public void Destroy() => Check(Api->Destroy(Id), "zerstören");

    public Entity Parent
    {
        get => new(Api->Parent(Id));
        set => Check(Api->SetParent(Id, value.Id), "umhängen");
    }

    public Entity[] Children
    {
        get
        {
            int count = Api->Children(Id, null, 0);
            if (count <= 0)
            {
                return Array.Empty<Entity>();
            }
            var ids = new ulong[count];
            fixed (ulong* data = ids)
            {
                count = Math.Min(count, Api->Children(Id, data, ids.Length));
            }
            var result = new Entity[count];
            for (int i = 0; i < count; ++i)
            {
                result[i] = new Entity(ids[i]);
            }
            return result;
        }
    }

    // Name aus der Komponente Name; leer ohne Namen. Setzen gilt nach den Skripten.
    public string Name
    {
        get
        {
            byte* text = null;
            int length = Api->Name(Id, &text);
            return length > 0 ? Encoding.UTF8.GetString(text, length) : "";
        }
        set
        {
            byte[] bytes = Encoding.UTF8.GetBytes(value);
            fixed (byte* text = bytes)
            {
                Check(Api->SetName(Id, text, bytes.Length), "umbenennen");
            }
        }
    }

    public bool Equals(Entity other) => Id == other.Id;
    public override bool Equals(object? obj) => obj is Entity other && Equals(other);
    public override int GetHashCode() => Id.GetHashCode();
    public static bool operator ==(Entity a, Entity b) => a.Id == b.Id;
    public static bool operator !=(Entity a, Entity b) => a.Id != b.Id;
    public override string ToString() => $"Entity {Id}";

    internal static WorldApi* Api
    {
        get
        {
            var api = Host.Api;
            if (api == null)
            {
                throw new InvalidOperationException("Die Engine ist nicht bereit");
            }
            return &api->World;
        }
    }

    private InvalidOperationException Missing<T>() where T : unmanaged, IComponent =>
        new($"{this} hat keine Komponente {T.TypeName} (oder lebt nicht)");

    private void Check(int result, string what)
    {
        if (result != 0)
        {
            throw new InvalidOperationException($"{this} lässt sich nicht {what} (lebt nicht?)");
        }
    }
}
