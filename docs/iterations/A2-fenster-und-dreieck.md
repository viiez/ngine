# A2: Fenster und Vulkan-Dreieck

- **Phase:** Ausarbeitung
- **Status:** in Arbeit
- **Pull Requests:** –

## Plan

**Ziel:** Die Sandbox öffnet ein Fenster und zeichnet mit Vulkan ein Dreieck, auf der RTX unter
Windows und unter Linux (CI mit lavapipe).

**Features:** PLAT-01, PLAT-02 (Grundlage), PLAT-03, REND-01, REND-02, REND-03, REND-04, CORE-04,
CORE-10

**Risiken:**

- Vulkan-Fehler, die nur auf echter Hardware auftreten; in der CI gibt es keine GPU.
- Die RHI-Schnittstelle wird zu früh zu groß oder zu sehr an Vulkan gebunden.
- Slang in vcpkg und unter beiden Plattformen einzubinden, kostet mehr Zeit als gedacht.

**Abnahme:**

- Dreieck sichtbar unter Windows (RTX) und Linux (lavapipe), Fenster lässt sich verkleinern und schließen.
- Validation Layers melden keine Fehler.
- `--headless` startet weiterhin ohne GPU; CI bleibt grün.

## Stand

- Umgesetzt: `ngine.platform` (SDL3), `ngine.rhi`, `ngine.rhi.vulkan`, `ngine.shaders` (Slang),
  `ngine::verify()`, Sandbox mit Dreieck.
- In der CI: Offscreen-Test des Dreiecks und 60 Frames mit Fenster unter Xvfb, beides über lavapipe
  mit Validation Layers.
- Offen: Test auf echter Hardware (RTX unter Windows).
- Abweichung vom Plan: Statt Vulkan 1.3 mit Feature-Stufen setzt die Engine jetzt eine moderne GPU
  voraus (Vulkan 1.4, Mesh Shader, Raytracing, [ADR 0014](../adr/0014-modern-gpu-baseline.md)). Das
  Intel-Notebook ist damit keine Testplattform mehr.
- Abweichung vom Plan: CORE-10 (Mathematik) ist für das Dreieck nicht nötig und wandert nach R1.

## Review

Folgt nach dem Test auf echter Hardware.
