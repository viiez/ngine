# 0051: Ein Zusammenbau der Simulation für alle Apps und ein Server im festen Takt

- **Status:** angenommen
- **Datum:** 2026-11-19

## Kontext

Bis R4.6 setzten die Apps die Simulation zweimal zusammen: `ModelScene` (Sandbox und Editor) mit
Renderer, und `playground.cppm` für `--headless`. Welt, Spielschleife, Physik, Skripte und
Animation kamen in R4 eins nach dem anderen dazu, jedes an beiden Stellen (TD-64). Der dedizierte
Server rechnete noch die Kugeln aus D3. R5 braucht eine Stelle, an der sich Replikation,
Vorhersage und Terrain übers Netz einhängen, und einen Server, der dieselbe Welt rechnet wie das
Spiel. Dazu muss jeder Spieler auf dem Server seine eigene Figur steuern, die Skripte lasen aber
nur eine Eingabe für alle.

## Entscheidung

- **`app::GameWorld`** in `apps/common` besitzt alles ohne GPU: Typen, Welt, Job-System,
  Ressourcen, Prefabs, `Simulation`, Physik mit Formen der Modelle, Ereignisse, Materialtabelle
  des Terrains, `AnimationSystem` und `ScriptSystem`. Es lädt den Spielplatz oder eine Szene und
  rechnet ohne Fenster ein Bild (`update()`). `ModelScene` legt Renderer, Audio, Kamera,
  Steuerung und Debug-Zeichnen darüber; `--headless --playground` und der Server nutzen es direkt.
- **Eingabe pro Figur:** `GameWorld::bind_player(figur, aktionen)` bindet ein `ActionState` an eine
  Figur. Skripte bekommen beim Aufruf die Entity, deren Skript gerade läuft (`Host.Current`), und
  `Input` liest die für sie gebundenen Aktionen, sonst die allgemeinen. Ohne Skripte steuert
  `steer_players()` die Figuren in C++. `ActionState::apply()` übernimmt Werte von außen.
- **Server im festen Takt:** Ein Bild des Servers ist genau ein fester Schritt zu 1/60 s; hinkt er
  hinterher, holt er ohne Pause auf. Damit hängt das Ergebnis nicht von der Uhr ab: Ohne Clients
  rechnet er bitgleich wie `--headless --playground`, nachgewiesen über `--track` und die Prüfsumme.
- **Figuren:** Der erste Client übernimmt die Figur aus der Szene, jeder weitere bekommt eine neue
  aus `prefabs/spielfigur.prefab`. Clients schicken pro Bild die Werte aller Aktionen
  (`replication::encode_actions()`, geprüft über `ActionMap::hash()`), der Server meldet ihnen ihre
  Figur (`encode_possess()`).
- **`jolt_world.cpp`** zerfällt in die interne Partition `ngine.physics:jolt` (gemeinsame Typen und
  `PhysicsWorld::Impl`) und Dateien nach Aufgabe: Körper, Figuren, Terrain, Abfragen.

## Konsequenzen

- Netzwerk-Stufen kommen an genau eine Stelle; Sandbox, Editor, `--headless` und Server rechnen
  dieselbe Welt in derselben Reihenfolge.
- Die Prüfsumme von `--headless --playground` rechnet seit R5.1 die Szene `szenen/spielplatz.json`
  mit Laterne, Spaziergängern und deren Skripten, nicht mehr die Welt aus Code; sie ist deshalb
  eine andere als bis R4.6.
- `Input` in Skripten ist jetzt eine Frage der Entity. Skripte an Dingen ohne eigene Aktionen
  (Laterne) lesen auf dem Server nichts; welcher Spieler sie bedient, klärt R5.5 mit Skripten im
  Netz.
- Die Eingabe geht in R5.1 jedes Bild unzuverlässig und ohne Nummer; Verlust kostet eine Flanke.
  Befehle pro Schritt mit Wiederholung kommen mit der Vorhersage (R5.3).
- Der Server zeichnet nicht und spielt keinen Klang; Posen rechnet er nur für `PoseMode::Always`.

## Erwogene Alternativen

- **Der Server baut die Systeme ein drittes Mal:** jetzt schneller, aber jede Netzwerk-Stufe müsste
  in drei Zusammenbauten gleich eingehängt werden.
- **Eine Welt pro Spieler für die Skripte oder eine Eingabe pro Skript-Instanz in C#:** mehr Umbau
  in der Laufzeit; eine Entity pro Aufruf reicht und braucht keine neue API für Skripte.
- **Server mit gemessener Bildzeit wie die Sandbox:** dann rechnet er je nach Last unterschiedlich
  viele Schritte pro Bild, `Update` der Skripte sieht andere Zeiten, und kein Lauf gleicht dem
  anderen.
