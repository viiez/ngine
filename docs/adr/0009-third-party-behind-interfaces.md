# 0009: Fremdbibliotheken nur hinter Engine-Schnittstellen

- **Status:** angenommen
- **Datum:** 2026-10-23

## Kontext

Wir wollen nur das Wesentliche selbst bauen und den Rest einbinden, aber jede Bibliothek später durch
eine eigene Implementierung ersetzen können.

## Entscheidung

Jede Fremdbibliothek steckt in einem Adapter hinter einer Engine-Schnittstelle. Der Adapter bindet sie
im globalen Modulfragment ein und exportiert nur Engine-Typen. Kein anderer Code kennt die Bibliothek.

## Konsequenzen

- Bibliotheken bleiben austauschbar, und ihre Typen sickern nicht durch die Engine.
- Etwas Mehraufwand für die Schnittstellen.

## Erwogene Alternativen

- **Bibliotheken direkt nutzen:** schneller am Anfang, Austausch später sehr teuer.
