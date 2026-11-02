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
