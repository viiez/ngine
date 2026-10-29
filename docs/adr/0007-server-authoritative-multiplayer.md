# 0007: Server-autoritativer Multiplayer, Headless-Start

- **Status:** angenommen
- **Datum:** 2026-10-23

## Kontext

Das Spiel braucht Koop-Multiplayer wie Enshrouded und Valheim. Multiplayer nachträglich einzubauen,
gilt als einer der teuersten Umbauten einer Engine.

## Entscheidung

- Der Server ist autoritativ. Ein Spieler kann hosten (Listen-Server); dazu gibt es einen dedizierten
  Server ohne Grafik.
- Die Engine kann von Anfang an ohne Fenster und GPU starten (`--headless`).
- Spielzustand liegt in ECS-Komponenten, die sich replizieren lassen.

## Konsequenzen

- Alles unterhalb des Renderings muss headless laufen; das prüft das CI.
- Der Durchstich D3 klärt Transport und Replikation früh.

## Erwogene Alternativen

- **Peer-to-Peer:** schwerer gegen Schummeln und Desync abzusichern.
- **Multiplayer später nachrüsten:** zu riskant.
