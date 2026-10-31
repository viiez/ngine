# A2: Fenster und Vulkan-Dreieck

- **Phase:** Ausarbeitung
- **Status:** abgeschlossen
- **Pull Requests:** #3, #4

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

**Ergebnis:** Die Sandbox öffnet ein SDL3-Fenster und zeichnet mit Vulkan ein Dreieck. Es gibt eine
erste RHI (`ngine.rhi`) mit Vulkan-Backend (volk, VMA), Slang-Shader, die beim Bauen eingebettet
werden, und einen Offscreen-Test, der Pixel prüft. Die CI testet unter Linux mit lavapipe und
Validation Layers, auch mit Fenster unter Xvfb. Auf der RTX 3060 Ti unter Windows lief PR #3 ohne
Probleme. Mit PR #4 setzt die Engine eine moderne GPU voraus (Vulkan 1.4, Mesh Shader, Raytracing).
Alle Abnahmekriterien sind erfüllt; Linux auf echter GPU testen wir, sobald der PC Linux hat.

**Erkenntnisse:**

- Slang übersetzt `SV_VertexID` mit der SPIR-V-Fähigkeit DrawParameters; das Feature
  `shaderDrawParameters` muss eingeschaltet sein.
- ctest gibt Testnamen mit Umlauten unter Windows falsch kodiert weiter; Testnamen nur in ASCII.
- Clang 18 übersetzt `std::expected` aus libstdc++ nicht; Mindestversion ist Clang 20, wir nutzen 23.
- Der Vulkan-Loader 1.3 aus Ubuntu 24.04 reicht für Vulkan 1.4, weil Device-Funktionen vom Treiber
  kommen.
- lavapipe kann Vulkan 1.4, Mesh Shader und Raytracing. Die CI prüft damit die volle
  Mindestanforderung.
- GPU-Tests, die ohne Gerät übersprungen werden, ergeben einen grünen Lauf ohne Aussage. In der CI
  schlagen sie deshalb jetzt fehl (`NGINE_REQUIRE_GPU`).

**Anpassungen:** ADR 0013 (RHI-Aufbau), ADR 0014 (moderne GPU, ersetzt Teile von ADR 0004),
Clang 23 und der Grundsatz „aktuelle Werkzeuge“ (ADR 0002, 0008), Einrichtungsanleitung
[setup.md](../setup.md), Vision mit den Leitprinzipien „Seriös und performant“ und „Sauber statt
schnell fertig“, Architektur-Review und Schuldenregister im [Prozess](../process.md).

**Architektur-Review:**

- Die Abhängigkeiten stimmen: `ngine.platform` und `ngine.rhi` kennen sich nicht, die Sandbox verbindet
  sie über `SurfaceDesc`. SDL3 steckt nur in `window_sdl.cpp`, Vulkan, volk und VMA nur in
  `ngine.rhi.vulkan`.
- Die RHI ist bewusst klein und reicht für das Dreieck. Für den Renderer in R1 fehlen Puffer,
  Descriptor-Verwaltung (Bindless) und Shader-Reflection; das ist geplant, keine Schuld.
- `engine/rhi/vulkan/device.cpp` ist mit über 1300 Zeilen zu groß und mischt Instanz, Geräteauswahl,
  Swapchain, Ressourcen und Command Lists: TD-01, Abbau zu Beginn von A3.
- Weitere Schulden: TD-02 bis TD-05 im [Schuldenregister](../tech-debt.md).
