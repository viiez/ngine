using System;
using NGine;

namespace Spiel;

// Steuerung der Spielfigur (R4.4; bis R4.3 apply_controls() in apps/common/player.cppm). Blick,
// Kamera und Graben pro Bild in Update, die Bewegung im festen Schritt. Die Bewegung rechnet die
// Physik (CharacterController), die Kamera bleibt in C++ (player_view()) und liest PlayerInput.
// Seit R4.6 setzt das Skript auch die Parameter der Animation.
public class Spielersteuerung : Script
{
    // Pause zwischen zwei Pinselstrichen beim gehaltenen Graben, in Sekunden.
    [Range(0, 2)] public float Grabpause = 0.15f;
    // Wie schnell Tempo der Geschwindigkeit folgt (1/s); höher heißt härter (R4.6).
    [Range(0, 30)] public float Glaettung = 10.0f;

    private float pause;
    private float tempo;

    public override void Update()
    {
        if (!Entity.Has<PlayerInput>())
        {
            return;
        }
        ref PlayerInput input = ref Get<PlayerInput>();
        Vec2 look = Input.Value("look");
        input.Yaw -= look.X * input.LookSpeed;
        input.Pitch = Math.Clamp(input.Pitch - look.Y * input.LookSpeed, -1.55f, 1.55f);
        if (Input.Pressed("camera"))
        {
            input.View = input.View == CameraView.FirstPerson ? CameraView.Shoulder : CameraView.FirstPerson;
        }
        Get<Transform>().Rotation = Quat.AngleAxis(input.Yaw, Vec3.Up);

        pause = MathF.Max(pause - Time.Delta, 0.0f);
        if (Input.Down("dig") && pause <= 0.0f && Dig(input, add: Input.Down("fast")))
        {
            pause = Grabpause;
        }
        Animate();
    }

    // Parameter des Automaten (figuren/spielfigur.animgraph) aus dem Zustand der Physik. Ohne
    // Animator an der Figur oder darunter tut das nichts.
    private void Animate()
    {
        if (!Entity.TryRead(out CharacterMotion motion))
        {
            return;
        }
        Vec3 velocity = motion.Velocity;
        float speed = MathF.Sqrt(velocity.X * velocity.X + velocity.Z * velocity.Z);
        tempo += (speed - tempo) * MathF.Min(Glaettung * Time.Delta, 1.0f);
        bool grounded = motion.Ground != GroundState.Air;
        Entity.SetAnimFloat("Tempo", tempo);
        Entity.SetAnimBool("AmBoden", grounded);
        Entity.SetAnimBool("Geduckt", motion.Crouched);
        if (grounded && Input.Pressed("jump"))
        {
            Entity.SetAnimTrigger("Springen");
        }
    }

    public override void FixedUpdate()
    {
        if (!Entity.Has<PlayerInput>() || !Entity.Has<CharacterMotion>())
        {
            // Den Zustand der Bewegung legt die Physik im ersten Schritt an.
            return;
        }
        Quat facing = Quat.AngleAxis(Read<PlayerInput>().Yaw, Vec3.Up);
        Vec2 stick = Input.Value("move");
        Vec3 move = facing * Vec3.Forward * stick.Y + facing * Vec3.Right * stick.X;
        move.Y = 0.0f;
        if (move.Length > 1.0f)
        {
            move = move.Normalized;
        }
        ref CharacterMotion motion = ref Get<CharacterMotion>();
        motion.Move = move;
        motion.Run = Input.Down("fast");
        motion.Crouch = Input.Down("crouch");
        // Bleibt gesetzt, bis die Physik den Sprung verbraucht.
        motion.Jump = motion.Jump || Input.Pressed("jump");
    }

    // Pinsel aus R3.4 dort, wo der Blick aus dem Auge das Terrain trifft; mit add auftragen.
    private bool Dig(in PlayerInput input, bool add)
    {
        float eye = input.EyeHeight;
        if (Entity.TryRead(out CharacterController controller) && Entity.TryRead(out CharacterMotion motion) &&
            motion.Crouched && controller.Height > 0.0f)
        {
            eye *= controller.CrouchHeight / controller.Height;
        }
        DVec3 origin = Read<Transform>().Position + new Vec3(0.0f, eye, 0.0f);
        Quat view = Quat.AngleAxis(input.Yaw, Vec3.Up) * Quat.AngleAxis(input.Pitch, Vec3.Right);
        RayHit? hit = Physics.Raycast(origin, view * Vec3.Forward, input.DigReach, new QueryFilter { Ignore = Entity });
        if (hit is not RayHit target || !target.Entity.Has<Terrain>())
        {
            return false;
        }
        Terrains.Edit(target.Entity, target.Point, input.DigRadius, add);
        return true;
    }
}
