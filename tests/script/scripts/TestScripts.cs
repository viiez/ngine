using NGine;

namespace Tests.Scripts;

// Meldet sich beim Start über das Log der Engine.
public class Greeter : IScript
{
    public void Start() => Log.Info("Hallo aus C#");
    public void Stop() => Log.Info("Tschüss aus C#");
}

// Hebt jede Position pro Sekunde um eine Einheit an und verschiebt sie um ihren Index nach x.
public class Mover : IScript
{
    public void Update(in Frame frame)
    {
        var positions = frame.Positions;
        for (int i = 0; i < positions.Length; ++i)
        {
            positions[i].X += i;
            positions[i].Y += frame.DeltaTime;
        }
    }
}

// Abstrakte Klassen sind keine Skripte.
public abstract class NotAScript : IScript
{
}
