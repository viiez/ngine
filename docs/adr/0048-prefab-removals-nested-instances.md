# 0048: Prefabs mit entfernten Knoten und Komponenten und verschachtelten Instanzen

- **Status:** angenommen
- **Datum:** 2026-11-15

## Kontext

[ADR 0037](0037-prefabs-field-overrides.md) speichert von einer Instanz nur die Felder, die von der
Vorlage abweichen. Eine Instanz konnte deshalb keinen Knoten und keine Komponente der Vorlage
dauerhaft entfernen (nach dem Laden waren sie wieder da), und `make_prefab()` löste Instanzen im
Teilbaum auf, sodass eine Straße aus Laternen nicht mitbekam, wenn sich die Laterne änderte
(TD-32). Seit R4.4 hängen Skripte als Komponenten an Prefabs; eine Laterne ohne ihr Skript oder
Möbel aus mehreren Prefabs brauchen beides.

## Entscheidung

- **Überschreibungen** bekommen weitere Einträge pro Knoten: `"removed": true` (Knoten mit allem
  darunter fehlt; nur der oberste entfernte Knoten steht in der Datei), `"removed_components":
  [Namen]` (auch unbekannte Komponenten) und `"overrides": [...]` für eine verschachtelte Instanz.
  Wie bisher berechnet `prefab_overrides()` sie beim Speichern aus dem Vergleich mit der Vorlage;
  der Editor muss beim Löschen nichts davon wissen.
- **Verschachtelte Instanzen** sind Knoten der Vorlage mit `"prefab"` (Kennung) und
  `"overrides"` statt Komponenten; sie haben in der Datei keine Kinder. In der Welt sind sie voll
  ausgebaute Instanzen, deren Wurzel in `PrefabInstance::nested` ihren Knoten im äußeren Prefab
  trägt. `instance_nodes()` bleibt bei den eigenen Knoten, `nested_instances()` liefert die
  verschachtelten.
- **Eigene Überschreibungen einer verschachtelten Instanz** ersetzen die der Vorlage als Ganzes,
  statt sie Feld für Feld zu mischen. Ändert sich die innere Vorlage, ziehen auch verschachtelte
  Instanzen nach; ändert sich ihr Eintrag in der äußeren Vorlage, entstehen sie neu.
- **Format:** Prefab-Dateien bekommen Version 2 und lesen Version 1 weiter. Szenen behalten ihre
  Version, weil die neuen Einträge nur dazukommen.
- **IDs bleiben stabil:** Ist der Teilbaum schon eine Instanz („Änderungen in Vorlage
  übernehmen“), behalten ihre Knoten die IDs, neue zählen danach (`prefab_layout()`). Vorher
  wurden die Knoten neu durchgezählt, was Überschreibungen anderer Instanzen verschoben hätte,
  sobald ein Knoten fehlt.
- Vorlagen, die sich selbst enthalten, enden nach acht Ebenen mit einer Fehlermeldung; ohne
  bekannte Vorlage bleibt von einer verschachtelten Instanz die Wurzel mit ihren Überschreibungen
  als Text, wie bei Instanzen in Szenen.

## Konsequenzen

- Entfernen und Verschachteln überstehen Speichern, Laden, Nachziehen und den Spielmodus, ohne
  dass `ngine.edit` Befehle dafür kennt.
- Wer `prefab_overrides()`, `instantiate()`, `update_instances()` oder `make_prefab()` für
  verschachtelte Instanzen nutzt, gibt eine `PrefabLookup` mit; ohne sie verhält sich alles wie in
  R2.5 (Instanzen im Teilbaum werden aufgelöst).
- Entities, die jemand unter die Knoten einer verschachtelten Instanz hängt, kommen nicht in eine
  Vorlage (Warnung) und hängen in Szenen nach dem Laden an der Wurzel der verschachtelten
  Instanz, weil `"parent_node"` nur Knoten der äußeren Vorlage kennt (TD-55).

## Erwogene Alternativen

- **Entfernen als Befehl im Editor mitschreiben:** genauer, aber jeder Befehl müsste Prefabs
  kennen; das hat ADR 0037 schon verworfen.
- **Verschachtelte Instanzen beim Speichern der Vorlage auflösen** (bisher): einfach, aber
  Änderungen an der inneren Vorlage erreichen die äußeren nie.
- **Überschreibungen verschachtelter Instanzen Feld für Feld mischen:** genauer, wenn innere und
  äußere Vorlage dieselbe Komponente ändern, aber schwer vorherzusagen; das Ersetzen reicht für
  Möbel und Straßen.
- **Pfade über mehrere Ebenen in `"parent_node"`:** würde fremde Entities tief in verschachtelten
  Instanzen halten, macht das Format aber für einen seltenen Fall komplizierter.
