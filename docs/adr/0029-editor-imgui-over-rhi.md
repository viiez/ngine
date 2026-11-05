# 0029: Editor als eigene Anwendung, Dear ImGui hinter `ngine.editor_ui`, gezeichnet über die RHI

- **Status:** vorgeschlagen
- **Datum:** 2026-11-05

## Kontext

R2 bringt einen eigenen Editor ([R2](../iterations/R2-welt-und-editor.md)). Für Engine-Werkzeuge ist
Dear ImGui der Standard ([third-party.md](../third-party.md)): schnell gebaut, Docking im
Docking-Zweig, viel Erfahrung im Netz. ImGui bringt eigene Backends für SDL3 und Vulkan mit. Die
Engine hat aber zwei Regeln, die dagegen stehen: Fremdbibliotheken nur hinter einer
Engine-Schnittstelle ([ADR 0009](0009-third-party-behind-interfaces.md)), und nur die RHI kennt
Vulkan ([ADR 0013](0013-rhi-design.md)). Dazu kommt, dass ImGui aus Makros und Headern besteht und
die Engine C++-Module nutzt.

## Entscheidung

- Der Editor ist eine eigene Anwendung (`apps/editor/`) neben der Sandbox. Er braucht ein Fenster
  und wird ohne `NGINE_WITH_WINDOW` nicht gebaut. Was beide zeigen (die Standardszene), liegt in
  `apps/common/`.
- **`ngine.editor_ui`** kapselt ImGui. Nur `context.cpp` und `widgets.cpp` binden ImGui im globalen
  Modulfragment ein. Exportiert werden reine Engine-Typen:
  - `:draw`: die Zeichenliste (`UiVertex` mit demselben Aufbau wie `ImDrawVert`, `UiCommand` mit
    Beschneidung in Pixeln und Textur, Indizes als `u32` schon über alle Fenster verschoben) und
    `TextureHost` für die Texturen, die die UI selbst braucht.
  - `:context`: ein ImGui-Kontext mit Docking; Eingaben in eigenen Typen (`Key`, `MouseButton`),
    Zwischenablage über Rückrufe, Mauszeiger als Wunsch (`Cursor`).
  - `:widgets`: eine schmale Schicht im Stil von ImGui (Fenster, Menüs, Text, Bild, Eingabefelder,
    Andocken mit Standardanordnung). Keine eigene UI-Bibliothek: Was der Editor braucht, kommt
    als dünne Funktion dazu.
- **Zeichnen über die RHI:** `UiRenderer` ist ein Pass im Render-Graph mit eigenem Shader
  (`ui.slang`). Eckpunkte liest der Shader über die Geräteadresse ([ADR 0022](0022-vertex-pulling-bda.md)),
  Texturen kommen bindlos ([ADR 0026](0026-render-graph-bindless.md)), jeder Draw bekommt einen
  Scissor (dafür neu: `CommandList::set_scissor()`). Weder das Vulkan- noch das SDL-Backend von
  ImGui wird benutzt; Eingaben kommen aus `platform::Window`.
- **Texturen:** Der Pass erfüllt `ImGuiBackendFlags_RendererHasTextures` als `TextureHost`. Die
  Kennung einer Textur in der UI ist ihr bindloser Index plus eins (0 heißt: keine). Ändert ImGui
  den Schriftatlas, entsteht eine neue Textur, die alte geht über die Löschliste.
- **Viewport:** Der Renderer zeichnet in eine dauerhafte sRGB-Textur, die das Viewport-Fenster mit
  `image()` zeigt. Ihre Größe folgt dem Fenster mit einem Frame Verzug, weil die Zeichenliste eines
  Frames auf die Textur zeigt, die beim Aufbau der UI galt.
- Nur Docking im Hauptfenster; mehrere Viewports von ImGui (eigene Fenster für Panels) bleiben aus.

## Konsequenzen

- Die Regeln aus ADR 0009 und ADR 0013 gelten weiter: Ein späteres D3D12-Backend der RHI zeichnet
  die UI ohne Änderung, und ImGui ließe sich hinter `ngine.editor_ui` ersetzen.
- `ngine.editor_ui` braucht weder Fenster noch GPU: Kontext, Zeichenliste, Eingaben, Andocken,
  Konsole und Viewport-Größe sind ohne beides getestet. Der Pass hat einen GPU-Test über lavapipe.
- Die Schicht in `:widgets` wächst mit jedem Editor-Fenster. Sie bleibt bewusst dünn und eng an
  ImGui; eine Abstraktion für eine zweite UI-Bibliothek wäre ohne Bedarf.
- ImGui-Farben sind sRGB-kodiert. Der Shader rechnet sie in linear um und blendet linear; das sieht
  etwas anders aus als die Referenz-Backends, passt aber zu sRGB-Swapchains und zur Engine.
- Eigene Fenster für Panels außerhalb des Hauptfensters brauchten mehrere Swapchains. Das kommt
  erst, wenn es gebraucht wird.

## Erwogene Alternativen

- **Backends von ImGui für SDL3 und Vulkan:** am schnellsten, aber dann kennt der Editor Vulkan
  und SDL direkt, und ein zweites RHI-Backend bräuchte ein zweites ImGui-Backend.
- **ImGui direkt im Editor ohne Engine-Schicht:** weniger Code, aber ImGui-Header in jeder Datei
  des Editors und keine Grenze, hinter der sich die Bibliothek austauschen lässt.
- **Eigene UI oder RmlUi/Qt:** viel mehr Aufwand für ein Werkzeug; Qt passt schlecht zu einer
  Engine, die selbst zeichnet.
