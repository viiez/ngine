# 0010: Eigener Logger statt spdlog

- **Status:** angenommen
- **Datum:** 2026-10-27

## Kontext

Geplant war spdlog. Mit `std::format` aus C++20 ist ein schlanker Logger aber schnell geschrieben.

## Entscheidung

`ngine::log` ist ein eigener Logger mit Stufen und austauschbaren Sinks (Konsole als Standard).

## Konsequenzen

- Eine Abhängigkeit weniger, voller Einblick.
- Weitere Sinks (Datei, Editor-Konsole) bauen wir selbst; spdlog ließe sich als Sink anbinden.

## Erwogene Alternativen

- **spdlog:** ausgereift, aber für unseren Bedarf nicht nötig.
- **quill:** noch schneller, deutlich komplexer.
