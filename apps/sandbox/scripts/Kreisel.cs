using NGine;

namespace Sandbox.Scripts;

// Skript-Komponente für die Messung mit vielen Skripten (--scripts n, R4.4): dreht ihre Entity in
// jedem Bild ein Stück um die Hochachse.
public class Kreisel : Script
{
    public float Geschwindigkeit = 1.0f;

    public override void Update()
    {
        ref Transform transform = ref Get<Transform>();
        transform.Rotation = Quat.AngleAxis(Geschwindigkeit * Time.Delta, Vec3.Up) * transform.Rotation;
    }
}
