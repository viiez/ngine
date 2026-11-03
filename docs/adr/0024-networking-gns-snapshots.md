# 0024: Netzwerk über GameNetworkingSockets, Replikation per Schnappschuss über die Reflection

- **Status:** angenommen
- **Datum:** 2026-11-01

## Kontext

Der Multiplayer ist server-autoritativ ([ADR 0007](0007-server-authoritative-multiplayer.md)).
Der Durchstich D3 sollte klären, welche Transportbibliothek wir nutzen, wie Spielzustand vom
Server zum Client kommt und ob die Reflection ([ADR 0018](0018-own-reflection.md)) dafür reicht.
Anforderungen: UDP mit zuverlässigen und unzuverlässigen Nachrichten, Verschlüsselung, Linux und
Windows, ohne Fenster und GPU lauffähig, Pakete aus vcpkg, freie Lizenz.

## Entscheidung

- **Transport:** GameNetworkingSockets (BSD-3, Valve) hinter `ngine::net::Transport`
  ([ADR 0009](0009-third-party-behind-interfaces.md)). Aus vcpkg ohne das Feature `ice` (P2P über
  STUN/TURN brauchen wir für dedizierte Server nicht). Ohne Steam gibt es keine Zertifikate;
  Verbindungen sind verschlüsselt, aber die Gegenseite ist nicht beglaubigt
  (`IP_AllowWithoutAuth`). Rückrufe laufen nur in `Transport::poll()`, im Thread des Aufrufers.
- **Replikation:** Der Server schickt mit fester Rate (in D3 30 Hz) einen vollen Schnappschuss,
  unzuverlässig: alle Entities mit der Komponente `Replicated` und von ihnen die Komponenten einer
  festen Liste. Die Felder schreibt die Reflection (`serialize::write_binary`), Komponenten
  brauchen also keinen eigenen Netzwerkcode. Der Client spiegelt den Schnappschuss in seine Welt
  (`replication::Mirror`) und ordnet Server-Kennungen eigenen Entities zu.
- **Protokoll:** Jede Nachricht beginnt mit einem Byte für die Art (Willkommen, Schnappschuss,
  Nutzdaten der Anwendung). Die Willkommensnachricht (zuverlässig) trägt die Protokollversion und
  die Namen der replizierten Komponenten; passt die Liste nicht zum Client, trennt er.
- **Eingaben** vom Client sind Nutzdaten der Anwendung; der Server wertet sie aus und bewegt die
  Spielfigur selbst.

## Konsequenzen

- Transport, Replikation und dedizierter Server laufen ohne Fenster und GPU; die CI startet Server
  und Client unter Linux und Windows als zwei Prozesse.
- Volle Schnappschüsse sind robust (Verluste heilen mit dem nächsten Tick, kein Bestätigen nötig),
  aber teuer: 33 Entities mit Name und Position brauchen rund 1,2 KB pro Tick. Deltas gegen
  bestätigte Schnappschüsse, Quantisierung und Relevanz kommen in R5
  ([TD-13](../tech-debt.md)).
- Weil das Binärformat keine Feldnamen trägt, müssen Server und Client derselbe Build sein. Die
  Willkommensnachricht prüft nur die Namen der Komponenten, nicht ihre Felder; ein Hash über die
  Felder ist eine kleine Erweiterung.
- GameNetworkingSockets zieht Protobuf und OpenSSL mit. Unter Linux linkt vcpkg statisch, unter
  Windows liegt `GameNetworkingSockets.dll` neben der ausführbaren Datei.
- Die Bibliothek wählt keinen freien Port selbst (Port 0 wird abgelehnt). `listen(0)` probiert
  deshalb zufällige Ports aus dem dynamischen Bereich.
- Qualität und Datenraten misst die Bibliothek in Intervallen von einigen Sekunden; davor meldet
  `stats()` keine Werte.

## Erwogene Alternativen

- **ENet:** klein und bewährt, aber ohne Verschlüsselung und ohne Fragmentierung großer
  unzuverlässiger Nachrichten; das müssten wir selbst bauen.
- **yojimbo:** gutes Protokoll, aber Verbindungen über Tokens eines Matchmakers und weniger
  gepflegt; nicht in vcpkg.
- **Eigenes UDP-Protokoll:** viel Lernwert, aber Zuverlässigkeit, Staukontrolle und
  Verschlüsselung sind Monate Arbeit, bevor das Spiel davon etwas hat.
- **Replikation über Ereignisse (jede Änderung einzeln, zuverlässig):** wenige Bytes, solange sich
  wenig ändert, aber Reihenfolge und Verlust machen es fehleranfällig; Schnappschüsse mit späteren
  Deltas sind der übliche Weg für Spiele mit vielen bewegten Objekten.
- **flecs-eigene Serialisierung:** würde flecs außerhalb des Adapters sichtbar machen
  ([ADR 0018](0018-own-reflection.md)).
