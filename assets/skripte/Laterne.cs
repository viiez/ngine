using System;
using System.Collections.Generic;
using NGine;

namespace Spiel;

// Laterne (R4.4): Die Aktion light (F, am Gamepad Y bzw. Dreieck) schaltet das Punktlicht der
// nächsten Laterne in Reichweite der Spielfigur an und aus; an flackert es leicht. Das Licht ist
// die Laterne selbst oder ein Kind mit PointLight (prefabs/laterne.prefab: „Leuchte“).
public class Laterne : Script
{
    public bool An = true;
    [Range(0, 1)] public float Flackern = 0.15f;
    // Abstand zur Spielfigur, ab dem light wirkt, in Metern.
    [Range(0, 20)] public float Reichweite = 4.0f;

    // Alle laufenden Laternen, damit ein Druck nur die nächste schaltet.
    private static readonly List<Laterne> running = new();

    private Entity light;
    private float intensity;
    private float time;

    public override void Start()
    {
        running.Add(this);
        light = FindLight(Entity);
        if (light != Entity.None)
        {
            intensity = light.Read<PointLight>().Intensity;
        }
        Apply();
    }

    public override void Update()
    {
        if (Input.Pressed("light") && IsNearest())
        {
            An = !An;
            Log.Info($"{Entity.Name}: {(An ? "an" : "aus")}");
        }
        time += Time.Delta;
        Apply();
    }

    public override void Stop()
    {
        running.Remove(this);
        // Ruhiges Licht wie im Prefab, wenn das Skript endet.
        if (light.Alive && light.Has<PointLight>())
        {
            light.Get<PointLight>().Intensity = intensity;
        }
    }

    private void Apply()
    {
        if (!light.Alive || !light.Has<PointLight>())
        {
            return;
        }
        ref PointLight point = ref light.Get<PointLight>();
        point.Active = An;
        // Zwei Sinusse mit krummem Verhältnis wirken unregelmäßig und bleiben reproduzierbar.
        float wobble = 0.6f * MathF.Sin(time * 7.3f) + 0.4f * MathF.Sin(time * 13.1f + 1.7f);
        point.Intensity = intensity * (1.0f + Flackern * wobble);
    }

    // Ob diese Laterne die nächste in Reichweite einer Spielfigur ist; so schaltet ein Druck nur eine.
    private bool IsNearest()
    {
        DVec3 position = Read<Transform>().Position;
        foreach (var player in World.Query<PlayerInput, Transform>())
        {
            DVec3 at = player.Item2.Position;
            double distance = (at - position).Length;
            if (distance > Reichweite)
            {
                continue;
            }
            foreach (Laterne other in running)
            {
                if (other != this && other.Entity.Alive &&
                    (at - other.Read<Transform>().Position).Length < distance)
                {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    private static Entity FindLight(Entity entity)
    {
        if (entity.Has<PointLight>())
        {
            return entity;
        }
        foreach (Entity child in entity.Children)
        {
            Entity found = FindLight(child);
            if (found != Entity.None)
            {
                return found;
            }
        }
        return Entity.None;
    }
}
