# 0053: Nummerierte Befehle mit Warteschlange, Vorhersage der eigenen Figur und Abgleich mit Glättung

- **Status:** angenommen
- **Datum:** 2026-11-22

## Kontext

Bis R5.2 schickte der Client pro Bild seine Aktionen (`GameMessage::Actions`) und seinen Blick
(`GameMessage::View`), und er sah seine Figur erst, wenn der Server sie bewegt und der
Schnappschuss 100 ms Puffer durchlaufen hatte ([ADR 0052](0052-replication-deltas-relevance.md)).
Bei 150 ms Latenz lief die eigene Figur gut eine Viertelsekunde hinterher. Der Server bleibt die
Wahrheit ([ADR 0007](0007-server-authoritative-multiplayer.md)); der Client soll trotzdem sofort
reagieren, mit derselben Physik ([ADR 0045](0045-physics-jolt-double-precision.md),
[ADR 0046](0046-terrain-collision-character-virtual.md)).

## Entscheidung

- **Befehle pro festem Schritt:** Ein `replication::Command` ist alles, was eine Figur in einem
  festen Schritt braucht: Nummer (`u32`, zählt der Client ab 1), Blick (`yaw`, `pitch`) und die
  Werte und Flanken aller Aktionen in der Reihenfolge der Belegung (`ActionState::fixed_values()`).
  Die Nachricht `Commands` trägt die neuesten drei Befehle (höchstens 8), pro Aktion ein Byte mit
  Flanken und dem Bit „Wert folgt“, nur dann `x` und `y` als `f32`. Sie ersetzt `Actions` und
  `View`; das Protokoll hat die Version 3. Der Client rechnet mit dem kodierten und wieder
  gelesenen Befehl, damit beide Seiten dieselben Werte sehen.
- **Warteschlange statt gemeinsamer Uhr:** Der Server legt Befehle pro Spieler in eine
  `CommandQueue` nach Nummer (Doppelte und zu späte verworfen, über 8 die ältesten) und nimmt pro
  festem Schritt genau einen. Er beginnt bei zwei wartenden; fehlt der nächste, gilt der letzte
  ohne Flanken unter der fehlenden Nummer. Mit jedem Schnappschuss meldet er die Länge der
  Schlange, und der Client stellt den Takt seiner festen Schritte um höchstens ±5 % nach
  (`Simulation::set_time_scale()`), bis zwei warten. Abweichung vom Plan R5, der gemeinsame Ticks
  und eine geschätzte Uhr des Servers vorsah.
- **Prüfen auf dem Server:** Lage und Geschwindigkeit kommen nie vom Client. `limit_command()`
  hält Achsen in `Action::limit`, Tasten in 0 bis 1 und `pitch` im erlaubten Bereich;
  `decode_commands()` verwirft nicht endliche Werte, unbekannte Flanken und falsche Belegungen.
- **Stand des Besitzers:** Mit jedem Schnappschuss bekommt der Besitzer einer Figur die Nachricht
  `Owner`: letzter verarbeiteter Befehl, Länge der Schlange und der Zustand der Figur danach (Lage
  als `f64`, Geschwindigkeit, Boden, geduckt), unabhängig vom quantisierten Transform.
- **Physik auf dem Client im Modus `remote`:** `PhysicsSettings::remote` bewegt nur Figuren mit
  der lokalen Komponente `physics::Predicted`; alle anderen Körper und Figuren sind kinematische
  Stellvertreter, die ihrem replizierten Transform folgen. Dafür gehen `Collider`, `RigidBody` (Art
  und Ebene) und `CharacterController` wieder an die Clients. Die Kollision des Terrains baut der
  Client selbst aus der replizierten Komponente `Terrain` (bitgleich nach ADR 0039).
- **Zustand und Neurechnen einer Figur:** `PhysicsWorld::character_snapshot()`,
  `set_character()` und `step_character()` lesen, setzen und rechnen eine einzelne Figur ohne
  Körper und ohne Ereignisse. Gesetzt wird über Lage, Geschwindigkeit und `RefreshContacts()` von
  Jolt plus den eigenen Teil (Boden, geduckt, Sprungwunsch), nicht über `SaveState()`.
- **`replication::Predictor`:** Ring der letzten 64 Befehle mit vorhergesagtem Zustand, ohne
  Physik (Lesen, Setzen und Schritt kommen als Rückrufe). Weicht der Stand des Servers um mehr als
  1 cm oder 5 cm/s ab, übernimmt er ihn für Befehl k und rechnet die gemerkten Befehle k+1 bis n
  neu; darunter bleibt die Vorhersage stehen.
- **Fremde Körper in der Vergangenheit:** Stellvertreter leichter Körper (unter einem Fünftel der
  Masse der Figur, etwa Kugeln), die auf dem Server dynamisch sind, bremsen die vorhergesagte Figur
  nur in Ruhe (unter 0,3 m/s, `CharacterBodyFilter`); bewegt fliegt sie durch, wie auf dem Server,
  wo der Körper abprallt. Schwere Körper (Kisten) halten die Figur immer auf, damit sie beim
  Schieben nicht hindurchläuft. Korrekturen zählt der
  Client nach Ursache (an Körpern, in der Luft, frei); dafür meldet `Owner`, ob die Figur einen
  beweglichen Körper berührt.
- **Glättung:** Den Sprung zwischen alter und neuer Vorhersage hält der Client als
  `Interpolated::offset` der Darstellung und baut ihn mit 60 ms Halbwertszeit ab; über 2 m setzt
  er sofort. Gerechnet wird ohne Versatz.
- **Spiegel und Darstellung:** `Mirror::set_predicted()` lässt `Transform`, `CharacterMotion` und
  `PlayerInput` der eigenen Figur in Ruhe. Die Steuerung rechnet auf dem Client
  `app::apply_motion()` in C++, auf dem Server das Skript (ohne .NET derselbe C++-Code). Die
  Animation der eigenen Figur setzt der Client selbst (`AnimationSystem::set_local()`).

## Konsequenzen

- Die eigene Figur reagiert ohne Verzögerung; der Client läuft dem Server um die halbe
  Rundlaufzeit plus zwei Schritte voraus, ohne Uhrenabgleich.
- Verlorene Befehle kosten nichts, solange eine der drei Kopien ankommt; fehlen alle, wiederholt
  der Server den letzten, und der Client wird höchstens einmal korrigiert.
- Bleibende Unterschiede zwischen Skript und C++ oder zwischen Compilern zeigt die Zahl der
  Korrekturen (Fenster „Netzwerk“, Log); die Toleranz verdeckt Rundung in der letzten Stelle.
- Fremde Körper stehen auf dem Client in der Vergangenheit; wer gegen eine bewegte Kiste läuft,
  wird korrigiert, sichtbar weich. Körper sagt der Client bewusst nicht voraus.
- Eine Korrektur kostet bei 150 ms rund 15 Schritte einer Figur in einem Bild.
- Kugeln und andere Würfe rechnet weiter nur der Server; sie erscheinen beim Werfer um die
  Rundlaufzeit verzögert.

## Erwogene Alternativen

- **Gemeinsame Ticks mit geschätzter Uhr des Servers** (Plan R5): braucht eine Schätzung von
  Rundlaufzeit und Versatz; die Länge der Warteschlange misst direkt, was zählt.
- **Abgleich auf dem Transform des Schnappschusses:** quantisiert auf Millimeter und ohne
  Geschwindigkeit und Boden, dazu nicht dem Befehl zugeordnet.
- **Client kennt nur das Terrain:** einfacher, aber die Figur liefe durch Kisten und Figuren, bis
  der Server korrigiert.
- **Vorhergesagte Figuren auch auf dem Server in C++ steuern:** sicherer gegen Abweichungen, aber
  das Skript verlöre die Bewegung.
- **`CharacterVirtual::SaveState()`/`RestoreState()`:** sichert mehr, als der Abgleich braucht,
  und lässt sich nicht aus dem `Owner`-Stand des Servers füllen; Lage, Geschwindigkeit und neu
  bestimmte Kontakte genügen.
