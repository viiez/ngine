# 0037: Prefabs als Dateien, Instanzen voll ausgebaut mit Überschreibungen auf Feldebene

- **Status:** angenommen
- **Datum:** 2026-11-08

## Kontext

R2.5 soll Prefabs bringen: eine Vorlage für einen Teilbaum aus Entities als Datei, Instanzen davon
in Szenen mit Überschreibungen, im Editor anlegen und aufbrechen (WORLD-04). Abnahme: Prefab
ändern, und alle Instanzen ziehen nach; Überschreibungen bleiben. Die ECS-Welt kennt bisher nur
Entities mit Komponenten und Eltern ([ADR 0003](0003-ecs-flecs.md)); Szenen sind JSON über die
Reflection ([ADR 0020](0020-scene-format-json.md)).

## Entscheidung

- **Vorlage:** eine `.prefab`-Datei im Asset-Ordner (mit `.meta`-Datei und Kennung) im Format einer
  Szene mit genau einer Wurzel (`"format": "ngine.prefab"`). Die IDs der Datei sind die Knoten
  der Vorlage und bleiben stabil, solange niemand sie umnummeriert.
- **Instanz in der Welt:** voll ausgebaut, jede Entity ist eine normale Entity. Jede trägt
  `PrefabNode{node}`, die Wurzel zusätzlich `PrefabInstance{prefab}`. Systeme, Renderer und
  Inspector brauchen nichts über Prefabs zu wissen. Beide Komponenten sind transient und werden
  nicht als Komponenten gespeichert.
- **Instanz in der Szene:** ein Eintrag mit `"prefab"` (Kennung) und `"overrides"`: pro Knoten nur
  die Felder, die von der Vorlage abweichen, als JSON wie in Szenen. Die Überschreibungen werden
  beim Speichern berechnet (Vergleich mit der Vorlage), nicht beim Bearbeiten mitgeschrieben.
  Entities, die jemand unter einen Knoten gehängt hat, stehen als normale Einträge mit
  `"parent_node"` in der Szene.
- **Nachziehen:** Ändert sich eine Vorlage (Hot Reload), rechnet `update_instances()` für jede
  Instanz die Abweichungen gegen den alten Stand, setzt die Werte des neuen Stands und dann die
  Abweichungen wieder. Neue Knoten kommen dazu, weggefallene verschwinden samt allem darunter;
  Entities, die es weiter gibt, behalten ihre Kennung. Komponenten, die aus der Vorlage fallen,
  verschwinden, außer die Instanz überschreibt sie.
- **Fehlende Vorlage:** Die Instanz bleibt als Wurzel mit ihren Überschreibungen als Text
  (`PrefabOverrides`) und wird unverändert zurückgeschrieben; nichts geht verloren.
- **Editor:** „Als Prefab speichern“ schreibt den Teilbaum als Vorlage (Wurzel im Ursprung) und
  macht ihn mit einem Undo-Schritt zur ersten Instanz. „Änderungen in Vorlage übernehmen“ schreibt
  eine Instanz als neuen Stand der Datei; Hot Reload zieht die anderen nach. „Prefab aufbrechen“
  löst die Verknüpfung (Undo stellt sie wieder her).

## Konsequenzen

- Überschreibungen sind robust gegen neue Felder und neue Komponenten in der Vorlage, weil nur
  Abweichungen gespeichert werden.
- Eine Instanz kann einen Knoten oder eine Komponente der Vorlage nicht dauerhaft entfernen; wer
  das braucht, bricht die Instanz auf. Das steht als Schuld im Register.
- Verschachtelte Prefabs gibt es nicht: `make_prefab()` löst Instanzen im Teilbaum auf.
- Nachziehen geschieht in der Welt außerhalb des Verlaufs; Undo-Schritte, die sich auf
  weggefallene Knoten beziehen, melden, dass es die Entity nicht mehr gibt.
- Der Vergleich über JSON ist langsamer als über Bytes, aber nur beim Speichern und Nachziehen
  nötig.

## Erwogene Alternativen

- **Instanz nur als Verweis, aufgebaut erst zur Laufzeit:** spart Speicher in der Szene, aber jedes
  System müsste Instanzen auflösen, und der Inspector könnte nichts bearbeiten.
- **Überschreibungen beim Bearbeiten mitschreiben** (wie Unity): genauer bei Werten, die zufällig
  wieder gleich der Vorlage sind, aber jeder Befehl müsste Prefabs kennen. Der Vergleich beim
  Speichern hält `ngine.edit` frei davon.
- **Überschreibungen auf Komponentenebene:** einfacher, aber eine geänderte Farbe würde jede
  spätere Änderung der Helligkeit in der Vorlage verdecken.
- **Pfade statt Knoten-IDs:** lesbarer, brechen aber beim Umbenennen eines Knotens.
