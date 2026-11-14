# 0044: Spielschleife mit Phasen, festem Zeitschritt, Interpolation und Befehlspuffer

- **Status:** angenommen
- **Datum:** 2026-11-13

## Kontext

Bis R3 rief jede Anwendung `Schedule::run()` und `update_transforms()` selbst auf und reichte die
Bildzeit per Referenz an ihre Systeme. Bewegung hing damit von der Bildrate ab, der Server rechnete
in einer eigenen Schleife, und Systeme durften keine Entities anlegen oder zerstören
([ADR 0019](0019-systems-on-job-system.md)). Für Physik (R4.2), Skripte (R4.4) und die
Netzwerk-Ticks mit Vorhersage (R5) braucht die Engine eine Simulation, die bei jeder Bildrate
dasselbe Ergebnis liefert, und einen Weg für Strukturänderungen aus Systemen heraus, der die
Reproduzierbarkeit nicht zerstört.

## Entscheidung

- **Phasen:** Jedes System gehört zu einer Phase (`Phase::Fixed`, `Phase::Frame`, `Phase::Late`;
  ohne Angabe `Frame`). `Schedule::run(jobs, phase)` führt die Systeme einer Phase aus.
  Abhängigkeiten aus Zugriffen gelten wie bisher, aber nur innerhalb einer Phase.
  `SystemOptions::after` erzwingt zusätzlich eine Reihenfolge nach dem Namen eines früheren
  Systems derselben Phase.
- **Spielschleife:** `world::Simulation` besitzt den Schedule und eine Uhr. `advance(dt)` sammelt
  Zeit an und rechnet so viele feste Schritte zu 1/60 s, wie hineinpassen, höchstens vier pro Bild;
  was darüber hinausgeht, verfällt. Danach laufen `Frame` und `Late` einmal, dann
  `update_transforms()` und die Interpolation. Pause, Zeitlupe und Einzelschritt gehören zur Uhr.
  Sandbox, Editor, `--headless` und `--server` nutzen dieselbe Schleife.
- **Zeit:** `world::Time` ist ein Wert der Welt (`World::time()`): Zeitschritt der laufenden Phase,
  Bildzeit ohne Zeitlupe, Nummer des festen Schritts, simulierte Zeit und `alpha`. Systeme lesen
  sie, statt die Zeit einzufangen. Die simulierte Zeit ist `tick × fixed_delta`, nicht eine Summe
  von Bildzeiten.
- **Interpolation nur für Entities mit `Interpolated`:** Vor jedem festen Schritt merkt sich die
  Komponente die Lage. Nach den Phasen setzt die Schleife `Transform` der bewegten Entities kurz
  auf die Mischung zwischen alter und neuer Lage mit `alpha`, rechnet `update_transforms()` (Kinder
  ziehen über die Hierarchie mit) und stellt `Transform` ohne Meldung an die Änderungserkennung
  wieder her. `WorldTransform` hält danach die gemischte Lage, `Transform` die Lage der Simulation,
  die gespeichert und repliziert wird. Ruhende Entities werden nicht angefasst.
- **Befehlspuffer:** Ein System nimmt `Commands&` als ersten Parameter und kann damit Entities
  anlegen (auch aus einem Prefab), zerstören, Komponenten setzen und entfernen und umhängen.
  Angelegte Entities bekommen sofort einen vorläufigen Griff (oberstes Bit gesetzt), den spätere
  Befehle desselben Puffers benutzen. Der Schedule teilt jeden Chunk in feste Stücke der Größe
  `grain` und gibt jedem Stück einen eigenen Puffer. Nach der Phase führt der Hauptthread alle
  Puffer aus, geordnet nach System, Chunk und Zeile, also unabhängig davon, welcher Thread welches
  Stück gerechnet hat. Werte liegen als Bytes mit ihrem `TypeInfo` in Blöcken, die nie umziehen
  und über Läufe hinweg wiederverwendet werden.

## Konsequenzen

- Dieselbe Szene ergibt nach zwei Sekunden bei 30, 144 und 240 Bildern pro Sekunde 120 feste
  Schritte und bitgleiche Komponenten (Test ohne GPU). Das ist die Grundlage für Physik und für die
  Vorhersage in R5.
- Unter 15 Bildern pro Sekunde läuft das Spiel langsamer, statt sich mit immer mehr Schritten pro
  Bild festzufahren.
- Gezeichnet wird bis zu einem festen Schritt (16,7 ms) hinter der Simulation. Für Eingaben, die
  sofort wirken sollen (Kamera), gibt es `Phase::Frame`.
- Wer eine Entity in `Phase::Fixed` bewegt, ohne ihr `Interpolated` zu geben, sieht sie springen;
  wer sie außerhalb der festen Schritte versetzt, sieht sie für einen Schritt gleiten. Physik-Körper
  bekommen die Komponente in R4.2 selbst.
- Die Mischung schreibt pro bewegter Entity zweimal `Transform` im Hauptthread. Bei tausenden
  Körpern ist das in R4.2 zu messen.
- Befehle an Entities, die im selben Lauf schon zerstört wurden, entfallen ohne Fehler.
- Wer `update_transforms()` nach der Schleife erneut aufruft (Renderer, Editor), verliert die
  Mischung nicht, weil die Rücknahme keine Änderung meldet. Wer `Transform` einer interpolierten
  Entity zwischen zwei Bildern liest, sieht die Lage der Simulation.

## Erwogene Alternativen

- **Variabler Zeitschritt:** einfacher, aber Physik und Vorhersage werden von der Bildrate
  abhängig und nicht reproduzierbar.
- **Interpolation für alle Entities:** Jede Entity bräuchte zwei Lagen und jedes Bild eine
  Neuberechnung aller Teilbäume; die meisten Entities stehen still.
- **Mischen von `WorldTransform` nach `update_transforms()`:** braucht eine zweite Propagation für
  die Kinder interpolierter Entities, die die Hierarchie nicht kennt. Die kurze Ersetzung von
  `Transform` nutzt die vorhandene Propagation samt Änderungserkennung.
- **Deferred-Modus von flecs als Befehlspuffer:** flecs ist nicht für Schreiben aus unseren Jobs
  gebaut, und die Reihenfolge hinge von der Verteilung der Jobs ab.
- **Ein gemeinsamer Puffer mit Sperre:** einfach, aber die Reihenfolge der Befehle hinge vom Zufall
  der Threads ab und die Sperre würde bei vielen Befehlen zum Engpass.
- **Eigenes Modul für die Schleife:** lohnt erst, wenn die Schleife mehr als die Welt treibt
  (Audio, Netzwerk); bis dahin gehört sie zu `ngine.world`.
