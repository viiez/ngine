# 0020: Szenen als JSON mit eigenem Parser

- **Status:** vorgeschlagen
- **Datum:** 2026-11-01

## Kontext

A4 soll Szenen speichern und laden (WORLD-05). Szenen bearbeitet später der Editor, sie liegen im
Repository des Spiels und sollen sich in Diffs lesen lassen. Für Netzwerk und gekochte Assets
brauchen wir zusätzlich ein kompaktes binäres Format. Beides soll über die Reflection
([ADR 0018](0018-own-reflection.md)) laufen.

## Entscheidung

- Das Quellformat für Szenen ist JSON: ein Objekt mit `format`, `version` und `entities`. Jede
  Entity hat eine Datei-ID, optional `parent` und ein Objekt `components` mit einem Schlüssel pro
  Komponente (Name aus der Reflection). Eltern stehen vor Kindern; dieselbe Welt ergibt immer
  denselben Text.
- Beim Laden behalten fehlende Felder ihren Standardwert, unbekannte Felder und Komponenten werden
  übersprungen (mit Warnung). So überleben Szenen kleine Änderungen am Code.
- JSON lesen und schreiben wir selbst (`ngine.serialize`, gut 800 Zeilen mit Kommentaren): ein `json::Value`
  mit geordneten Objekten, Parser nach RFC 8259 mit Tiefenbegrenzung, Gleitkommazahlen über
  `std::to_chars`/`std::from_chars`, die bitgenau zurückkommen. `f32` wird in seiner kürzesten Form
  geschrieben (`0.1` statt `0.10000000149011612`).
- Daneben gibt es ein binäres Format ohne Feldnamen (`BinaryWriter`/`BinaryReader`) für Daten, die
  derselbe Build liest.

## Konsequenzen

- Keine weitere Abhängigkeit; der Parser ist klein, getestet und an unsere Bedürfnisse angepasst
  (Reihenfolge der Schlüssel, Fehler mit Zeile und Spalte, Pfad zum Feld bei falschen Werten).
- JSON-Szenen sind groß (eine Demo mit 2000 Entities hat knapp 600 KB) und langsamer zu laden als
  Binärdaten. Für große Welten und den Versand ans Spiel kochen wir sie später in ein binäres
  Format (Asset-Pipeline, R2). Das Voxel-Terrain speichert ohnehin eigene Daten (Deltas, VOX-06).
- Schemaänderungen über das Überspringen hinaus (Felder umbenennen, Werte umrechnen) brauchen
  Migrationen anhand von `version`.

## Erwogene Alternativen

- **nlohmann/json:** sehr bequem, aber groß, langsam beim Übersetzen und ohne Kontrolle über
  Zahlen und Fehlermeldungen.
- **glaze, simdjson, yyjson:** sehr schnell. glaze setzt auf eigene Compile-Zeit-Reflection, die
  unsere ersetzen würde; simdjson liest nur; yyjson ist C und bräuchte dennoch eine eigene
  C++-Schicht.
- **JSON-Addon von flecs:** bindet das Format an flecs und dessen Reflection.
- **Eigenes Textformat oder TOML/YAML:** YAML ist mehrdeutig und schwer zu parsen, TOML passt
  schlecht zu tief verschachtelten Daten; ein eigenes Format kennt kein Werkzeug.
