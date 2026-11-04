# 0026: Render-Graph über explizite Zustände, ein bindloser Descriptor-Satz

- **Status:** angenommen
- **Datum:** 2026-11-03

## Kontext

Bis D1 setzte die RHI Barrieren selbst: `begin_rendering()` wechselte das Layout grob über alle
Stufen (TD-02), das Backend merkte sich das Layout jeder Textur. Für ein Dreieck und die Voxel-Demo
reichte das; mit Compute-Passes, Schattenkarten, HDR-Ziel und Tone Mapping (R1.3, R1.4) wird es
falsch oder teuer. Texturen gab es im Shader noch gar nicht. Pipelines hatten je ein eigenes Layout
nur aus Push Constants.

Die Mindestanforderung ([ADR 0017](0017-platform-baseline-feature-tiers.md)) enthält Descriptor
Indexing mit Update-After-Bind und Timeline Semaphores. Geometrie liest der Shader schon über
Adressen ([ADR 0022](0022-vertex-pulling-bda.md)).

## Entscheidung

**Zustände statt Layouts in der RHI.** `rhi::ResourceState` beschreibt die Nutzung einer Ressource
(`ColorTarget`, `ShaderRead`, `ShaderWrite`, `TransferDst`, `Present` …). `CommandList::barrier()`
nimmt Übergänge mit Vorher und Nachher; das Backend übersetzt sie in Stufen, Zugriffe und Layouts
von Synchronization2. Die RHI selbst merkt sich keine Zustände.

**Der Render-Graph kennt die Zustände.** Das neue Modul `ngine.render` baut jeden Frame einen Graph
aus Passes, die erklären, welche Texturen und Puffer sie in welchem Zustand lesen oder schreiben.
`compile()` verwirft Passes ohne Abnehmer, berechnet die Barrieren und die Lebensdauer transienter
Texturen, ohne die GPU zu brauchen. Die Reihenfolge bleibt die der Deklaration, es gibt eine Queue.

**Ein bindloser Descriptor-Satz.** Das Device hält genau einen Satz mit drei Feldern (Texturen zum
Lesen, Sampler, Storage-Texturen), alle mit `PARTIALLY_BOUND` und `UPDATE_AFTER_BIND`. Jede Textur
mit passender Verwendung bekommt beim Erzeugen einen festen Index. Alle Pipelines teilen ein Layout
aus diesem Satz und 128 Bytes Push Constants; der Satz wird einmal pro Command Buffer gebunden.
Puffer bleiben bei Geräteadressen.

**Lebensdauer über eine Timeline.** Jede Abgabe an die Queue signalisiert den nächsten Wert eines
Timeline Semaphores. Zerstörte Ressourcen und ihre bindlosen Indizes kommen in eine Löschliste und
werden frei, wenn die GPU den Wert ihrer letzten möglichen Nutzung erreicht hat. Frames im Flug,
Staging-Speicher für Uploads und der Neuaufbau der Swapchain hängen an derselben Zählung.

## Konsequenzen

- Barrieren sind so fein, wie die Zustände es erlauben, und an einer Stelle berechnet; die
  Synchronisationsprüfung der Validation Layers läuft in allen GPU-Tests mit.
- Wer ohne Graph aufzeichnet (Tests, Werkzeuge), setzt Barrieren selbst. Das ist gewollt: Es gibt
  keinen versteckten Zustand, der zwischen zwei Aufzeichnungen auseinanderlaufen kann.
- Shader brauchen keine Descriptor-Sets pro Draw; Materialien (R1.3) speichern nur Indizes. Ein
  falscher Index zeigt sich nicht als Validierungsfehler, sondern als falsches Bild oder
  GPU-Absturz.
- Ressourcen dürfen jederzeit zerstört werden; der Preis ist Speicher, der bis zu zwei Frames länger
  belegt bleibt.
- Pflicht werden zusätzlich: Update-After-Bind und nicht einheitliche Indizes für Storage-Texturen,
  Update-Unused-While-Pending sowie Lesen und Schreiben von Storage-Texturen ohne Format. Alle
  GPUs der Zielgruppe (ADR 0017) können das.
- Nicht enthalten und bei Bedarf später: Umordnen von Passes, asynchrones Compute auf einer zweiten
  Queue, Aliasing des Speichers transienter Texturen, transiente Puffer.

## Erwogene Alternativen

- **Zustände weiter im Backend verfolgen (wie bisher):** bequem für einfache Aufrufer, aber das
  Backend weiß nicht, wie eine Ressource als Nächstes benutzt wird, und muss grob absichern.
- **Descriptor-Sets pro Material oder Draw:** klassisch und gut von den Validation Layers geprüft,
  aber viel Verwaltung in RHI und Renderer und ein zweiter Weg neben dem Mesh-Shader-Pfad.
- **Descriptor Buffer (`VK_EXT_descriptor_buffer`):** noch direkter, aber nicht in der
  Mindestanforderung und auf Mesa und älteren Treibern lückenhaft.
- **Fertiger Render-Graph aus einer Bibliothek:** Es gibt keinen verbreiteten, der zur RHI passt;
  der eigene ist klein und ohne GPU testbar.
