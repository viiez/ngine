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

// Prüft die Welt-API aus C# (R4.4); C++ ruft die Methoden über ScriptRuntime::invoke auf.
public static class WorldTests
{
    // Verschiebt jede Entity mit Transform um 1 m nach x und zählt sie in Name der Entity "Zähler".
    public static void MoveAll()
    {
        int count = 0;
        foreach (var row in World.Query<Transform>())
        {
            row.Item1.Position.X += 1.0;
            ++count;
        }
        Log.Info($"WorldTests: {count} Entities verschoben");
    }

    // Liest und schreibt über Entity, legt eine neue Entity mit Komponenten an.
    public static void EditOne()
    {
        Entity first = World.Find("Erste");
        if (!first.Alive)
        {
            throw new System.InvalidOperationException("Erste fehlt");
        }
        ref Transform transform = ref first.Get<Transform>();
        transform.Scale = new Vec3(2, 2, 2);
        if (first.Has<RigidBody>())
        {
            throw new System.InvalidOperationException("Erste hat keinen RigidBody");
        }
        first.Set(new RigidBody { Mass = 5.0f });
        Log.Info($"WorldTests: Name {first.Name}, Position {first.Read<Transform>().Position}");

        Entity created = World.Create("AusCSharp");
        created.Set(new Transform { Position = new DVec3(7, 8, 9), Rotation = Quat.Identity, Scale = new Vec3(1, 1, 1) });
        created.Parent = first;
        if (created.Alive)
        {
            throw new System.InvalidOperationException("Neue Entities leben erst nach den Skripten");
        }
    }

    // Abfrage über zwei Komponenten und Entfernen über den Befehlspuffer.
    public static void RemoveBodies()
    {
        foreach (var row in World.Query<Transform, RigidBody>())
        {
            row.Item2.Mass += 1.0f;
            row.Entity.Remove<RigidBody>();
        }
    }

    public static void Throws() => throw new System.InvalidOperationException("Absichtlich");
}
