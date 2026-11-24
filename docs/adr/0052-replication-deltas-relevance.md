# 0052: Replikation als Delta gegen den bestätigten Stand, Markierung in der Reflection

- **Status:** angenommen
- **Datum:** 2026-11-21

## Kontext

Seit D3 schickte der Server in jedem Tick den vollen Zustand aller Entities mit `Replicated`,
Felder ungepackt und nur die Komponenten einer Liste, die Server und Client gleich festlegten
([ADR 0024](0024-networking-gns-snapshots.md), TD-13). Schon 33 Entities überschritten ein
UDP-Paket. R5 braucht einen Spielplatz mit Terrain, Licht, Figuren und Hunderten Körpern bei
64 KB/s pro Client, über WLAN mit Verlust und Latenz.

## Entscheidung

- **Markierung in der Reflection:** `types.add<T>("T").replicated()` markiert eine Komponente,
  `.field(...).local()` nimmt ein Feld aus, `Hint::Entity` kennzeichnet Felder mit einer
  Entity-Kennung (`u64`), die der Client in seine eigene übersetzt. `replication::Schema` liest
  daraus die Liste der Komponenten (Reihenfolge der Registrierung) und einen Hash über
  Protokollversion, Namen, Felder und Codecs. Die Willkommensnachricht trägt den Hash; passt er
  nicht, trennt der Client. Repliziert werden alle Entities mit einer markierten Komponente, die
  Komponente `Replicated` trägt nur noch der Client.
- **Codecs:** Felder schreibt `serialize::write_binary`; `Transform` hat einen eigenen Codec:
  Position als Zelle zu 64 m (`i32`) plus Millimeter (`u16`) je Achse, Drehung als die drei
  kleinsten Komponenten zu je 15 Bit, Skalierung als drei `f32`. Jedes Feld (beim Transform
  Position, Drehung, Skalierung) ist ein Teil, und Deltas gehen pro Teil.
- **Delta gegen den bestätigten Stand:** Der Server liest pro Schnappschuss einmal die Welt in
  unveränderliche Stände pro Entity (`EntityRecord`, geteilt über `shared_ptr`; unveränderte
  behalten ihren Zeiger). Pro Client merkt er sich die letzten 32 gesendeten Stände (Views) und
  kodiert gegen den, den der Client zuletzt bestätigt hat: neue Entities voll, geänderte mit einer
  Maske der Teile, entfernte als Liste. Der Client bestätigt jeden angewendeten Schnappschuss mit
  einer eigenen Nachricht `Ack` und hält dieselben 32 Stände. Kennt er die Basis nicht, verwirft
  er den Schnappschuss, und der Server schickt ohne neue Bestätigung irgendwann voll.
- **Relevanz und Budget pro Client:** Relevant ist, was höchstens 150 m von der Figur entfernt
  ist, dazu Entities ohne Lage und solche mit einer globalen Komponente (`Terrain`,
  `DirectionalLight`). Bis 40 m jeder Schnappschuss, darüber jeder dritte. Ein Schnappschuss hat
  höchstens 64 KB/s geteilt durch 30 Hz; Geänderte warten sonst, Vorrang hat, wer am längsten
  wartet, dann wer am nächsten ist. Ohne bestätigte Basis zählt kein Warten, damit bis zur ersten
  Bestätigung immer dieselbe Auswahl geht.
- **Interpolation auf dem Client:** `Mirror::interpolate()` zeichnet Transforms 3 Ticks (100 ms)
  hinter dem neuesten Schnappschuss, mit einer Uhr, die sanft nachstellt; fehlen Stände, rechnet
  sie höchstens 3 Ticks weiter. Seit R5.3 überbrückt eine bewegte Entity Lücken bis 9 Ticks, statt
  stehen zu bleiben, und jede Entity bekommt eine zusätzliche Verzögerung aus dem Abstand ihrer
  Stände (bis 300 ms), damit selten oder unregelmäßig gesendete nicht zittern.
- **Blick vom Client:** Der Client dreht die Kamera selbst und schickt Gier- und Nickwinkel
  (`GameMessage::View`), statt dass der Server Mausbewegungen aufsummiert, die verloren gehen.
- **Simulierte Netzqualität:** `Transport::simulate()` stellt Verzögerung und Verlust von
  GameNetworkingSockets für den ganzen Prozess ein (`--net-sim`, Tests).

## Konsequenzen

- Neue Komponenten brauchen keinen Netzwerkcode, nur `.replicated()`; was nur lokal ist (Eingaben
  der Figur, Zwischenwerte), markiert `.local()`.
- Ein geänderter Typ ändert den Hash: Server und Client müssen aus demselben Stand gebaut sein.
- Statische Entities kosten nur beim Beitreten; in Ruhe ist ein Schnappschuss 11 Bytes.
- Der Server hält pro Client nur Zeiger, aber jede Änderung eines Teils kostet eine Kopie des
  Stands der Entity.
- `RigidBody` und `Collider` bleiben in R5.2 lokal: Der Client rechnet keine Physik, und
  Debug-Zeichnen der Kollision braucht er nicht.
- Die eigene Figur ist bis zur Vorhersage (R5.3) um Latenz plus Puffer verzögert.

## Erwogene Alternativen

- **Delta gegen den zuletzt gesendeten Stand:** kleiner, aber jeder Verlust zerbricht die Kette;
  der bestätigte Stand heilt von selbst.
- **Liste der Komponenten im Code wie in D3:** doppelt gepflegt, und Felder ließen sich nicht
  ausnehmen.
- **Bitgenaue Kodierung (Bitstream) statt Bytes:** einige Prozent kleiner, aber schwerer zu prüfen;
  erst, wenn die Messung es verlangt.
- **Eigene Kopie der Stände pro Client:** einfacher, aber Speicher und Zeit wachsen mit Clients mal
  Entities.
