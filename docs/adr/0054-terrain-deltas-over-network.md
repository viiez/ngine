# 0054: Terrain übers Netz als Deltas vom Server mit Vorrang nach Abstand und vorhergesagtem Pinsel

- **Status:** angenommen
- **Datum:** 2026-11-24

## Kontext

Seit R3.4 hält jedes Terrain seine Änderungen als Deltas gegenüber der prozeduralen Grundwelt,
pro Chunk der Stufe 0 als Lauflängen kodiert ([ADR 0042](0042-terrain-deltas-region-files.md)).
Bis R5.3 baut jeder Client sein Terrain aus der replizierten Komponente `Terrain` selbst
(bitgleich nach [ADR 0039](0039-deterministic-noise-rule-generator.md)), kennt aber keine
Änderungen; der Server schaltete das Graben von Clients deshalb ab. Der Server ist die Wahrheit
([ADR 0007](0007-server-authoritative-multiplayer.md)), die eigene Figur rechnet der Client voraus
([ADR 0053](0053-commands-prediction-reconciliation.md)). Pinsel rechnen in `f32` mit `sqrt` und
`floor` und sind nicht bitgleich über Compiler.

## Entscheidung

- **Graben als Aktion im Befehl:** Die Aktion `dig` steckt im Befehl jedes festen Schritts. Der
  Server wirft den Strahl selbst aus dem Auge seiner Figur in ihrem Blick (Skript Spielersteuerung
  bzw. `GameWorld::dig_players()` mit `app::figure_dig_target()`), so lang wie
  `PlayerInput::dig_reach`, im Takt der Grabpause (`app::DigTimer`, 0,15 s). Reichweite, Takt,
  Radius und Material bestimmt der Server; einen Zielpunkt vom Client gibt es nicht.
- **Deltas statt Pinsel:** Der Server schickt die Deltas geänderter Chunks ganz, nicht als
  Differenz zur vorigen Fassung, in der zuverlässigen Nachricht `GameMessage::Terrain`: Kennung des
  Terrains auf dem Server, letzter verarbeiteter Befehl des Empfängers, Flag „Umgebung
  vollständig“, dann je Chunk Koordinate (ZigZag-Varints) und Delta mit Länge, Länge 0 für die
  Grundwelt. Protokoll Version 4.
- **`replication::TerrainSender`:** merkt sich pro Client und Terrain den Zeiger des zuletzt
  geschickten Deltas jedes Chunks; Deltas sind unveränderlich und geteilt, ein Zeigervergleich
  genügt. Ändert sich die Momentaufnahme, vergleicht er sie mit der vorigen und merkt die
  geänderten Chunks bei allen Clients vor; ein neuer Client hat alle vorgemerkt. Mit jedem
  Schnappschuss (30 Hz) geht pro Client und Terrain höchstens eine Nachricht.
- **Vorrang und Budget:** Vorgemerkte Chunks gehen nach Abstand zur Figur. In der Relevanz (150 m)
  gehen sie sofort und ohne Budget, alles andere teilt sich 128 KB/s pro Client (mindestens ein
  Chunk pro Nachricht) neben den 64 KB/s der Schnappschüsse
  ([ADR 0052](0052-replication-deltas-relevance.md)). Die erste Nachricht, nach der in der
  Relevanz nichts mehr fehlt, trägt „Umgebung vollständig“; bis dahin rechnet der Client seine
  Figur nicht voraus und zeigt sie wie fremde Figuren.
- **Bestätigung:** Jede Nachricht trägt die Nummer des letzten Befehls des Empfängers, die der
  Server verarbeitet hatte. Weil sie zuverlässig und geordnet ist und Änderungen in der Relevanz nie
  warten, sind alle Änderungen bis zu diesem Befehl beim Client, wenn die Bestätigung ankommt. Hat
  der Client seit der letzten Nachricht gegraben, schickt der Server die Bestätigung auch ohne
  Chunks.
- **`replication::TerrainReceiver`:** hält Nachrichten zurück, bis der Spiegel das Terrain kennt und
  es Laufzeitdaten hat, und setzt die Deltas über `world::restore_terrain()`. Den bestätigten Stand
  hält er getrennt vom Terrain. Legt der Spiegel das Terrain neu an, bekommt es alles Bestätigte.
- **Vorhergesagter Pinsel:** Gräbt der vorhergesagte Befehl n, wendet der Client denselben Pinsel
  sofort an (`TerrainReceiver::predict()`) und merkt sich Befehl und Pinsel. Bestätigt eine
  Nachricht k ≥ n, gehen alle berührten Chunks auf den bestätigten Stand, und die offenen Pinsel
  nach k werden neu angewendet. Pinsel, die der Server nach dem Stand der Figur (`Owner`) seit 60
  Befehlen verarbeitet haben müsste, verfallen.
- **Grobe Stufen über einen Index** (TD-41): `TerrainDeltas` hält neben der `DeltaMap` einen
  `CoarseDeltas`-Index mit den geänderten Samples auf dem Raster jeder Stufe 1 bis 10, nach grobem
  Chunk und Herkunft; Tabelle und Index sind zusammen unveränderlich (`DeltaState`) und werden mit
  den Jobs geteilt. `apply_deltas()` über den Index ist bitgleich zum Weg über alle Deltas.

## Konsequenzen

- Alle Clients sehen dieselben Gruben wie der Server; Kollision und Kisten rechnet der Server,
  Netze und Kollision der Clients folgen den Deltas.
- Ein Pinselstrich kostet einige hundert Bytes bis wenige KB je nach Zahl der Chunks; ein Beitritt
  kostet alle Deltas der Welt, die Umgebung der Figur zuerst.
- Gräbt der Server anders als der Client (Strahl traf beim Server eine Kiste), flackert die
  vorhergesagte Grube kurz; der Client zählt solche Pinsel als abweichend.
- Wer mehr als 1 MB Deltas hat, wartet beim Beitritt gut acht Sekunden, bis alles da ist; die
  Umgebung ist meist nach einem Bruchteil davon vollständig.
- Ohne Terrain in der Welt wartet die Vorhersage nicht. Gibt es eines, wartet sie auf die erste
  Nachricht mit „Umgebung vollständig“.

## Erwogene Alternativen

- **Pinsel als Befehle an alle Clients:** kleiner pro Grube, verlangt aber bitgleiche Pinsel auf
  allen Plattformen (Plan R5, Punkt 5).
- **Nachricht `Dig` mit Zielpunkt:** trifft genau, was der Client sah, vertraut ihm aber mehr und
  braucht eine zweite Prüfung des Takts.
- **Ein gemeinsames Budget von 64 KB/s für alles:** Beitritte nach viel Graben dauerten deutlich
  länger, und Schnappschüsse müssten warten.
- **Bestätigung über `Owner`:** unzuverlässig und nicht geordnet mit den Deltas; unter Verlust käme
  die Bestätigung vor der Grube, und die vorhergesagte Grube flackerte.
- **Nur ein räumlicher Index der Chunks mit Delta für TD-41:** weniger Speicher, aber grobe Chunks
  läsen weiter jedes Sample der überlappenden Deltas.
