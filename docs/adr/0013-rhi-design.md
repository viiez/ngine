# 0013: Aufbau der RHI

- **Status:** angenommen
- **Datum:** 2026-10-29

## Kontext

Iteration A2 braucht die erste Fassung der RHI (Render Hardware Interface). Sie soll klein bleiben,
Vulkan-Typen nicht nach außen tragen und später weitere Backends erlauben. Außerdem muss die RHI ohne
Fenster laufen (Tests, Werkzeuge), und die Plattformschicht soll Vulkan nicht kennen.

## Entscheidung

- `ngine.rhi` definiert abstrakte Klassen: `Device`, `Texture`, `Shader`, `Pipeline`, `CommandList`.
  Ressourcen gehören dem Aufrufer als `std::unique_ptr` und müssen vor dem `Device` zerstört werden.
- Jedes Backend ist ein eigenes Modul mit Fabrikfunktion, z. B. `ngine::rhi::vulkan::create_device()`.
  Die Anwendung wählt das Backend.
- Fehler beim Erzeugen kommen als `std::expected<T, std::string>` zurück.
- Fenster und RHI verbindet eine `SurfaceDesc` mit Funktionen statt Typen: Instanz-Erweiterungen und
  eine Funktion, die aus der API-Instanz eine Surface erzeugt.
- Ohne Surface arbeitet das Device offscreen; `execute()` und `read_texture()` erlauben Tests.
- Layout-Übergänge fügt die RHI automatisch mit groben Barrieren ein. Feinere Synchronisation kommt
  mit dem Render-Graph (REND-05).
- Die y-Achse zeigt im Viewport nach oben (negative Viewport-Höhe in Vulkan).

## Konsequenzen

- Der Renderer kennt nur `ngine.rhi`; ein D3D12- oder Metal-Backend käme als weiteres Modul dazu.
- Virtuelle Aufrufe pro Befehl kosten wenig, solange die Anzahl der Draw Calls klein ist. Falls das
  später bremst, können Handles und Befehlspuffer ohne virtuelle Aufrufe folgen.
- Die groben Barrieren sind korrekt, aber nicht optimal.

## Erwogene Alternativen

- **Handles statt Klassen (z. B. `PipelineHandle`):** schneller und besser für Netzwerk und Editor,
  aber für den Anfang mehr Code. Kann den Klassenansatz später ersetzen.
- **Backend zur Compile-Zeit (Templates oder `#ifdef`):** keine virtuellen Aufrufe, aber nur ein
  Backend pro Build.
- **Plattformschicht erzeugt die Surface direkt:** dann müsste sie Vulkan kennen.
