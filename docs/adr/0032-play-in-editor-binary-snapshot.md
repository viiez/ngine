# 0032: Spielen im Editor über eine binäre Sicherung der Welt im Dokument

- **Status:** angenommen
- **Datum:** 2026-11-07

## Kontext

„Spielen“ im Editor soll die Systeme laufen lassen und beim Stoppen genau den Stand von vorher
wiederherstellen (EDIT-06, [R2.3](../iterations/R2.3-gizmos-und-spielen.md)). Laut Plan von R2
sichert der Editor dafür die Welt im Binärformat, statt eine zweite Welt zu kopieren. Offen war,
wo die Sicherung liegt, was mit Verlauf, Auswahl und stabilen Kennungen geschieht und wie „genau
wie vorher“ geprüft wird. Wer die Welt am Dokument vorbei ändert, macht laut
[ADR 0030](0030-undo-commands-reflected-values.md) den Verlauf ungültig.

## Entscheidung

- **Sicherung im Dokument:** `SceneDocument::begin_play()` hält alle Entities in aufsteigender
  Reihenfolge ihrer Kennungen in der Welt fest, jeweils mit Kennung im Dokument, Elternteil und
  allen reflektierten Komponenten als Bytes im Binärformat (auch Laufzeitkomponenten wie
  `MeshRenderer` und Zustand wie `Orbit::time`). `end_play()` zerstört die Welt, legt alle
  Entities vorab an, vergibt sie aufsteigend in derselben Reihenfolge, setzt Eltern und
  Komponenten und verknüpft die alten Kennungen im Dokument wieder.
- **Gesperrtes Dokument:** Während des Spielens ist das Dokument nur lesbar (`read_only()`):
  keine Befehle, kein Undo und Redo, kein Speichern. Die Systeme ändern die Welt am Verlauf
  vorbei; weil der Stand danach bitgenau zurückkommt und Befehle nur stabile Kennungen kennen,
  gilt der Verlauf nach dem Stoppen weiter.
- **Prüfung:** Tests vergleichen die Welt vor dem Spielen und nach dem Stoppen Byte für Byte und
  die gespeicherte Szene Text für Text; die CI spielt im Editor einige Frames und vergleicht die
  danach gespeicherte Szene mit der vorher gespeicherten.
- Die Systeme gehören zur Szene (im Editor die Standardszene mit `Orbit`), nicht zum Dokument;
  der Editor ruft sie nur beim Spielen auf. C#-Skripte kommen mit R4 dazu.

## Konsequenzen

- Spielen ist einfach und deckt Fehler im Serialisieren früh auf: Was das Binärformat nicht
  vollständig festhält, fällt beim Stoppen auf.
- Zustand außerhalb der reflektierten Komponenten (z. B. in Systemen selbst oder im Renderer) wird
  nicht gesichert. Systeme halten ihren Zustand deshalb in Komponenten.
- Kennungen in der Welt sind nach dem Stoppen andere (gleiche Reihenfolge, neue Generation);
  wer sie außerhalb des Dokuments festhält, muss sie neu holen.
- Änderungen während des Spielens (wie in Unity, die nach dem Stoppen verfallen) gibt es noch
  nicht; Inspector und Szenenbaum sind solange ausgegraut.
- Bei großen Welten kostet Sichern und Wiederherstellen Zeit proportional zur Welt; für R2 reicht
  das, gemessen wird mit Tracy, wenn Szenen groß werden.

## Erwogene Alternativen

- **Zweite Welt als Kopie, beim Stoppen tauschen:** schnelleres Stoppen, aber Renderer, Dokument
  und Fenster müssten die Welt wechseln, und flecs-Welten lassen sich nicht einfach kopieren.
- **Szene als JSON sichern:** wiederverwendet `save_scene()`, verliert aber Laufzeitkomponenten
  und rundet nicht bitgenau zurück.
- **Verlauf beim Spielen verwerfen (`reset_history()`):** einfacher, aber ärgerlich: Wer kurz
  spielt, verliert sein Undo.
