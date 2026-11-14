using System;
using System.Runtime.InteropServices;
using System.Text;

namespace NGine;

// Dienste der Engine für Skripte; Aufbau wie GameApi in engine/script/game_api.cppm.
[StructLayout(LayoutKind.Sequential)]
public unsafe struct GameApi
{
    public delegate* unmanaged<byte*, int, NativeAction*, int> Action;
    public delegate* unmanaged<double*, float*, float, NativeFilter*, NativeHit*, int> Raycast;
    public delegate* unmanaged<NativeShape*, double*, float*, float*, float, NativeFilter*, NativeHit*, int> ShapeCast;
    public delegate* unmanaged<NativeShape*, double*, float*, NativeFilter*, ulong*, int, int> Overlap;
}

[StructLayout(LayoutKind.Sequential)]
public struct NativeAction
{
    public float X, Y;
    public byte Down, Pressed, Released, Reserved;
}

[StructLayout(LayoutKind.Sequential)]
public struct NativeFilter
{
    public uint Layers;
    public uint Sensors;
    public ulong Ignore;
}

[StructLayout(LayoutKind.Sequential)]
public struct NativeHit
{
    public ulong Entity;
    public DVec3 Point;
    public Vec3 Normal;
    public float Distance;
    public uint Material;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential)]
public struct NativeShape
{
    public int Shape;
    public float Radius;
    public Vec3 HalfExtents;
    public float HalfHeight;
}

// Aktionen der Eingabe (assets/eingabe/*.input). Pressed und Released gelten für ein Bild; in
// FixedUpdate besser Down lesen oder den Druck in Update merken.
public static unsafe class Input
{
    public static bool Down(string action) => Get(action).Down != 0;
    public static bool Pressed(string action) => Get(action).Pressed != 0;
    public static bool Released(string action) => Get(action).Released != 0;
    // Achse (x) bzw. zwei Achsen (x, y), z. B. "move".
    public static Vec2 Value(string action)
    {
        NativeAction value = Get(action);
        return new Vec2(value.X, value.Y);
    }

    private static NativeAction Get(string action)
    {
        var api = Host.Api;
        NativeAction value = default;
        if (api == null)
        {
            return value;
        }
        Span<byte> buffer = stackalloc byte[Encoding.UTF8.GetMaxByteCount(action.Length)];
        int length = Encoding.UTF8.GetBytes(action, buffer);
        fixed (byte* text = buffer)
        {
            api->Game.Action(text, length, &value);
        }
        return value;
    }
}

// Treffer eines Strahls oder einer Form.
public readonly struct RayHit
{
    // Bei Treffern auf dem Terrain das Terrain.
    public Entity Entity { get; init; }
    public DVec3 Point { get; init; }
    public Vec3 Normal { get; init; }
    public float Distance { get; init; }
    // Material des Terrains am Treffer (Nummer der Materialtabelle), sonst 0.
    public uint Material { get; init; }

    internal static RayHit From(in NativeHit hit) => new()
    {
        Entity = new Entity(hit.Entity),
        Point = hit.Point,
        Normal = hit.Normal,
        Distance = hit.Distance,
        Material = hit.Material,
    };
}

// Welche Körper eine Abfrage sieht.
public struct QueryFilter
{
    public QueryFilter() { }
    public uint Layers = uint.MaxValue;
    public Entity Ignore;
    public bool Sensors;

    internal readonly NativeFilter Native => new()
    {
        Layers = Layers,
        Sensors = Sensors ? 1u : 0u,
        Ignore = Ignore.Id,
    };
}

// Form für ShapeCast und Overlap.
public struct QueryShape
{
    public ShapeType Shape;
    public float Radius;
    public Vec3 HalfExtents;
    public float HalfHeight;

    public static QueryShape Sphere(float radius) => new() { Shape = ShapeType.Sphere, Radius = radius };
    public static QueryShape Box(Vec3 halfExtents) => new() { Shape = ShapeType.Box, HalfExtents = halfExtents };
    public static QueryShape Capsule(float radius, float halfHeight) =>
        new() { Shape = ShapeType.Capsule, Radius = radius, HalfHeight = halfHeight };

    internal readonly NativeShape Native => new()
    {
        Shape = (int)Shape,
        Radius = Radius,
        HalfExtents = HalfExtents,
        HalfHeight = HalfHeight,
    };
}

// Abfragen der Physik (ngine::physics::PhysicsWorld).
public static unsafe class Physics
{
    public static RayHit? Raycast(DVec3 origin, Vec3 direction, float maxDistance, QueryFilter filter = default)
    {
        filter = Normalize(filter);
        NativeFilter native = filter.Native;
        NativeHit hit;
        return Api->Raycast((double*)&origin, (float*)&direction, maxDistance, &native, &hit) != 0
            ? RayHit.From(hit)
            : null;
    }

    public static RayHit? ShapeCast(QueryShape shape, DVec3 origin, Quat rotation, Vec3 direction,
                                    float maxDistance, QueryFilter filter = default)
    {
        filter = Normalize(filter);
        NativeFilter native = filter.Native;
        NativeShape s = shape.Native;
        NativeHit hit;
        return Api->ShapeCast(&s, (double*)&origin, (float*)&rotation, (float*)&direction, maxDistance, &native, &hit) != 0
            ? RayHit.From(hit)
            : null;
    }

    public static Entity[] Overlap(QueryShape shape, DVec3 position, Quat rotation, QueryFilter filter = default)
    {
        filter = Normalize(filter);
        NativeFilter native = filter.Native;
        NativeShape s = shape.Native;
        Span<ulong> buffer = stackalloc ulong[64];
        int count;
        fixed (ulong* data = buffer)
        {
            count = Api->Overlap(&s, (double*)&position, (float*)&rotation, &native, data, buffer.Length);
        }
        ulong[] ids = count <= buffer.Length ? buffer[..count].ToArray() : new ulong[count];
        if (count > buffer.Length)
        {
            fixed (ulong* data = ids)
            {
                count = Math.Min(count, Api->Overlap(&s, (double*)&position, (float*)&rotation, &native, data, ids.Length));
            }
        }
        var result = new Entity[count];
        for (int i = 0; i < count; ++i)
        {
            result[i] = new Entity(ids[i]);
        }
        return result;
    }

    // default(QueryFilter) hat Layers = 0; gemeint sind dann alle Ebenen.
    private static QueryFilter Normalize(QueryFilter filter)
    {
        if (filter.Layers == 0)
        {
            filter.Layers = uint.MaxValue;
        }
        return filter;
    }

    private static GameApi* Api
    {
        get
        {
            var api = Host.Api;
            if (api == null)
            {
                throw new InvalidOperationException("Die Engine ist nicht bereit");
            }
            return &api->Game;
        }
    }
}

// Ereignisse der Physik für Skripte; Werte wie EventKind in game_api.cppm.
public enum EventKind
{
    ContactBegan,
    ContactEnded,
    TriggerEntered,
    TriggerLeft,
    Landed,
    Step,
}

[StructLayout(LayoutKind.Sequential)]
public struct NativeEvent
{
    public EventKind Kind;
    public uint Material;
    public ulong A;
    public ulong B;
    public DVec3 Point;
    public Vec3 Normal;
    public float Value;
}

// Berührung mit einem anderen Körper.
public readonly struct Contact
{
    public Entity Other { get; init; }
    public DVec3 Point { get; init; }
    // Zeigt von diesem Skript zum anderen Körper.
    public Vec3 Normal { get; init; }
}
