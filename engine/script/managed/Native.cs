using System.Runtime.InteropServices;

namespace NGine;

// Funktionen der Engine, die C# aufrufen darf. Der C++-Host füllt die Tabelle und übergibt sie an
// Host.Initialize. Aufbau und Reihenfolge müssen zu NativeApi in engine/script/runtime.cpp passen.
[StructLayout(LayoutKind.Sequential)]
public unsafe struct NativeApi
{
    // Ebene wie ngine::log::Level, Text als UTF-8 ohne Null am Ende.
    public delegate* unmanaged<int, byte*, int, void> Log;
    // Nur für die Messung der Aufrufkosten von C# nach C++.
    public delegate* unmanaged<int, int, int> Add;
    // Welt: Entities, Komponenten, Abfragen (R4.4).
    public WorldApi World;
    // Skript-Komponenten (R4.4).
    public ScriptApi Script;
}

// Funktionen der Welt; Aufbau wie WorldApi in engine/script/world_api.cppm. Komponenten heißen
// über den Index ihres Typs in der Registry, -1 ist unbekannt.
[StructLayout(LayoutKind.Sequential)]
public unsafe struct WorldApi
{
    public delegate* unmanaged<byte*, int, int> TypeIndex;
    public delegate* unmanaged<ulong, int, int, void*> Get;
    public delegate* unmanaged<ulong, int, int> Has;
    public delegate* unmanaged<ulong, int, void*, int> Set;
    public delegate* unmanaged<ulong, int, int> Remove;
    public delegate* unmanaged<byte*, int, ulong> Create;
    public delegate* unmanaged<ulong, int> Destroy;
    public delegate* unmanaged<ulong, int> Alive;
    public delegate* unmanaged<ulong, ulong> Parent;
    public delegate* unmanaged<ulong, ulong, int> SetParent;
    public delegate* unmanaged<ulong, ulong*, int, int> Children;
    public delegate* unmanaged<ulong, byte**, int> Name;
    public delegate* unmanaged<ulong, byte*, int, int> SetName;
    public delegate* unmanaged<byte*, int, ulong> Find;
    public delegate* unmanaged<int*, int, NativeChunk**, int> Query;
}

// Ein Block von Entities mit denselben Komponenten (NativeChunk in world_api.cppm).
[StructLayout(LayoutKind.Sequential)]
public unsafe struct NativeChunk
{
    public const int MaxTerms = 16;
    public uint Count;
    public uint Reserved;
    public ulong* Entities;
    public fixed ulong Columns[MaxTerms];
}

// Position wie ngine::script::Position (drei float). Daten, die sich C++ und C# teilen, müssen
// blittable sein: gleiches Layout auf beiden Seiten, keine Verweise.
[StructLayout(LayoutKind.Sequential)]
public struct Position
{
    public float X;
    public float Y;
    public float Z;
}
