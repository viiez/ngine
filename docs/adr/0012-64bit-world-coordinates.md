# 0012: 64-Bit-Weltkoordinaten, kamerarelatives Rendering

- **Status:** angenommen
- **Datum:** 2026-10-23

## Kontext

Mit 32-Bit-Gleitkommazahlen beginnen Positionen in großer Entfernung vom Ursprung zu zittern. Das Spiel
soll große Welten haben.

## Entscheidung

Weltpositionen werden in 64 Bit gespeichert. Für das Rendering rechnen wir relativ zur Kamera in 32 Bit.
Die Idee stammt von Unigine.

## Konsequenzen

- Kein Zittern auch weit vom Ursprung.
- Physik und Rendering brauchen eine klare Umrechnung an ihren Grenzen.

## Erwogene Alternativen

- **Ursprung verschieben (Origin Rebasing):** funktioniert, ist im Multiplayer aber umständlich.
