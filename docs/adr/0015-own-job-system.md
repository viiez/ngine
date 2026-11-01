# 0015: Eigenes Job-System mit Work-Stealing, ohne Fibers

- **Status:** angenommen
- **Datum:** 2026-10-31

## Kontext

NGine soll alle Kerne nutzen („Seriös und performant“). Voxel-Meshing, Kollision, Beleuchtung,
Animation und später der Render-Graph zerfallen in viele kleine, teils voneinander abhängige
Aufgaben (ADR 0005). Das Job-System bestimmt damit, wie schnell die Engine ist, und es ist eines der
Themen, die wir mit dem Projekt verstehen wollen.

## Entscheidung

- Wir schreiben das Job-System selbst (`ngine.jobs`) statt eine Bibliothek einzubinden.
- Scheduling über **Work-Stealing**: Jeder Worker und der Thread, der das `JobSystem` erzeugt, haben
  eine lock-freie Chase-Lev-Deque (Speicherordnung nach Lê et al. 2013, mit seq_cst-Zugriffen statt
  Zäunen, damit ThreadSanitizer sie prüfen kann). Jobs aus anderen Threads landen in einer
  gemeinsamen Warteschlange mit Sperre.
- Ein Worker pro Kern minus eins; der erzeugende Thread arbeitet mit, sobald er wartet. Ohne Arbeit
  schlafen die Worker über `std::atomic::wait`.
- **Kein Heap pro Job:** Ein Job ist zwei Cache-Zeilen groß und speichert sein Funktionsobjekt
  (bis 80 Bytes) inline. Jobs kommen aus einem Pool pro Thread und kehren nach der Ausführung
  dorthin zurück, aus fremden Threads über einen lock-freien Stapel.
- **Abhängigkeiten** auf zwei Ebenen: `Counter` zum Warten auf eine Gruppe von Jobs und `TaskGraph`
  für Aufgaben, die erst nach ihren Vorgängern starten. Ein Graph wird einmal gebaut und kann oft
  laufen. Dazu kommt `parallel_for`, das einen Bereich rekursiv halbiert.
- `wait()` blockiert nicht, sondern arbeitet andere Jobs ab. Jobs dürfen deshalb selbst warten.
- **Keine Fibers.** Jobs laufen auf normalen Threads bis zum Ende.
- ThreadSanitizer prüft Job-System und Allokatoren in der CI.

## Konsequenzen

- Wir kontrollieren Speicher, Scheduling und Profiling-Zonen vollständig und können das System an
  Voxel und Render-Graph anpassen.
- Lock-freier Code ist schwer richtig zu bekommen. Stresstests und ThreadSanitizer sind Pflicht;
  Änderungen an der Deque brauchen besondere Sorgfalt im Review.
- Ohne Fibers kann ein Job, der lange wartet, seinen Thread nicht abgeben. Wartet er per `wait()`,
  arbeitet er stattdessen andere Jobs ab; das kann den Stack vertiefen und die Reihenfolge
  verändern. Für E/A und lange Aufgaben (Streaming) planen wir eigene Threads statt Jobs.
- Funktionsobjekte über 80 Bytes lehnt der Compiler ab. Größere Daten übergibt man per Zeiger.

## Erwogene Alternativen

- **enkiTS:** klein, erprobt, ähnlicher Ansatz. Wir würden aber weniger über Scheduling lernen und
  Speicher sowie Profiling weniger selbst bestimmen.
- **Taskflow:** sehr gute Graph-API, aber großer Header-Code und eigene Allokationen pro Task.
- **oneTBB:** ausgereift, aber schwergewichtig und mit eigenem Speicher- und Thread-Modell.
- **Fibers (wie bei Naughty Dog):** erlauben Warten ohne Thread-Blockade, brauchen aber
  plattformspezifischen Code (Assembler bzw. Windows-Fibers) und erschweren Debugger und Tracy.
  Wir prüfen sie erneut, falls Wartezeiten in Jobs zum Engpass werden.
- **Eine globale Warteschlange mit Sperre:** einfach, skaliert aber schlecht mit vielen Kernen.
