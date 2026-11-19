using System;
using NGine;

namespace Spiel;

// Figur, die im Kreis geht, rennt oder steht (R4.6, Spielplatz): bewegt ihren CharacterController
// auf einem Kreis um den Punkt Radius Meter links vom Start und setzt dabei die Parameter ihres
// Animators wie Spielersteuerung.cs. So sieht man Gehen, Laufen und Stehen nebeneinander.
public class Spaziergang : Script
{
    [Range(0, 20)] public float Radius = 2.0f;
    // Gewünschte Geschwindigkeit in m/s; 0 steht.
    [Range(0, 8)] public float Tempo = 1.6f;
    // 1 gegen den Uhrzeigersinn (von oben), -1 mit ihm.
    [Range(-1, 1)] public float Richtung = 1.0f;
    [Range(0, 30)] public float Glaettung = 10.0f;

    private DVec3 center;
    private float tempo;

    public override void Start()
    {
        center = Read<Transform>().Position - new Vec3(Radius, 0.0f, 0.0f);
    }

    public override void FixedUpdate()
    {
        if (!Entity.Has<CharacterMotion>() || !Entity.TryRead(out CharacterController controller))
        {
            return;
        }
        ref CharacterMotion motion = ref Get<CharacterMotion>();
        if (Tempo <= 0.0f || Radius <= 0.0f)
        {
            motion.Move = Vec3.Zero;
            return;
        }
        DVec3 offset = Read<Transform>().Position - center;
        var radial = new Vec3((float)offset.X, 0.0f, (float)offset.Z);
        float distance = radial.Length;
        Vec3 outward = distance > 0.001f ? radial * (1.0f / distance) : Vec3.Right;
        // Tangente plus Zug zurück auf den Kreis.
        Vec3 tangent = new Vec3(-outward.Z, 0.0f, outward.X) * Richtung;
        Vec3 direction = tangent + outward * Math.Clamp(Radius - distance, -1.0f, 1.0f);
        direction = direction.Length > 0.001f ? direction.Normalized : tangent;
        bool run = Tempo > controller.WalkSpeed;
        float top = run ? controller.RunSpeed : controller.WalkSpeed;
        motion.Move = direction * Math.Clamp(Tempo / MathF.Max(top, 0.1f), 0.0f, 1.0f);
        motion.Run = run;
        // Blick in Laufrichtung (die Figur schaut entlang -Z).
        Get<Transform>().Rotation = Quat.AngleAxis(MathF.Atan2(-direction.X, -direction.Z), Vec3.Up);
    }

    public override void Update()
    {
        if (!Entity.TryRead(out CharacterMotion motion))
        {
            return;
        }
        Vec3 velocity = motion.Velocity;
        float speed = MathF.Sqrt(velocity.X * velocity.X + velocity.Z * velocity.Z);
        tempo += (speed - tempo) * MathF.Min(Glaettung * Time.Delta, 1.0f);
        Entity.SetAnimFloat("Tempo", tempo);
        Entity.SetAnimBool("AmBoden", motion.Ground != GroundState.Air);
        Entity.SetAnimBool("Geduckt", motion.Crouched);
    }
}
