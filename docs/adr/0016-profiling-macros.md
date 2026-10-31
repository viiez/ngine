# 0016: Profiling mit Tracy über Makros in einem Header

- **Status:** vorgeschlagen
- **Datum:** 2026-10-31

## Kontext

Wir wollen Leistung von Anfang an messen (Leitprinzip „Seriös und performant“), und das
Job-System macht die Engine parallel. Tracy zeigt Zonen, Threads, Frames und Werte live und hat
nahezu keinen Overhead. Eine Zone braucht aber statische Daten zu ihrer Quellstelle (Name, Funktion,
Datei, Zeile), die pro Aufrufstelle genau einmal existieren. Das geht nur mit einem Makro, und
C++-Module exportieren keine Makros. Gleichzeitig soll außer dem Adapter niemand Tracy kennen
(ADR 0009).

## Entscheidung

- `ngine.profile` ist die Schnittstelle: `SourceLocation`, die RAII-Klasse `Zone`,
  `set_thread_name()`, `frame_mark()`, `plot()` und `message()`.
- Zonen setzt man mit `NGINE_PROFILE_ZONE("Name")` bzw. `NGINE_PROFILE_ZONE_COLOR`. Die Makros
  stehen in `engine/profile/include/ngine/profile.hpp`, der einzigen eigenen Header-Datei. Sie
  enthält nur Makros, die eine statische `SourceLocation` und eine `Zone` anlegen; Tracy kommt darin
  nicht vor. Wer sie nutzt, bindet sie im globalen Modulfragment ein und importiert `ngine.profile`.
- Nur `profile_tracy.cpp` kennt Tracy und spricht dessen C-Schnittstelle an. `SourceLocation` hat
  dasselbe Layout wie Tracys Struktur; `static_assert`s sichern das ab.
- Tracy läuft im On-Demand-Modus: Der Client sammelt erst, wenn sich der Profiler verbindet.
- `NGINE_WITH_TRACY` (Standard `ON`) schaltet alles ab; dann sind die Makros leer und kosten nichts.
  Der ThreadSanitizer-Lauf baut ohne Tracy.

## Konsequenzen

- Zonen kosten ohne verbundenen Profiler einen Funktionsaufruf und eine Abfrage, mit Profiler etwa
  so viel wie Tracys eigene Makros plus einen Aufruf.
- Ein Wechsel zu einem anderen Profiler betrifft nur `profile_tracy.cpp` und, falls dessen
  Quellstellen anders aussehen, die Makros.
- Mit Tracy lauscht jedes Programm auf einem Netzwerkport, bis sich der Profiler verbindet. Für
  ausgelieferte Spiele und Server bauen wir später ohne Tracy oder nur mit `TRACY_ONLY_LOCALHOST`.
- GPU-Zonen fehlen noch; sie kommen mit dem Render-Graph (R1).

## Erwogene Alternativen

- **Tracy-Makros direkt im Code:** am einfachsten, aber jede Datei kennt Tracy (gegen ADR 0009).
- **Zonen über `std::source_location` ohne Makro:** Die Quellstelle müsste zur Laufzeit angelegt
  werden (`___tracy_alloc_srcloc`), das kostet bei jeder Zone eine Allokation.
- **Optick, Superluminal:** Optick wird kaum noch gepflegt, Superluminal kostet Geld und läuft nur
  unter Windows.
