using System;
using System.Runtime.InteropServices;

namespace NGine;

// Mathe-Typen mit derselben Anordnung wie ngine::math. Von Hand geschrieben (Operatoren),
// geprüft wie die erzeugten Structs. System.Numerics hat kein DVec3; die Umwandlungen helfen,
// wo eine Bibliothek Vector3 oder Quaternion erwartet.

[StructLayout(LayoutKind.Sequential)]
public struct Vec2 : IComponent
{
    public static string TypeName => "Vec2";
    public static bool Trivial => true;
    public float X, Y;

    public Vec2(float x, float y) { X = x; Y = y; }
    public static Vec2 operator +(Vec2 a, Vec2 b) => new(a.X + b.X, a.Y + b.Y);
    public static Vec2 operator -(Vec2 a, Vec2 b) => new(a.X - b.X, a.Y - b.Y);
    public static Vec2 operator *(Vec2 a, float s) => new(a.X * s, a.Y * s);
    public readonly float Length => MathF.Sqrt(X * X + Y * Y);
    public override readonly string ToString() => $"({X}, {Y})";
}

[StructLayout(LayoutKind.Sequential)]
public struct Vec3 : IComponent
{
    public static string TypeName => "Vec3";
    public static bool Trivial => true;
    public float X, Y, Z;

    public Vec3(float x, float y, float z) { X = x; Y = y; Z = z; }
    public static readonly Vec3 Zero = new(0, 0, 0);
    public static readonly Vec3 Up = new(0, 1, 0);
    public static readonly Vec3 Forward = new(0, 0, -1);
    public static readonly Vec3 Right = new(1, 0, 0);

    public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vec3 operator -(Vec3 a, Vec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vec3 operator -(Vec3 a) => new(-a.X, -a.Y, -a.Z);
    public static Vec3 operator *(Vec3 a, float s) => new(a.X * s, a.Y * s, a.Z * s);
    public static Vec3 operator *(float s, Vec3 a) => a * s;
    public static Vec3 operator /(Vec3 a, float s) => new(a.X / s, a.Y / s, a.Z / s);
    public static float Dot(Vec3 a, Vec3 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    public static Vec3 Cross(Vec3 a, Vec3 b) =>
        new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    public readonly float Length => MathF.Sqrt(Dot(this, this));
    public readonly Vec3 Normalized => Length > 0 ? this / Length : Zero;

    public static implicit operator System.Numerics.Vector3(Vec3 v) => new(v.X, v.Y, v.Z);
    public static implicit operator Vec3(System.Numerics.Vector3 v) => new(v.X, v.Y, v.Z);
    public override readonly string ToString() => $"({X}, {Y}, {Z})";
}

[StructLayout(LayoutKind.Sequential)]
public struct Vec4 : IComponent
{
    public static string TypeName => "Vec4";
    public static bool Trivial => true;
    public float X, Y, Z, W;

    public Vec4(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
    public override readonly string ToString() => $"({X}, {Y}, {Z}, {W})";
}

// Position in der Welt in 64 Bit (ADR 0012).
[StructLayout(LayoutKind.Sequential)]
public struct DVec3 : IComponent
{
    public static string TypeName => "DVec3";
    public static bool Trivial => true;
    public double X, Y, Z;

    public DVec3(double x, double y, double z) { X = x; Y = y; Z = z; }
    public static DVec3 operator +(DVec3 a, DVec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static DVec3 operator -(DVec3 a, DVec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static DVec3 operator +(DVec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static DVec3 operator *(DVec3 a, double s) => new(a.X * s, a.Y * s, a.Z * s);
    public readonly double Length => Math.Sqrt(X * X + Y * Y + Z * Z);
    public static explicit operator Vec3(DVec3 v) => new((float)v.X, (float)v.Y, (float)v.Z);
    public static implicit operator DVec3(Vec3 v) => new(v.X, v.Y, v.Z);
    public override readonly string ToString() => $"({X}, {Y}, {Z})";
}

// Drehung als Einheitsquaternion (x, y, z, w) wie ngine::math::Quat.
[StructLayout(LayoutKind.Sequential)]
public struct Quat : IComponent
{
    public static string TypeName => "Quat";
    public static bool Trivial => true;
    public float X, Y, Z, W;

    public Quat(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
    public static readonly Quat Identity = new(0, 0, 0, 1);

    public static Quat AngleAxis(float angle, Vec3 axis)
    {
        Vec3 n = axis.Normalized;
        float s = MathF.Sin(angle * 0.5f);
        return new(n.X * s, n.Y * s, n.Z * s, MathF.Cos(angle * 0.5f));
    }

    // Hamilton-Produkt: erst b, dann a drehen.
    public static Quat operator *(Quat a, Quat b) => new(
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);

    public static Vec3 operator *(Quat q, Vec3 v)
    {
        var u = new Vec3(q.X, q.Y, q.Z);
        Vec3 t = 2.0f * Vec3.Cross(u, v);
        return v + q.W * t + Vec3.Cross(u, t);
    }

    public static implicit operator System.Numerics.Quaternion(Quat q) => new(q.X, q.Y, q.Z, q.W);
    public static implicit operator Quat(System.Numerics.Quaternion q) => new(q.X, q.Y, q.Z, q.W);
    public override readonly string ToString() => $"({X}, {Y}, {Z}, {W})";
}
