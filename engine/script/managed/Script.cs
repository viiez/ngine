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
