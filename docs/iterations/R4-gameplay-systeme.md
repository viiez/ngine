# R4: Gameplay-Systeme

- **Phase:** Konstruktion
- **Status:** im Review (R4.1 bis R4.6 abgeschlossen, Szenario auf dem PC offen)
- **Pull Requests:** #47 (Plan), weitere siehe Review

R4 ist der vierte Reifegrad der Konstruktion. Wie R1 bis R3 ist er zu groß für eine Iteration; wir
teilen ihn in sechs Iterationen, jede mit eigenem Dokument, eigenem PR und eigener Abnahme auf dem
PC. Dieses Dokument hält den Plan für den ganzen Reifegrad und am Ende sein Review.

## Plan

**Ziel:** Aus der Welt zum Ansehen wird eine Welt zum Spielen. Eine Spielfigur läuft über das
Voxel-Terrain, springt, gräbt und stößt Kisten um; starre Körper fallen und stoßen aneinander,
auch auf frisch gegrabenem Boden. Gameplay-Logik steht in C#-Skripten, die an Entities hängen, im
Inspector einstellbar sind, beim Speichern der Quelldatei neu laden und im Spielmodus des Editors
laufen. Die Welt klingt räumlich, und Figuren bewegen sich mit Skelett-Animation. Alles außer
Audio und Skinning läuft ohne Fenster und GPU, damit der dedizierte Server in R5 dieselbe
Simulation rechnet.

**Features:** PHYS-01, PHYS-02, PHYS-03, PHYS-04, VOX-08, SCR-01, SCR-02, SCR-03, SCR-04,
WORLD-02 (C#-Seite), WORLD-09, WORLD-10, EDIT-06 (Skripte im Spielmodus), PLAT-02 (Aktionen,
Gamepad), AUD-01, AUD-02, ANIM-01, ANIM-02, VOX-07 (Rest). Dazu die Schulden mit Frist R4: TD-29,
TD-32, TD-37, TD-38, TD-42, TD-45 und TD-46. Für UI-01 und UI-02 schlägt der Plan eine neue Frist
vor (siehe [Zur Abstimmung](#zur-abstimmung)).

### Aufteilung

```mermaid
flowchart LR
    r41["R4.1<br/>Spielschleife<br/>und Eingabe"] --> r42["R4.2<br/>Physik mit Jolt"]
    r42 --> r43["R4.3<br/>Terrain-Kollision<br/>und Spielfigur"]
    r43 --> r44["R4.4<br/>C#-Scripting"]
    r44 --> r45["R4.5<br/>Audio"]
    r44 --> r46["R4.6<br/>Animation"]
```

| Iteration | Inhalt | Features und Schulden | Abnahme |
| --- | --- | --- | --- |
| [R4.1](R4.1-spielschleife-und-eingabe.md) Spielschleife und Eingabe | Eine gemeinsame Spielschleife für Sandbox, Editor und Server: Phasen für Systeme (Eingabe, feste Simulation, Nachbereitung, Rendering) mit expliziter Reihenfolge, fester Zeitschritt für die Simulation mit Interpolation beim Zeichnen, Befehlspuffer für Strukturänderungen aus Systemen heraus. Eingabe als Aktionen (Tasten, Maus und Gamepad auf benannte Aktionen und Achsen abgebildet, Belegung als Datei). Fehlende Tasten in `editor_ui` und Kürzel der Voxel-Werkzeuge | WORLD-09, WORLD-10, PLAT-02, TD-42 | Ein System erzeugt und zerstört Entities über den Befehlspuffer (Test, auch unter ThreadSanitizer); die Simulation rechnet bei 30 und bei 240 Bildern pro Sekunde gleich viele Schritte pro Sekunde und dasselbe Ergebnis (Test); eine Aktion „Springen“ kommt von Tastatur und Gamepad; Kürzel der Pinsel im Editor |
| [R4.2](R4.2-physik-mit-jolt.md) Physik mit Jolt | Jolt hinter `ngine.physics` (`PhysicsWorld`), läuft auf dem Job-System der Engine und ohne GPU. Komponenten `RigidBody` und `Collider` (Kugel, Quader, Kapsel, konvexe Hülle und Dreiecksnetz aus dem gekochten Modell) in der Reflection, damit Editor, Szene und Prefabs sie kennen. Schritt in der festen Simulation, Lagen zurück in `Transform`. Strahl-, Form- und Überlappungsabfragen, Ebenen und Filter, Kontakt-Ereignisse über den `EventBus`. Formen als Debug-Zeichnen im Editor. Transforms parallel, wenn die Messung es lohnt | PHYS-01, PHYS-02, PHYS-04, TD-29 | Ein Stapel Kisten fällt im Editor im Spielmodus um und kommt beim Stoppen bitgenau zurück; Physik weit weg vom Ursprung (`--far`) zittert nicht; `--headless` rechnet dieselbe Szene; 2 000 bewegte Körper auf dem PC gemessen (Schritt der Physik, `update_transforms()`) |
| [R4.3](R4.3-terrain-kollision-und-spielfigur.md) Terrain-Kollision und Spielfigur | Kollisionsformen aus den Netzen der Chunks, gebaut in Jobs und nur dort, wo Körper oder Beobachter sind; nach einem Pinsel wird die Form des Chunks ersetzt und ruhende Körper wachen auf. Character Controller (Kapsel, Stufen, Hänge, Springen) und eine Spielfigur in der Sandbox mit Kamera in Ich- und Schulterperspektive, die läuft und gräbt. Materialtabelle des Terrains als Asset mit Reibung und Namen für Schrittgeräusche, im Editor bearbeitbar, Texturen über den Koch-Cache; mehr als acht Materialien pro Chunk | VOX-08, PHYS-03, VOX-07, TD-37 (TD-38 bei Bedarf, siehe R4.3) | Die Figur läuft über Hügel und durch Höhlen, ohne durch den Boden zu fallen, auch beim Streamen neuer Chunks; Boden unter einer Kiste weggraben: Sie fällt; Zeit für das Bauen einer Form pro Chunk und Speicher gemessen; `--headless` lässt Körper auf Terrain fallen; ein neues Material entsteht im Editor ohne Neubauen |
| [R4.4](R4.4-csharp-scripting.md) C#-Scripting | Bindings aus der Reflection erzeugt: Komponenten als C#-Structs, Entity-API (`entity.Get<T>()`, `Set`, Hierarchie, Abfragen), dazu Eingabe-Aktionen, Physik-Abfragen, Ereignisse und Log. Skript-Komponenten: C#-Klassen hängen an Entities, ihre Felder erscheinen im Inspector und in der Szene. Hot Reload: Ändert sich eine `.cs`-Datei, baut die Engine neu, entlädt, lädt und stellt die Feldwerte wieder her. Skripte laufen im Spielmodus des Editors und auf dem Server. Prefabs mit entfernten Knoten und verschachtelt | SCR-01, SCR-02, SCR-03, SCR-04, WORLD-02, EDIT-06, TD-32 | Die Steuerung der Spielfigur und eine Laterne, die man an- und ausschaltet, sind C#-Skripte; ein Wert im Skript ändern und speichern: Im laufenden Spiel gilt er ohne Neustart, Feldwerte bleiben; eine neue Komponente in C++ ist nach dem Bauen in C# da, ohne Handarbeit; Kosten von 10 000 Skript-Updates pro Frame gemessen; Build ohne .NET-SDK geht weiter |
| [R4.5](R4.5-audio.md) Audio | miniaudio hinter `ngine.audio` (`AudioDevice`). Klänge als Assets mit `.meta` (kurze im Speicher, lange als Strom), Komponenten `AudioSource` und `AudioListener`, 3D-Klang mit Abschwächung und Doppler, Busse für Lautstärken (Musik, Effekte). Ohne Gerät und im Server läuft ein Null-Gerät. Aus C# abspielbar | AUD-01, AUD-02 | Schritte je nach Terrain-Material, eine Klangquelle in der Welt ist geortet und wird mit Abstand leiser; `--headless` und die CI laufen ohne Audio-Gerät; kein Knacken beim Streamen des Terrains (Mischen im eigenen Thread, auf dem PC geprüft) |
| [R4.6](R4.6-animation.md) Animation | Skelette, Skins und Clips aus glTF, gekocht. Abtasten und Mischen in Jobs, Zustandsautomat mit Übergängen und einem 1D-Blend (Stehen, Gehen, Laufen), aus C# gesteuert. Skinning auf der GPU vor dem Zeichnen, sodass Meshlets, Culling, Schatten und der Rückfallpfad unverändert bleiben. Instanzen dauerhaft auf der GPU, Zähler und GPU-Zeiten je Pass im Statistikfenster | ANIM-01, ANIM-02, TD-45, TD-46 | Eine animierte Figur läuft über das Terrain, Übergänge ohne Sprung; Mesh-Shader-Pfad und `--baseline` zeigen dasselbe Bild (CI über lavapipe); 200 animierte Figuren auf dem PC gemessen; der Server rechnet Posen nur, wo Gameplay sie braucht |

Nicht in R4, damit der Reifegrad in überschaubarer Zeit fertig wird:

- **Spiel-UI** (UI-01, UI-02): HUD und Menüs braucht erst der Prototyp. In R4 reichen der
  Debug-Text und ein Fadenkreuz über die Zeichenliste. Vorschlag: nach R5, die Evaluation
  entscheidet über Bibliothek und Zeitpunkt.
- **Replikation von Physik, Skripten und Animation** (NET-04, NET-05): in R5. R4 legt dafür nur
  die Grundlage: fester Zeitschritt, Simulation ohne GPU, Eingabe als Aktionen.
- **Hall und Verdeckung** (AUD-03), **Inverse Kinematik** (ANIM-03), **Navigation und KI**
  (AI-01, AI-02), **Statik** (VOX-10): später.
- **Schatten der Taschenlampe** (TD-25): bleibt in R6; kann als einzelne Iteration vorgezogen
  werden, wenn die Laterne aus R4.4 sie braucht.
- **Fahrzeuge, Ragdolls, Gelenke, Stoff:** Jolt kann sie; wir bauen sie, wenn das Spiel sie
  braucht.

### Entwurf

```mermaid
flowchart TB
    subgraph apps["Sandbox, Editor, Server"]
        loop["Spielschleife<br/>Phasen, fester Zeitschritt"]
    end
    script["ngine.script<br/>Bindings, Skript-Komponenten,<br/>Hot Reload"]
    physics["ngine.physics<br/>PhysicsWorld (Jolt),<br/>Körper, Abfragen, Figur"]
    audio["ngine.audio<br/>AudioDevice (miniaudio)"]
    anim["ngine.animation<br/>Skelett, Clips, Automat"]
    input["ngine.input<br/>Aktionen"]
    world["ngine.world<br/>Phasen, Befehlspuffer, Terrain"]
    voxel["ngine.voxel"]
    render["ngine.render<br/>Skinning"]
    plat["ngine.platform"]
    loop --> script
    loop --> physics
    loop --> audio
    loop --> anim
    loop --> render
    script --> world
    script --> physics
    script --> input
    script --> audio
    script --> anim
    physics --> world
    physics --> voxel
    audio --> world
    anim --> world
    render --> anim
    input --> plat
```

- **Spielschleife:** Bisher ruft jede App `Schedule::run()` und `update_transforms()` selbst auf.
  Neu gibt es Phasen: Eingabe, feste Simulation (keinmal oder mehrmals pro Bild, Vorschlag 60 Hz),
  Nachbereitung (Transforms, Animation, Audio), Rendering. Physik, Skripte mit `FixedUpdate` und
  später die Netzwerk-Ticks laufen in der festen Simulation; gezeichnet wird zwischen den letzten
  beiden Schritten interpoliert. Der Befehlspuffer sammelt Strukturänderungen pro Thread und führt
  sie am Ende der Phase in fester Reihenfolge aus, damit die Simulation reproduzierbar bleibt.
- **Physik:** Nur der Adapter in `ngine.physics` kennt Jolt. Die Welt bleibt die Quelle der
  Wahrheit für das, was gespeichert wird (`Transform`, `RigidBody`, `Collider`); Jolt hält den
  Zustand der Simulation. Dynamische Körper schreiben nach jedem Schritt ihre Lage in
  `Transform`, kinematische lesen sie. Jolt rechnet über einen Adapter auf `ngine.jobs`, damit
  nicht zwei Thread-Pools um die Kerne streiten (Erkenntnis aus R3.3). Positionen in 64 Bit: Jolt
  mit doppelter Genauigkeit für Positionen, passend zu [ADR 0012](../adr/0012-64bit-world-coordinates.md).
- **Terrain-Kollision:** Die Form eines Chunks ist ein Dreiecksnetz aus dem Netz der Stufe 0, das
  der Mesher ohnehin baut; ein Höhenfeld scheidet wegen Höhlen und Überhängen aus. Formen
  entstehen in Jobs und nur in einem Radius um Körper und Beobachter (Vorschlag: ein Beobachter der
  Physik wie `TerrainObserver`). Körper außerhalb geladener Kollision schlafen, statt zu fallen.
  Grobe Stufen bekommen keine Kollision.
- **Scripting:** Ein Werkzeug erzeugt beim Bauen aus der `TypeRegistry` die C#-Structs der
  Komponenten (blittable, gleiche Anordnung wie in C++, geprüft durch einen Test) und die Aufrufe
  der Engine-API über `NativeApi`. Zugriff ohne Kopie wie in D2. Skript-Klassen meldet die
  C#-Seite mit ihren Feldern bei der Engine an; die Engine führt sie als Komponente `Script` mit
  Klassenname und Feldwerten, sodass Inspector, Szene, Prefabs und Undo sie ohne Sonderweg
  behandeln. Hot Reload nutzt den `FileWatcher` und `dotnet build` im Hintergrund; Feldwerte
  überleben über dieselbe Serialisierung. Ein ausgeliefertes Spiel braucht nur die Laufzeit, kein
  SDK.
- **Audio:** miniaudio mischt in seinem eigenen Thread; die Engine schickt ihm Befehle über eine
  Warteschlange ohne Sperren. Klänge sind Assets des Ressourcen-Managers. Der Server baut
  `ngine.audio` gar nicht ein, die CI nutzt das Null-Gerät.
- **Animation:** Abtasten und Mischen rechnet `ngine.animation` ohne GPU. Ein Compute-Pass schreibt
  die verformten Eckpunkte vor dem Zeichnen in den gemeinsamen Geometriepuffer; danach sieht der
  Renderer ein gewöhnliches Netz, dessen Meshlet-Hüllen um den Bewegungsspielraum vergrößert sind.
  So bleibt es bei einem Zeichenpfad ([ADR 0043](../adr/0043-gpu-driven-meshlets.md)).
- Voraussichtliche ADRs: Spielschleife mit Phasen und festem Zeitschritt (R4.1), Physik mit Jolt
  und Weltpositionen (R4.2), Kollision des Terrains (R4.3), C#-Bindings und Skript-Komponenten
  (R4.4), Audio mit miniaudio (R4.5), Animation und Skinning (R4.6).

### Leitlinien für alle sechs Iterationen

- Spielschleife, Physik, Skripte und das Rechnen der Animation laufen ohne Fenster und GPU und
  sind so getestet. `--headless` simuliert die Demo-Szene.
- Jede Iteration ist in Sandbox und Editor sichtbar. Die Sandbox bekommt eine Szene „Spielplatz“
  auf dem endlosen Terrain, die mit jeder Iteration wächst: Kisten, Spielfigur, Skripte, Klang,
  animierte Figur.
- Jede neue Komponente steht in der Reflection und ist damit im Inspector bearbeitbar, in Szenen
  und Prefabs speicherbar, in C# sichtbar und für R5 replizierbar.
- Leistung wird in jeder Iteration auf dem PC gemessen und im Iterationsdokument festgehalten.
  Was im Hauptthread pro Entity läuft, wird mit tausenden Entities gemessen (Erkenntnis aus R3).
- Neue Systeme laufen unter ThreadSanitizer (`[physics]`, `[animation]` kommen zu den Tests des
  Presets `linux-clang-tsan`).

**Risiken:**

- **Jolt und 64-Bit-Positionen:** Der vcpkg-Port baut nur mit einfacher Genauigkeit (in R4.2
  geprüft). Gegenmittel: ein Overlay-Port im Repository mit `DOUBLE_PRECISION`, siehe
  [R4.2](R4.2-physik-mit-jolt.md).
- **Jolt, Module und MSVC:** Die Header von Jolt sind groß und voller Makros. Gegenmittel: nur im
  Adapter einbinden, Pimpl, früh auf Windows bauen.
- **Kollision beim Streaming:** Eine Form fehlt, während ein Körper schon da ist, oder der Tausch
  nach einem Pinsel lässt Körper durchfallen. Gegenmittel: Körper schlafen ohne Kollision unter
  sich; Formen der Chunks um die Figur haben Vorrang; deterministische Tests (Körper auf Chunk,
  der gerade ersetzt wird).
- **Bindings und Anordnung im Speicher:** C#-Structs müssen Byte für Byte zu C++ passen, auch mit
  `math::Affine`, `AssetRef` und Texten. Gegenmittel: Größen und Offsets aus der Reflection in
  einem Test gegen C# prüfen; Felder, die nicht blittable sind, über Zugriffsfunktionen.
- **Hot Reload mit Zustand:** Entladen scheitert still, wenn die Engine noch Verweise auf
  Skript-Objekte hält (schon in D2). Gegenmittel: Engine hält nur Kennungen, Test auf Freigabe wie
  in D2, Neuladen im Spielmodus als eigener Test.
- **Audio in der CI und unter Linux:** Geräte fehlen oder verhalten sich je nach PipeWire,
  PulseAudio und ALSA anders. Gegenmittel: Null-Gerät in Tests, Test auf dem Notebook und dem PC.
- **Skinning und Meshlets:** Hüllen verformter Meshlets stimmen nicht, Figuren verschwinden am
  Bildrand. Gegenmittel: Hüllen vergrößern, Bildvergleich beider Pfade in der CI.
- **Umfang:** Sechs Iterationen mit drei neuen Bibliotheken sind mehr als in R3. Gegenmittel:
  R4.5 und R4.6 hängen nur an R4.4 und lassen sich tauschen oder einzeln verschieben.

**Abnahme:** die Abnahmen der sechs Iterationen, dazu am Ende ein Review des ganzen Reifegrads mit
Architektur-Review und einem kurzen Szenario auf dem PC: Editor öffnen, Szene „Spielplatz“ laden,
Spielen starten, mit der Figur über das Terrain in eine Höhle laufen, den Boden unter einer Kiste
weggraben, die Laterne schalten, Schritte und eine Klangquelle hören, ein Skript bei laufendem
Spiel ändern, stoppen: Die Welt ist wie vorher. Dasselbe mit Gamepad in der Sandbox und mit
`--headless` ohne Fehler.

### Zur Abstimmung

1. **Reihenfolge:** Vorschlag ist Spielschleife, Physik, Terrain-Kollision mit Spielfigur,
   Scripting, dann Audio und Animation. Die Spielschleife zuerst, weil Physik, Skripte und R5 den
   festen Zeitschritt brauchen. Physik vor Scripting, weil die Kollision mit dem streamenden
   Terrain das größte Risiko ist und die Bindings dann gleich etwas zum Anbinden haben. Audio und
   Animation zuletzt, weil sie am wenigsten Abhängige haben.
2. **Sechs Iterationen** statt fünf. Alternative: Animation ganz hinter R5 schieben, weil der
   Server sie kaum braucht; dann fehlt der Evaluation nach R5 aber ein Kernsystem.
3. **Fester Zeitschritt von 60 Hz** für die Simulation mit Interpolation beim Zeichnen. Der
   Netzwerk-Tick aus D3 (30 Hz) bleibt davon getrennt und wird in R5 festgelegt.
4. **Jolt mit doppelter Genauigkeit** für Positionen statt eines verschiebbaren Ursprungs. Kostet
   laut Jolt wenige Prozent, passt aber ohne Sonderfälle zu ADR 0012 und zu mehreren Spielern, die
   weit auseinander stehen. Bitgleiche Physik zwischen Plattformen schalten wir nicht ein: Der
   Server ist autoritativ ([ADR 0007](../adr/0007-server-authoritative-multiplayer.md)), und die
   Option kostet Leistung.
5. **Animation selbst bauen** (Abtasten, Mischen, Zustandsautomat) statt ozz-animation. Der Import
   aus glTF, das Kochen und das Skinning sind ohnehin eigene Arbeit, der Rest ist überschaubar
   und lehrreich; ozz-animation bleibt der Rückfall.
6. **Skript-Felder im Inspector und Hot Reload über `dotnet build`:** Zum Entwickeln braucht es
   das .NET-SDK, zum Spielen nur die Laufzeit. Alternative wäre Roslyn in der Engine; das ist
   schneller beim Neuladen, aber deutlich mehr Gewicht.
7. **Neue Fristen:** Spiel-UI (UI-01, UI-02) nach R5; die Evaluation entscheidet. TD-29
   (Transforms parallel) bauen wir in R4.2 nur ab, wenn die Messung mit 2 000 Körpern es lohnt,
   sonst wird die Frist „bei Bedarf“.
8. **Demo-Szene „Spielplatz“** in der Sandbox als roter Faden und Abnahme-Szenario. Modelle und
   Klänge dafür brauchen freie Lizenzen (CC0); Vorschläge kommen mit den Iterationen, die sie
   brauchen.

**Ergebnis der Abstimmung** (viiez, mit dem Merge des Plans): Alle acht Punkte passen wie
vorgeschlagen. Reihenfolge R4.1 bis R4.6 in sechs Iterationen; fester Zeitschritt von 60 Hz mit
Interpolation; Jolt mit doppelter Genauigkeit für Positionen, ohne bitgleiche Physik zwischen
Plattformen; Animation selbst gebaut, ozz-animation als Rückfall; Hot Reload über `dotnet build`;
Spiel-UI (UI-01, UI-02) nach R5, die Evaluation entscheidet; TD-29 in R4.2 nur, wenn die Messung
es lohnt; Demo-Szene „Spielplatz“ mit Modellen und Klängen unter CC0. Die neuen Fristen stehen im
[Schuldenregister](../tech-debt.md) und im [Feature-Katalog](../features.md).

## Review

**Ergebnis:** Aus der Welt zum Ansehen ist eine Welt zum Spielen geworden. Sandbox, Editor und
Server laufen über eine gemeinsame Spielschleife mit Phasen, festem Zeitschritt von 60 Hz,
Interpolation beim Zeichnen und einem Befehlspuffer; Eingabe kommt als benannte Aktionen von
Tastatur, Maus und Gamepad. Jolt rechnet mit Positionen in doppelter Genauigkeit auf dem
Job-System, starre Körper fallen und stoßen sich, Abfragen und Kontakte stehen dem Gameplay zur
Verfügung. Das endlose Terrain hat Kollision um Körper, Figuren und Beobachter, Graben ersetzt die
Formen, und eine Spielfigur läuft in Ich- und Schulterperspektive durch Höhlen. Gameplay steht in
C#-Skripten, deren Felder Inspector, Szenen und Prefabs kennen und die beim Speichern neu laden,
ohne dass das Spiel anhält. Ein eigener Mixer ortet Klänge in 64 Bit, Schritte klingen nach dem
Boden. Figuren bewegen sich mit Skelett-Animation, verformt auf der GPU vor allen Pässen.

| Iteration | Pull Requests | Ergebnis |
| --- | --- | --- |
| [R4.1](R4.1-spielschleife-und-eingabe.md) Spielschleife und Eingabe | #47 (Plan), #49 | `Simulation` mit Phasen und festem Schritt, `Commands`, `ngine.input` mit Aktionen und Gamepad, Kürzel der Pinsel |
| [R4.2](R4.2-physik-mit-jolt.md) Physik mit Jolt | #50 (Plan), #51 | Jolt 5.6 in doppelter Genauigkeit über einen Overlay-Port, `RigidBody` und `Collider`, Abfragen, Kontakte, Spielplatz, bitgenaues Stoppen im Editor |
| [R4.3](R4.3-terrain-kollision-und-spielfigur.md) Terrain-Kollision und Spielfigur | #52 (Plan), #53 | Formen der Chunks in Jobs um Anker, Einfrieren ohne Boden, `CharacterController`, Materialtabelle als Asset |
| [R4.4](R4.4-csharp-scripting.md) C#-Scripting | #54 (Plan), #55, #56 | Bindings aus der Reflection, Skript-Klassen als Typen der Registry, Hot Reload über `dotnet build`, Prefabs mit Entfernungen und verschachtelt |
| [R4.5](R4.5-audio.md) Audio | #57 | Eigener Mixer, miniaudio für Gerät und Dekodieren, `AudioSource` und `AudioListener`, Ströme, Klangsätze, Null-Gerät |
| [R4.6](R4.6-animation.md) Animation | #58 | Skins und Clips gekocht, Automat mit Hot Reload in Jobs, Skinning im Compute-Pass, GPU-Zeiten je Pass |

Abnahme: Alle sechs Iterationen sind abgenommen, jede mit grüner CI auf Linux, unter
ThreadSanitizer und auf Windows sowie einem Test auf dem PC. Offen ist das Szenario für den ganzen
Reifegrad auf dem PC (Schritte in der [Einrichtung](../setup.md#r4-testen-szenario-des-reifegrads));
mit ihm ist der Reifegrad abgeschlossen.

Abweichungen vom Plan:

- **Der Server rechnet die Simulation noch nicht mit:** `--server` lädt weder Spielplatz noch
  Skripte noch Figuren (TD-54, TD-63) und nimmt die eingebaute Materialtabelle (TD-51). Ohne GPU
  läuft alles, was der Server braucht, aber gezeigt ist das nur mit `--headless --playground`.
  Das ist die erste Aufgabe in R5.
- **Instanzen dauerhaft auf der GPU** (TD-46) und die Zähler der Meshlets je Pass sind nicht
  gekommen; TD-46 hat die Frist R5.
- **Root Motion, 2D-Blends, Ebenen im Automaten** und Ereignisse neben dem Modell fehlen bewusst
  (TD-61), ebenso das Kochen von Klängen (TD-56) und ihre Replikation (TD-58).
- **Bitgleich nur im selben Build:** Ein Lauf ist mit 1 und 16 Threads gleich, aber Clang und MSVC
  landen nach 600 Schritten an verschiedenen Orten (R4.3). Wie geplant: Der Server ist die
  Wahrheit ([ADR 0007](../adr/0007-server-authoritative-multiplayer.md)).
- **Spiel-UI** blieb wie abgestimmt draußen; der Debug-Text reicht bisher.

**Erkenntnisse:**

- Die Welt als einzige Quelle der Wahrheit hat sich mit jeder Bibliothek bezahlt gemacht: Physik,
  Skripte, Klänge und Animation halten ihren Laufzustand in eigenen Tabellen, und Inspector,
  Szenen, Prefabs, Undo und das Stoppen im Editor brauchten für keines davon einen Sonderweg.
- Reproduzierbarkeit hat mehr Fehler gefunden als jeder andere Test: die Prüfsumme des
  Spielplatzes, zwei Läufe hintereinander, 1 gegen 16 Threads, Bildraten von 30 bis 240. Fast jede
  Iteration hat dabei etwas gefunden, das sonst erst im Netzwerk aufgefallen wäre (Zustände in
  Jolt, Ende von Kontakten, Zeit in `f32`).
- Was nur in festen Schritten weiterrückt, ruckelt pro Bild. Doppler, Parameter der Animation und
  die Kamera mussten jeweils über die Zeit seit der letzten Änderung geglättet werden. Für die
  Vorhersage in R5 gilt das genauso.
- Eine Fremdbibliothek hinter einem Modul kostet vor allem Arbeit an den Schaltern: Jolt gab
  Befehlssätze weiter, die Clang beim Laden der Moduldateien ablehnt, und verhielt sich unter
  Windows als DLL anders. Die Prüfung der Definitionen beim Start und ein eigener Port haben sich
  gelohnt.
- Selbst gebaut war in R4 zweimal die bessere Wahl: der Mixer (bitgenau testbar mit dem
  Null-Gerät) und die Animation (klein, ohne neue Abhängigkeit). Bei der Physik war es richtig,
  Jolt zu nehmen.
- Abnahmen auf dem PC fanden in R4.5 und R4.6 Fehler, die nur zu hören oder zu sehen waren
  (Abschwächung ohne Stille, Skelett in der Figur). Hören und Sehen gehören weiter in jede Abnahme.
- Ein lokaler Build mit Renderer und Editor vor dem PR (seit R4.1) hat die Läufe der CI von rund
  25 Minuten pro Fehler deutlich verringert. Bruchstellen bleiben MSVC mit Modulen (`<span>` über
  andere Header, `==` auf Matrizen, Reihenfolge statischer Objekte) und Code, den ThreadSanitizer
  nicht sieht (Jolt, lavapipe, .NET).

**Architektur-Review des Reifegrads:**

- Schichten wie geplant: `ngine.input`, `ngine.physics`, `ngine.audio` und `ngine.animation` hängen
  an Core, Math, Reflection, Asset und Welt, nicht an Fenster oder GPU; `ngine.script` hängt an
  ihnen, nicht umgekehrt; `ngine.render` kennt `ngine.animation` nur für die Paletten. Nur das
  Werkzeug `ngine_bindgen` kennt den Renderer, um C#-Structs für dessen Komponenten (Kamera,
  Lichter, `MeshRenderer`) zu erzeugen. Damit bleibt die Simulation ohne Fenster und GPU
  lauffähig, wie der Server es braucht.
- Fremdbibliotheken: Jolt nur in `jolt_world.cpp`, miniaudio und `stb_vorbis` nur in
  `miniaudio.cpp` und `miniaudio_impl.cpp`, hostfxr nur in `runtime.cpp`, SDL3 weiter nur im
  Adapter des Fensters.
- Neue ADRs: 0044 (Spielschleife mit Phasen, festem Schritt und Befehlspuffer), 0045 (Jolt mit
  doppelter Genauigkeit), 0046 (Kollision des Terrains und `CharacterVirtual`), 0047
  (Skript-Komponenten, Bindings, Hot Reload), 0048 (Prefabs mit Entfernungen, verschachtelt),
  0049 (eigener Mixer mit miniaudio), 0050 (Animation und Skinning auf der GPU).
- **Die Simulation wird zweimal zusammengesetzt:** `ModelScene` in `apps/common` baut für Sandbox und
  Editor Spielschleife, Physik, Skripte, Animation und Audio zusammen und braucht dafür ein
  GPU-Gerät; `apps/sandbox/playground.cppm` baut dieselben Systeme für `--headless` noch einmal
  ohne GPU. Der Server in R5 bräuchte ein drittes Mal. Das ist der wichtigste Befund des Reviews:
  Die Engine-Module sind sauber getrennt, die Apps aber nicht.
- Größte Dateien: `engine/physics/jolt_world.cpp` (rund 2 260 Zeilen), `apps/common/model_scene.cppm`
  (rund 1 280), `engine/world/prefab.cpp` (rund 950), `apps/editor/main.cpp` (rund 940),
  `engine/voxel/surface_nets.cpp` (rund 940). `jolt_world.cpp` mischt sechs Aufgaben (Laufzeit,
  Job-Adapter, Ebenen und Filter, Kontakte, Kollision des Terrains, Figuren) und ist so groß wie
  keine Datei zuvor; `model_scene.cppm` hält Standardszene, Kamera, Laden und alle Systeme in einer
  Klasse. Beides kommt als TD-64 ins Register (siehe [Zur Abstimmung](#zur-abstimmung-des-reviews)).
  `main.cpp` des Editors und `surface_nets.cpp` sind seit R3 Kandidaten und kaum gewachsen.
- Schulden: Alle mit Frist R4 sind abgebaut oder umgeplant: TD-32, TD-37, TD-42, TD-45 und TD-52
  abgebaut; TD-29 und TD-38 nach Messung „bei Bedarf“, TD-46 nach R5. Neu aus R4 sind TD-47 bis
  TD-63; davon haben elf zusammen mit TD-13, TD-14 und TD-41 die Frist R5 (Server, Replikation,
  ThreadSanitizer für Jolt), die übrigen „bei Bedarf“ oder die Übergabe.
- 432 Testfälle (339 am Ende von R3), dazu die GPU-Tests über lavapipe in beiden Zeichenpfaden und
  die `[script]`-Fälle mit dem .NET-SDK in der CI.

**Leistung** auf dem PC (RTX 3060 Ti, 16 Threads, Windows, Release), gemessen in den Iterationen:

| Größe | Wert |
| --- | --- |
| Schritt der Physik mit 2 031 Körpern, Median | 2,9 ms |
| `update_transforms()` mit 2 031 Körpern, Median | 0,26 ms |
| Spielfigur (`CharacterVirtual`) | 0,013 ms pro Figur |
| Bau einer Form des Terrains | 1,1 ms pro Chunk (in Jobs) |
| 10 000 Skripte, `Update` pro Bild | 0,94 ms |
| 500 geortete Klänge, `AudioSystem` pro Bild / Last des Mixers | 0,3 ms / 12 % |
| 200 animierte Figuren, `AnimationSystem` in Jobs / Skinning auf der GPU | 0,073 ms / 0,25 ms |
| GPU gesamt im Spielplatz mit 200 Figuren | 4,32 ms (Schatten 1,70, Vorwärts 2,10) |

Teuerstes Einzelsystem ist der Schritt der Physik bei vielen wachen Körpern; der Spielplatz hat
nur einen Bruchteil davon. Die GPU-Zeit hängt an Schatten und Vorwärts-Pass, nicht an den
neuen Systemen.

### Zur Abstimmung des Reviews

1. **TD-64, Simulation einmal zusammensetzen** mit Frist R5, als erster Schritt der ersten
   Iteration: Ein Teil ohne GPU in `apps/common` (Arbeitstitel `GameWorld`: Welt, Spielschleife,
   Physik, Terrain-Kollision, Skripte, Automaten der Animation) nutzen `ModelScene`, `--headless`
   und der Server gleich; `ModelScene` legt nur Renderer, Audio und Kamera darüber. Dabei zerfällt
   `jolt_world.cpp` in Dateien für Laufzeit und Jobs, Körper und Abfragen, Terrain und Figuren mit
   einer internen Partition für den gemeinsamen Zustand, wie der Renderer seit R3.2. Alternative:
   Der Server baut die Systeme ein drittes Mal, und TD-64 bekommt die Frist „bei Bedarf“; das spart
   jetzt Zeit, aber Netzwerk-Code müsste dann in drei Zusammenbauten gleich eingehängt werden.
2. **Plan für R5 im selben PR:** Das Review hier schließt R4, sobald das Szenario auf dem PC
   bestanden ist; der [Plan für R5](R5-multiplayer.md) (Multiplayer) liegt zur Abstimmung daneben
   und verteilt die Schulden mit Frist R5 auf seine Iterationen.
