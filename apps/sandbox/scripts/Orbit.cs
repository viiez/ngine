using System;
using NGine;

namespace Sandbox.Scripts;

// Lässt alle Positionen auf Kreisen um den Ursprung laufen und meldet sich regelmäßig.
public class Orbit : IScript
{
    private float time;
    private int frames;

    public void Start() => Log.Info("Hallo aus C#: Orbit startet");

    public void Update(in Frame frame)
    {
        time += frame.DeltaTime;
        var positions = frame.Positions;
        for (int i = 0; i < positions.Length; ++i)
        {
            float radius = 1.0f + i * 0.01f;
            float angle = time * (1.0f + i * 0.001f);
            positions[i].X = MathF.Cos(angle) * radius;
            positions[i].Z = MathF.Sin(angle) * radius;
        }
        if (++frames % 60 == 0)
        {
            Log.Info($"Orbit: {frames} Frames, {positions.Length} Positionen, Zeit {time:F2} s");
        }
    }

    public void Stop() => Log.Info($"Orbit endet nach {frames} Frames");
}
