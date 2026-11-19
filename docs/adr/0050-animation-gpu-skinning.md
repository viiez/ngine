# 0050: Animation mit Automaten als Datei und Skinning im Compute-Pass

- **Status:** angenommen
- **Datum:** 2026-11-18

## Kontext

R4.6 bringt Skelett-Animation (ANIM-01) und Blending (ANIM-02). Figuren kommen aus glTF wie
Netze, ein Skript der Spielfigur soll die Animation aus der Bewegung steuern, und Schritte sollen
zu den Füßen passen. Der Renderer ist GPU-getrieben mit zwei Zeichenpfaden (Mesh Shader und
indirekte Draws, [ADR 0043](0043-gpu-driven-meshlets.md)); Culling, Schatten, ID-Puffer und beide
Phasen lesen dieselben Eckpunkte über Adressen. Server und `--headless` laufen ohne GPU und sollen
nur rechnen, was Gameplay braucht. Abgestimmt in #58 (acht Punkte im Plan).

## Entscheidung

- **Daten aus glTF in reinen CPU-Daten.** `asset::Model` bekommt Skins (Gelenke, inverse
  Bindematrizen), Clips (Spuren für Verschiebung, Drehung, Skalierung; Schritt oder linear,
  Kubik-Splines beim Import auf 30 Hz abgetastet) und je Eckpunkt vier Gelenke und Gewichte
  (`SkinVertex`, 16 Bytes, Gewichte als normierte `u16`). Mehr als vier Einflüsse kürzt der Import
  auf die vier stärksten. Das gekochte Format (Version 3) trägt alles mit.
- **Hüllen der Meshlets aus allen Posen.** Beim Bauen der Meshlets eines Netzes mit Skin werden
  Ruhelage und alle Clips mit 30 Hz abgetastet; jede Meshlet-Kugel umschließt danach alle Posen
  (mit Zuschlag), der Normalenkegel ist abgeschaltet. Die Instanz nimmt die Vereinigung dieser
  Kugeln. Das Culling braucht so keine Pose auf der CPU.
- **Modul `ngine.animation` ohne GPU.** `Rig` (Knoten, Ruhelage, Skins, Clips) und `AnimGraph`
  sind Ressourcen des Ressourcen-Managers mit Hot Reload. Der Automat steht als JSON-Datei
  `.animgraph` im Asset-Ordner: Parameter (`float`, `bool`, `trigger`), Zustände mit einem Clip
  oder einem 1D-Blend über einen Parameter (Phasen gleich lang normiert), Ereignisse zu
  normierten Zeiten im Zustand, Übergänge mit Bedingungen, Dauer und `after_end`, `*` als
  beliebiger Ausgangszustand. Höchstens ein Übergang pro Bild; Überblendung mit Smoothstep, ein
  Übergang mitten in einer Überblendung friert die zuletzt gezeigte Pose ein. Einen grafischen
  Editor gibt es nicht; das Fenster „Animation“ zeigt Zustand und Parameter.
- **`AnimationSystem` nach der Simulation des Bildes.** Es führt für jede Entity mit `Animator`
  einen Automaten, rechnet Posen und Paletten (3×4-Matrizen) in Jobs und schickt Ereignisse
  sortiert über den `EventBus`. Es schreibt nichts in die Welt; die Prüfsumme bleibt. Keine Root
  Motion: Die Physik bewegt die Figur. `PoseMode` je Animator: `Visible` rechnet Posen nur mit
  Renderer, `Always` immer, `Never` nie; der Automat läuft immer, damit Zustand und Ereignisse
  überall gleich sind. Parameter hält das System, nicht die Komponente.
- **Skinning im Compute-Pass.** Vor allen anderen Pässen verformt `skinning.slang` die Eckpunkte
  jedes Netzes mit Skin, dessen Animator (an der Entity oder einem Vorfahren, für dasselbe Modell)
  eine Pose hat: lineares Skinning mit vier Einflüssen, Ziel ist ein eigener Bereich des
  `GeometryBuffer` je Instanz und Frame im Flug. Die `GpuInstance` zeigt danach dorthin; Indizes und
  Meshlets teilt sie mit dem Ruhe-Netz. Paletten aller Figuren liegen in einem Puffer pro Frame.
  Ungenutzte Bereiche gibt der Renderer nach zwei Frames frei.
- **C#** setzt Parameter (`SetAnimFloat`, `SetAnimBool`, `SetAnimTrigger`), liest den Zustand
  (`GetAnimState`) an der Entity oder ihrem ersten Nachkommen mit Animator und bekommt Ereignisse
  als `OnAnimEvent` vor `Update`, an der Entity mit dem Animator und ihren Vorfahren.

## Konsequenzen

- Alle Pässe und beide Zeichenpfade sehen verformte Netze als gewöhnliche Netze; ein Bildvergleich
  in der CI über lavapipe prüft, dass beide Pfade dasselbe zeigen.
- Jede sichtbare Figur kostet Speicher für ihre Eckpunkte je Frame im Flug (die Puppe: 1 224
  Eckpunkte zu 48 Bytes, rund 59 KB je Frame) und einen Dispatch.
- Kugeln aus allen Posen sind größer als nötig; Figuren werden etwas später aussortiert.
- Automaten lassen sich ohne Neustart ändern; Fehler in der Datei melden sich im Log, der alte
  Stand läuft weiter.
- Offen: 2D-Blends und Ebenen, Parameter in der Replikation (TD-61), Cursor beim Abtasten und
  Hüllen aus der Pose (TD-62), Animation auf dem Server (TD-63).

## Erwogene Alternativen

- **Skinning im Vertex- und Mesh-Shader:** spart Speicher, aber jeder Pass (Schatten, ID-Puffer,
  beide Phasen) skinnt neu, und jeder Shader braucht eine zweite Fassung.
- **Dual Quaternion Skinning:** weniger Einknicken an Schultern; kommt, wenn eine echte Figur es
  zeigt.
- **Bibliothek (ozz-animation):** ausgereift, aber eigenes Format und Laufzeit; im Lernprojekt
  sollen Abtasten, Mischen und Automat sichtbar sein, und der Umfang ist überschaubar.
- **Ereignisse in einer Datei neben dem Modell (`.animevents`):** Ereignisse am Zustand gelten
  auch für Blends und stehen dort, wo man den Automaten bearbeitet.
- **Hüllkugel der Instanz aus der Pose auf der CPU:** genauer, kostet aber Rechnung pro Figur und
  Bild; die feste Vereinigung der Meshlet-Kugeln reicht.
