# R2: Welt und Editor

- **Phase:** Konstruktion
- **Status:** in Arbeit (R2.1 abgeschlossen, als Nächstes R2.2)
- **Pull Requests:** …

R2 ist der zweite Reifegrad der Konstruktion. Wie R1 ist er zu groß für eine Iteration; wir teilen
ihn in fünf Iterationen, jede mit eigenem Dokument, eigenem PR und eigener Abnahme auf dem PC.
Dieses Dokument hält den Plan für den ganzen Reifegrad und am Ende sein Review.

## Plan

**Ziel:** Es gibt einen eigenen Editor (`apps/editor/`), in dem man eine Szene öffnet, im Viewport
durchfliegt, Entities im Szenenbaum auswählt, ihre Komponenten im Inspector und mit Gizmos
bearbeitet, jede Änderung rückgängig macht, die Szene speichert und kurz „spielen“ lässt. Modelle
und Texturen kommen über stabile Asset-IDs aus einer Asset-Pipeline, die Quelldateien in
Laufzeitformate kocht und Änderungen an Dateien zur Laufzeit nachlädt.

**Features:** EDIT-01, EDIT-02, EDIT-03, EDIT-04, EDIT-05, EDIT-06 (ohne C#, siehe unten), EDIT-07,
EDIT-09, EDIT-10, CORE-11, CORE-12, REND-15 (Text), ASSET-02 (KTX2), ASSET-03, ASSET-04,
ASSET-05, PLAT-06, WORLD-04. Dazu die Schulden mit Frist R2: TD-08, TD-09, TD-12, TD-15, TD-17,
TD-18, TD-21, TD-22.

### Aufteilung

```mermaid
flowchart LR
    r21["R2.1<br/>Editor-Gerüst<br/>und Viewport"] --> r22["R2.2<br/>Szenenbaum, Inspector,<br/>Undo"]
    r22 --> r23["R2.3<br/>Gizmos und<br/>Spielen im Editor"]
    r23 --> r24["R2.4<br/>Asset-IDs, Ressourcen,<br/>Hot Reload"]
    r24 --> r25["R2.5<br/>Kochen, KTX2,<br/>Prefabs"]
```

| Iteration | Inhalt | Features und Schulden | Abnahme |
| --- | --- | --- | --- |
| R2.1 Editor-Gerüst und Viewport | Neue Anwendung `apps/editor/` mit Dear ImGui (Docking) hinter `ngine.editor_ui`. ImGui zeichnet über einen eigenen Pass im Render-Graph mit der RHI (Vertex Pulling, Schrift als bindlose Textur), Eingabe kommt aus `platform::Window`; weder Vulkan- noch SDL-Backend von ImGui. Der Renderer zeichnet in eine Textur, die das Viewport-Fenster zeigt; Größe folgt dem Fenster. Editor-Kamera mit derselben freien Steuerung wie die Sandbox. Log-Konsole als Sink, dazu ein Sink in eine Datei. `renderer.cpp` aufteilen | EDIT-01, EDIT-02, EDIT-10, CORE-12 ([R2.1](R2.1-editor-geruest-und-viewport.md)) | Editor startet unter Windows und Linux, Fenster andocken und wieder lösen, Standardszene im Viewport ohne Meldungen der Validation Layers; GPU-Test zeichnet ein ImGui-Fenster über lavapipe |
| R2.2 Szenenbaum, Inspector, Undo | Szenenbaum mit Hierarchie, Umbenennen und Umhängen per Ziehen; Auswahl per Klick im Viewport über einen ID-Puffer (Pass, der Entity-IDs in eine `R32_UINT`-Textur schreibt, ein Pixel wird zurückgelesen). Inspector baut Eingabefelder allein aus der Reflection; die Reflection lernt dafür Enums und Bereichsangaben. Undo und Redo als Befehle, die alte und neue Werte über die Reflection binär festhalten. Szene öffnen und speichern, Ereignisse zwischen Editor-Teilen (Auswahl, Änderungen) | EDIT-03, EDIT-04, EDIT-07, CORE-11, TD-08, TD-09, TD-18, TD-22 (aus R2.1) | Szene laden, Lichter und Transforms im Inspector ändern, alles rückgängig machen; gespeicherte Szene ist textgleich mit der geladenen, wenn nichts geändert wurde; Undo-Stapel und Inspector-Logik ohne GPU getestet |
| R2.3 Gizmos und Spielen im Editor | Gizmos für Verschieben, Drehen und Skalieren, selbst gebaut aus `ngine.math` und dem Debug-Zeichnen (Treffer über Strahlen gegen Achsen und Ringe), lokal und global, mit Raster. Text in der Welt für Namen und Werte (über die Zeichenliste von ImGui). „Spielen“ sichert die Welt binär, lässt den Schedule laufen und stellt beim Stoppen den Stand wieder her. Transform-Propagation nur für geänderte Teilbäume | EDIT-05, EDIT-06, REND-15, TD-15 | Objekte mit Gizmos bewegen, drehen und skalieren, jeweils mit Undo; Spielen und Stoppen hinterlässt die Welt bitgenau wie vorher; Treffertests und Sichern/Wiederherstellen ohne GPU getestet |
| R2.4 Asset-IDs, Ressourcen, Hot Reload | Jede Quelldatei bekommt eine stabile ID (`.meta`-Datei daneben); `MeshRenderer` und Materialien verweisen über IDs statt Indizes, damit Szenen mit Modellen vollständig speicherbar sind. Ressourcen-Manager mit Handles, Referenzzählung und Laden im Hintergrund über das Job-System; Uploads über eine eigene Transfer-Queue. Dateien beobachten und Shader, Texturen und Modelle zur Laufzeit neu laden. Asset-Browser im Editor mit Ziehen in die Szene | ASSET-04, ASSET-05, PLAT-06, EDIT-09, TD-12, TD-17 | Modell aus dem Browser in die Szene ziehen, speichern, Editor neu starten, Szene sieht gleich aus; Shader oder Textur ändern und das Bild ändert sich ohne Neustart |
| R2.5 Kochen, KTX2, Prefabs | Asset-Pipeline kocht glTF in ein binäres Mesh-Format und Bilder in KTX2 mit Basis Universal (KTX-Software); gekochte Daten liegen in einem Cache und werden nur bei Änderung neu erzeugt. Tangenten nach MikkTSpace beim Kochen. Prefabs: Vorlage als Datei, Instanzen mit Überschreibungen, im Editor anlegen und aufbrechen. Szenen optional binär | ASSET-02, ASSET-03, WORLD-04, TD-21 | Standardszene lädt aus dem Cache spürbar schneller als aus den Quellen (gemessen); Prefab ändern und alle Instanzen ziehen nach, Überschreibungen bleiben |

Nicht in R2, damit der Reifegrad in überschaubarer Zeit fertig wird:

- **C#-Skripte im Spielmodus** (Teil von EDIT-06): „Spielen“ in R2.3 lässt nur die C++-Systeme
  laufen; Skripte kommen mit dem vollständigen Scripting in R4 dazu.
- **Virtuelles Dateisystem und Archive** (PLAT-05): erst, wenn wir Pakete ausliefern (Übergabe);
  der Ressourcen-Manager liest bis dahin aus Ordnern.
- **Raytracing** (REND-14, im Katalog noch bei R2): gehört zum Rendering, nicht zu Welt und Editor.
  Vorschlag: nach R3, zusammen mit Mesh Shadern und GPU-Culling, oder später als eigene Stufe.
- **Phasen für Systeme und Befehlspuffer** (WORLD-09, WORLD-10): bleiben in R4, solange der
  Editor ohne sie auskommt.

### Entwurf

```mermaid
flowchart TB
    subgraph editor["apps/editor"]
        panels["Fenster: Viewport, Szenenbaum,<br/>Inspector, Assets, Konsole"]
        cmds["Befehle mit Undo/Redo"]
        gizmo["Gizmos, Auswahl"]
    end
    ui["ngine.editor_ui<br/>(Adapter für Dear ImGui)"]
    asset["ngine.asset<br/>IDs, Ressourcen, Kochen"]
    render["ngine.render<br/>Renderer, Pass für UI, ID-Puffer"]
    world["ngine.world<br/>Szenen, Prefabs"]
    reflect["ngine.reflect"]
    panels --> ui --> render
    panels --> cmds --> reflect
    gizmo --> render
    panels --> asset --> render
    cmds --> world --> reflect
```

- Der Editor ist eine eigene Anwendung neben der Sandbox. Was nicht nur der Editor braucht
  (Ressourcen-Manager, Asset-IDs, Pass für den ID-Puffer, Text in der Welt), liegt in der Engine.
- **Dear ImGui hinter `ngine.editor_ui`:** Nur der Adapter kennt ImGui. Wir nutzen ImGui im
  Editor zwar sehr direkt, deshalb exportiert der Adapter eine schmale Schicht im Stil von ImGui
  (Fenster, Felder, Bäume, Ziehen und Ablegen), keine eigene UI-Bibliothek. Das Zeichnen
  übernimmt ein eigener Pass über die RHI; so bleibt die Regel „nur die RHI kennt Vulkan“ und der
  Pass läuft später auch mit D3D12.
- **Inspector und Undo allein aus der Reflection:** Kein Code pro Komponente. Ein Befehl hält
  Entity, Komponente, Feldpfad und den alten und neuen Wert als Bytes aus dem Binärformat fest;
  dasselbe nutzt später die Replikation.
- **Spielen im Editor** sichert die Welt im Binärformat (Szene plus Laufzeitkomponenten) statt
  eine zweite Welt zu kopieren. Das ist einfach und deckt Fehler im Serialisieren früh auf.
- Voraussichtliche ADRs: Editor-Architektur und ImGui-Anbindung (R2.1), Undo über Befehle mit
  reflektierten Werten (R2.2), Asset-IDs und Ressourcen-Manager (R2.4), Format gekochter Assets
  und KTX2 (R2.5).

### Leitlinien für alle fünf Iterationen

- Editor-Logik (Befehle, Auswahl, Inspector-Modell, Treffertests der Gizmos, Asset-Datenbank)
  ist von der UI getrennt und ohne Fenster und GPU testbar. Die Fenster sind dünn.
- Die Engine bleibt ohne Editor lauffähig; `--headless` und `NGINE_WITH_WINDOW=OFF` bauen und
  starten weiter. Der Editor selbst braucht ein Fenster und wird ohne SDL3 nicht gebaut.
- Jede Iteration ist im Editor sichtbar; die Sandbox bleibt Schaufenster für Engine-Features und
  bekommt keine Editor-Fenster.
- Szenen bleiben JSON nach [ADR 0020](../adr/0020-scene-format-json.md); alles, was der Editor
  speichert, ist in Diffs lesbar.

**Risiken:**

- ImGui als Fremdbibliothek über Module: viele Makros und Header. Gegenmittel: ImGui nur im
  globalen Modulfragment des Adapters, ein früher Build unter MSVC und Clang in R2.1.
- Mehrere Viewports von ImGui (Fenster außerhalb des Hauptfensters) brauchen mehrere Swapchains.
  Gegenmittel: in R2.1 nur Docking im Hauptfenster; eigene Fenster für Panels erst bei Bedarf.
- Undo über Bytes bricht, wenn sich die Struktur der Welt ändert (Entity gelöscht und neu
  angelegt). Gegenmittel: Befehle verweisen auf stabile Entity-IDs der Szene, nicht auf
  flecs-IDs; Tests für Löschen und Wiederherstellen.
- Asset-IDs ändern das Szenenformat (`MeshRenderer`). Bis R2.4 kann der Editor Szenen mit
  Modellen nur zusammen mit dem Modellpfad laden, nicht vollständig speichern. Das steht so in der
  Abnahme von R2.2 und verschwindet mit R2.4.
- KTX-Software und Basis Universal sind groß; Bauzeit in der CI. Gegenmittel: Kochen nur im
  Editor-Build und im Werkzeug, nicht in Spiel und Server.

**Abnahme:** die Abnahmen der fünf Iterationen, dazu am Ende ein Review des ganzen Reifegrads mit
Architektur-Review und einem kurzen Szenario auf dem PC: Editor starten, Modell aus dem
Asset-Browser platzieren, mit Gizmos ausrichten, Licht im Inspector einstellen, alles einmal
rückgängig machen und wiederholen, speichern, spielen, stoppen, neu starten.

### Zur Abstimmung

1. **Reihenfolge:** Vorschlag ist Editor zuerst (R2.1 bis R2.3), Asset-Pipeline danach. Das macht
   früh etwas sichtbar und die Editor-Architektur ist das größere Risiko. Andersherum wären
   Szenen mit Modellen ab R2.2 vollständig speicherbar.
2. **Gizmos selbst bauen** statt ImGuizmo: Vorschlag selbst bauen, weil `ngine.math`,
   Debug-Zeichnen und Strahlen schon da sind und es lehrreich ist. ImGuizmo wäre schneller
   fertig, bringt aber eigene Mathematik mit.
3. **Spielen im Editor** zieht im Katalog von R4 nach R2.3 vor, ohne C#. Mit C# wird es in R4
   vollständig.
4. **Raytracing** (REND-14) verlässt R2.

## Review

*Folgt nach dem letzten Merge.*
