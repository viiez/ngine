# 0047: Skript-Klassen als Komponententypen, Bindings aus der Reflection, Hot Reload über dotnet build

- **Status:** angenommen
- **Datum:** 2026-11-14

## Kontext

Seit D2 läuft .NET 10 in der Engine ([ADR 0023](0023-dotnet-hosting-hostfxr.md)): Skripte lassen
sich aus einem entladbaren Kontext laden und wieder entladen, sie sehen aber nur ein Array von
Positionen. Für R4.4 sollen Skripte an Entities hängen, Komponenten ohne Kopie lesen und ändern,
Eingabe und Physik nutzen, im Inspector, in Szenen und Prefabs wie Komponenten erscheinen und sich
beim Speichern einer `.cs`-Datei im laufenden Spiel neu laden, ohne ihre Werte zu verlieren. Die
Abstimmung zum Plan (#54) hat die Richtung festgelegt; dieses ADR hält fest, wie es gebaut ist.

## Entscheidung

- **Bindings aus der Reflection:** `ngine_bindgen` registriert beim Bauen die Typen der Engine und
  schreibt `Components.g.cs`: pro Struct ein C#-Struct mit `LayoutKind.Explicit`, Größe und
  `[FieldOffset]` aus der `TypeInfo`, Standardwerten aus C++ im Konstruktor ohne Parameter, pro
  Enum ein C#-Enum mit gleich großem Grundtyp. Texte (`std::string`) fehlen im Struct, ihr Platz
  bleibt frei; Typen mit Texten lassen sich lesen (`Read`) und als Ganzes setzen (`Set`), aber
  nicht per Referenz holen. Mathe-Typen (`Vec3`, `Quat`, `DVec3`, …) sind von Hand geschrieben.
  Beim Start meldet C# Größe und Offsets aller Structs und die Größe aller Enums; die Engine
  vergleicht sie mit der Reflection und lädt bei einer Abweichung keine Skripte. Komponenten der
  Anwendung (z. B. `PlayerInput`) erzeugt die Engine zur Laufzeit aus derselben Registry als
  `Komponenten.g.cs` im Cache; das Skript-Projekt bindet sie über `$(NgineBindings)` ein.
- **API von Hand:** Funktionen kennt die Reflection nicht. Die Tabelle `NativeApi` (Welt, Skripte,
  Eingabe, Physik) bleibt von Hand geschrieben, auf beiden Seiten mit gleicher Anordnung. `ref T
  Get<T>()` zeigt direkt in die Spalte der Welt; Strukturänderungen aus Skripten (`Create`, neue
  Komponente, `Remove`, `Destroy`, Eltern, Name) gehen in einen Befehlspuffer, der nach der Phase
  läuft ([ADR 0044](0044-game-loop-phases-fixed-step-commands.md)).
- **Jede Skript-Klasse ist ein Typ der Registry:** Beim Laden meldet C# jede Klasse, die von
  `NGine.Script` erbt, mit ihren öffentlichen Feldern; die Registry legt daraus einen
  zusammengesetzten Typ an (`add_composite`: Felder in natürlicher Anordnung, Standardwerte aus
  einer frischen C#-Instanz). Damit behandeln Inspector, Undo, Szene, Prefab-Überschreibungen und
  der Snapshot des Spielmodus Skripte wie jede Komponente. Die Komponente ist die Quelle der
  Wahrheit für die öffentlichen Felder; das C#-Objekt bekommt sie beim Anlegen und auf Wunsch
  (`load_fields`), zurück schreibt die Engine nur bei Bedarf (`store_fields`: Speichern, Inspector
  beim Spielen, Neuladen). Die Engine hält keine Verweise auf C#-Objekte; C# führt pro Klasse eine
  Tabelle Entity → Objekt.
- **Ablauf:** `FixedUpdate` läuft als feste Stufe vor der Physik, `Update` als Stufe nach den
  Systemen von `Phase::Frame`, beide im Hauptthread, nach Klassenname und dann in der Reihenfolge
  der Chunks. Neue Komponenten bekommen vorher `Start`, verschwundene `Stop`. Kontakte, Auslöser,
  Landungen und Schritte der Physik sammeln sich und gehen vor dem nächsten `FixedUpdate` an die
  Skripte der beteiligten Entities. Aktionen liefern in `FixedUpdate` die Flanken seit dem letzten
  festen Schritt.
- **Neuladen:** Ändert sich das Layout einer Klasse, mustert die Registry den alten Typ aus (er
  bleibt gültig, ist aber nicht mehr zu finden), legt einen neuen an und zieht die Komponenten
  über JSON um: gleiche Namen behalten ihre Werte, neue Felder bekommen den Standardwert. Fehlt
  eine Klasse, bleiben ihre Werte als `UnknownComponents` (JSON) an der Entity und werden
  unverändert gespeichert; taucht der Typ wieder auf, übernimmt die Engine sie. Dasselbe gilt für
  Szenen, die vor den Skripten geladen werden.
- **Bauen und Hot Reload:** Skripte liegen als `.cs` unter `assets/skripte/` mit
  `Skripte.csproj`. `ScriptSystem` baut das Projekt mit `dotnet build` in einem eigenen Thread in
  den Cache (`assets/.cache/skripte`), beobachtet den Ordner mit dem `FileWatcher` und lädt nach
  einem gelungenen Bau neu; die Felder überstehen das, privater Zustand nicht (`Start` läuft
  erneut). Scheitert der Bau, laufen die alten Skripte weiter, die Meldungen des Compilers stehen
  mit Datei und Zeile im Log und im Fenster „Skripte“. Ohne SDK lädt die Engine die zuletzt
  gebaute Assembly aus dem Cache. Externe Programme startet `ngine.platform` (`run_process`, ein
  Adapter für POSIX und einer für Windows).

## Konsequenzen

- Eine neue Komponente in C++ ist nach dem Bauen in C# da, ohne Handarbeit, und ein Fehler in der
  Anordnung fällt beim Start auf statt als kaputter Speicher.
- Skripte erscheinen im Inspector mit Bereichen und Farbwählern (`[Range]`, `[Color]`, `[Asset]`),
  werden mit Szenen und Prefabs gespeichert und kommen beim Stoppen des Spielmodus bitgenau zurück.
- Ein Neubau dauert auf dem Entwicklungsrechner rund zwei Sekunden; das Spiel läuft währenddessen
  weiter. Weil C# Assemblies aus einem Stream lädt, sperrt Windows die Datei nicht, und der Bau
  kann in denselben Ordner schreiben.
- Referenzen aus `Get` gelten nur innerhalb eines Aufrufs; wer sie länger hält, liest nach einer
  Strukturänderung falschen Speicher. Das steht in der API; eine Prüfung im Debug-Build fehlt noch.
- Statische Felder in Skripten, die auf Objekte außerhalb des Skript-Kontexts zeigen, verhindern das
  Entladen; die Engine meldet dann, dass der alte Kontext nicht frei wurde.
- Pro Skript und Bild kostet ein `Update` mit einem `Get` einige hundert Nanosekunden (Debug-Build
  ohne Fenster); 10 000 Skripte misst `--scripts 10000`.
- Die Steuerung der Figur in C++ (`apply_controls()`) bleibt als Rückfall für Builds ohne .NET.

## Erwogene Alternativen

- **Eine feste Komponente `Script` mit Klassenname und JSON:** einfacher in der Registry, aber
  Inspector, Undo, Prefab-Überschreibungen und Replikation hätten Sonderwege gebraucht, und eine
  Entity könnte nur ein Skript tragen. In der Abstimmung verworfen.
- **Werte nach jedem Schritt in die Komponente schreiben:** immer aktuell, kostet aber bei vielen
  Skripten jedes Bild; beim Speichern und im Inspector reicht es auf Wunsch.
- **Bindings im Repository:** sichtbar im Review, veralten aber bei jeder Änderung einer
  Komponente. Die Prüfung beim Start fängt eine veraltete Datei ohnehin ab.
- **Roslyn in der Engine statt `dotnet build`:** schneller beim Neuladen, aber ein zweiter Weg zu
  bauen und mehr Abhängigkeiten in der Engine. Bleibt der Rückfall, falls der Bau zu langsam wird.
- **`System.Numerics` statt eigener Mathe-Typen:** `Vector3` hat keine doppelte Genauigkeit und
  passt nicht zur Anordnung von `DVec3`; Umwandlungen in beide Richtungen gibt es trotzdem.
