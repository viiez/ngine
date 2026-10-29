# 0001: Dokumentation als Code im Repository

- **Status:** angenommen
- **Datum:** 2026-10-29

## Kontext

Vision, Entscheidungen und Planung entstanden zuerst in einem Planungsdokument außerhalb des
Repositorys. Mit wachsendem Code veraltet so etwas schnell, und es ist nicht versioniert.

## Entscheidung

Die Dokumentation liegt als Markdown unter `docs/` und ändert sich im selben Pull Request wie der Code.
Entscheidungen halten wir als ADRs fest, Diagramme als Mermaid. Die Definition of Done verlangt
aktualisierte Doku.

## Konsequenzen

- Doku ist versioniert, reviewbar und auf GitHub lesbar.
- Jeder PR kostet etwas mehr Arbeit, dafür bleibt die Doku aktuell.
- Planungsdokumente außerhalb des Repositorys gelten nur noch als Vorarbeit.

## Erwogene Alternativen

- **Externes Dokument oder Wiki:** bequem zum Schreiben, veraltet aber unbemerkt.
- **Generierte Website (MkDocs, Doxygen):** später möglich; reines Markdown reicht vorerst und lässt
  sich dafür weiterverwenden.
