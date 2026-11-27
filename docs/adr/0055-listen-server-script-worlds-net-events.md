# 0055: Listen-Server über ein Paar im Speicher, Skript-Domänen mit Rollen in einer Laufzeit, Ereignisse und Einmal-Klänge vom Server

- **Status:** angenommen
- **Datum:** 2026-11-27

## Kontext

Bis R5.4 gibt es einen dedizierten Server ohne GPU ([ADR 0051](0051-game-world-dedicated-server.md))
und Clients mit Spiegel, Vorhersage und Terrain-Empfänger
([ADR 0052](0052-replication-deltas-relevance.md), [ADR 0053](0053-commands-prediction-reconciliation.md),
[ADR 0054](0054-terrain-deltas-over-network.md)). Skripte liefen nur auf dem Server; Clients hatten
keine. Pro Prozess gibt es eine .NET-Laufzeit ([ADR 0023](0023-dotnet-hosting-hostfxr.md)) und
bisher genau eine gebundene Welt ([ADR 0047](0047-script-components-bindings-hot-reload.md)). Was
nur der Server erlebt (Klänge aus Skripten, Auslöser der Animation zwischen zwei Schnappschüssen),
erreichte keinen Client (TD-58, TD-61), und die Laterne sah auf dem Server keine Taste (TD-66). Für
das Szenario des Reifegrads R5 soll ein Spieler hosten und mitspielen, auch aus dem Editor.

## Entscheidung

- **Paar im Speicher:** `net::Transport::connect_local(Transport& server)` legt mit
  `CreateSocketPair` von GameNetworkingSockets zwei verbundene Enden an; eines meldet sich beim
  lauschenden Transport des Servers wie ein neuer Client. Der Host ist damit ein gewöhnlicher
  Client mit Spiegel, Vorhersage und Terrain-Empfänger, ohne eigenen Codepfad. Die Verbindung hat
  keine Latenz; `--net-sim` gilt nur für UDP.
- **Server als Baustein in `apps/common`:** `app::GameServer` (Spieler, Figuren, Befehle, Terrain,
  Ereignisse), `app::ServerSession` (Transport, `replication::Server`, ein Bild des Servers) und
  `app::ListenServer` (eigene `GameWorld` mit `deterministic`, Session, Paar im Speicher; pro Bild
  des Hosts so viele Server-Bilder, wie 1/60 s in die vergangene Zeit passen, höchstens vier).
  Beide Welten teilen sich das `JobSystem` des Hosts (`GameWorldDesc::jobs`). Dedizierter Server,
  Listen-Server, Sandbox `--host` und Tests nutzen dieselben Klassen.
- **Skript-Domänen:** Jedes `ScriptSystem` ist eine Domäne der einen Laufzeit
  (`ScriptRuntime::create_domain(role)`, `select_domain()`), mit eigenen Bindungen, Instanzen, Zeit
  und Typ-Indizes in der Registry seiner Welt. Vor jedem Aufruf in .NET wählt die Laufzeit die
  Domäne an einer Stelle aus (Zeiger auf der nativen Seite, `Host.Domain` auf der verwalteten).
  Bauen, Laden und Hot Reload bleiben einmal pro Prozess; beim Neuladen werden die Felder aller
  Domänen gesichert und die Klassen in jeder Registry neu angemeldet.
- **Rollen:** `Local` (ohne Netz, alle Skripte), `Server` (Klassen ohne `[Client]`) und `Client`
  (Klassen mit `[Client]` oder `[Everywhere]`). Ohne Attribut läuft ein Skript nur auf dem Server;
  `Net.Role`, `Net.IsServer` und `Net.IsClient` fragen die Rolle ab. Clients laden dieselbe Assembly
  wie der Server, damit das Schema übereinstimmt. Auf Clients übernimmt kein Skript die Steuerung
  der Figur; dort steuert die Vorhersage.
- **Replizierte Skript-Komponenten:** `[Replicated]` an der Klasse entspricht `.replicated()`,
  `[Local]` an einem Feld `.local()`. Klassen, die auf Clients laufen (`[Client]`, `[Everywhere]`),
  sind immer repliziert, weil ihre Komponente dort sonst fehlt. Der Server schreibt die Felder vor
  jedem Schnappschuss in die Komponenten; der Client sichert sie vor einem Schnappschuss und liest
  sie danach zurück. Schreibt ein Client-Skript in eine replizierte Komponente, steht einmal pro
  Typ und Klasse eine Warnung im Log; der nächste Schnappschuss überschreibt den Wert.
- **Ereignisse vom Server:** Nachricht `Event` (zuverlässig) mit Tick, Entity, Name (höchstens 64
  Bytes) und einem `f32`. `Net.Broadcast(entity, name, value)` geht an alle Clients, für die die
  Entity relevant ist, `Net.Send(player, entity, name, value)` an den Besitzer der Figur `player`.
  Auf dem Client bekommen die Skripte der Entity `OnNetEvent(name, value)`.
- **Einmal-Klänge:** `Audio.Play` auf dem Server wird zur Nachricht `Sound` (unzuverlässig) mit
  Tick, Klang, Ort, Bus, Lautstärke und Tonhöhe, an Clients mit Figur in Hörweite (50 m), ohne Ort
  an alle.
- **Im Takt der Interpolation:** Der Client stellt Ereignisse und Klänge zu, wenn
  `Mirror::render_tick()` ihren Tick erreicht; ist die Entity 30 Ticks danach noch nicht im
  Spiegel, wird das Ereignis verworfen und gezählt. Protokoll Version 5.
- **Schritte auf dem Client:** Jeder Client rechnet die Automaten aller Figuren; `Schritte` ist
  `[Client]`, `CharacterMotion::ground_material` ist repliziert. Schritte kosten kein Byte im Netz.
- **Eingabe pro Figur:** `Input.For(entity)` liest die Aktionen des Spielers, dem die Figur gehört
  (ohne Netz die allgemeinen). Die Laterne prüft so die Taste jeder Figur in Reichweite (TD-66).
- **Auslöser als Zähler:** `AnimatorParameters` hält pro Auslöser einen Zähler (vier mal acht Bit);
  der Client löst aus, wenn er sich ändert (Teil von TD-61).
- **Klänge freigeben:** Das `AudioSystem` gibt einen Klang frei, der 30 s keine Stimme hatte
  (TD-57).
- **Editor:** „Spielen als Listen-Server“ legt die Server-Welt aus der Sicherung des Dokuments an
  (`SceneDocument::copy_play_backup()`, mit Deltas des Terrains), leert die Welt des Dokuments
  (`clear_while_playing()`) und macht sie zum Spiegel des Clients. Ein zweites, gesperrtes Dokument
  über der Server-Welt zeigt Szenenbaum und Inspector auf Wunsch dort. Beim Stoppen kommt die Welt
  bitgenau zurück ([ADR 0032](0032-play-in-editor-binary-snapshot.md)).

## Konsequenzen

- Host, entfernte Clients und Tests laufen über denselben Code; Fehler im Client-Pfad sieht man
  schon beim Hosten.
- Der Host rechnet zwei Welten; Speicher und Rechenzeit der Server-Welt kommen dazu (gemessen in
  der Abnahme von R5.5).
- Skripte müssen wissen, wo sie laufen. Darstellung und Klang gehören in `[Client]`, Spiellogik
  ohne Attribut auf den Server; `[Everywhere]` fragt `Net.Role`.
- Ohne .NET auf dem Client passt das Schema nicht, sobald der Server Skript-Komponenten repliziert;
  der Client bekommt beim Verbinden eine Meldung.
- Die Warnung beim Schreiben replizierter Komponenten trifft auch `Get<T>()` ohne Schreiben
  (TD-68); Skripte auf Clients lesen mit `Read<T>()`.
- Ereignisse vom Client an den Server gibt es außer den Befehlen nicht.

## Erwogene Alternativen

- **Eigene Verbindung im Speicher ohne die Bibliothek:** unabhängig von GameNetworkingSockets, aber
  mehr Code und ein zweiter Weg für Zuverlässigkeit und Trennung.
- **Dedizierter Server als zweiter Prozess über localhost:** keine Skript-Domänen nötig, aber kein
  Blick in die Server-Welt im Editor und zwei Prozesse zum Debuggen.
- **Schritte als Einmal-Klänge vom Server:** einfacher, aber die eigenen Schritte kämen eine
  Rundlaufzeit zu spät, und viele Spieler kosten dauernd Bandbreite.
- **Ereignis „benutzen“ vom Client:** eigene Nachricht und eigene Prüfung statt der Befehle, die
  der Server ohnehin hat.
- **Editor zeigt die Server-Welt und steuert die Figur direkt:** ein Weg mit Sonderfällen, den
  Punkt 7 des Plans von R5 ausgeschlossen hat.
