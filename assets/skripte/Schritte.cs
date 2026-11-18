using System;
using NGine;

namespace Spiel;

// Schritte der Spielfigur (R4.5): Bei jedem Schritt (OnStep) und beim Landen spielt das Skript den
// Klang des Bodens. Auf dem Terrain kommt er aus der Materialtabelle (footstep_sounds in
// terrain/standard.terrain-materials), auf Körpern wie Kisten und der Ebene aus AufKoerpern.
public class Schritte : Script
{
    [Range(0, 2)] public float Gehen = 0.5f;
    [Range(0, 2)] public float Rennen = 0.9f;
    // Klang oder Klangsatz für Schritte auf Körpern (Material 0).
    [Asset] public ulong AufKoerpern;
    // Ab dieser Fallhöhe in Metern klingt das Landen wie zwei Schritte zugleich.
    [Range(0, 5)] public float Landehoehe = 0.6f;

    public override void OnStep(DVec3 position, uint material, bool run)
    {
        Play(position, material, run ? Rennen : Gehen);
    }

    public override void OnLanded(float height, uint material)
    {
        if (height < Landehoehe)
        {
            return;
        }
        float volume = MathF.Min(Rennen + height * 0.1f, 1.5f);
        Play(Get<Transform>().Position, material, volume);
    }

    private void Play(DVec3 position, uint material, float volume)
    {
        ulong sound = material != 0 ? Terrains.FootstepSound(material) : AufKoerpern;
        if (sound != 0)
        {
            Audio.Play(sound, position, AudioBus.Effects, volume);
        }
    }
}
