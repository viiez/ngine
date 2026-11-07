# 0030: Undo über Befehle mit reflektierten Werten und stabilen Entity-Kennungen

- **Status:** angenommen
- **Datum:** 2026-11-06

## Kontext

Der Editor soll jede Änderung rückgängig machen können (EDIT-07, [R2.2](../iterations/R2.2-szenenbaum-inspector-undo.md)):
Felder im Inspector, Umbenennen, Umhängen, Anlegen und Löschen von Entities, Komponenten hinzu
und weg. Der Inspector baut seine Felder allein aus der Reflection, ohne Code pro Komponente
([ADR 0018](0018-own-reflection.md)); dasselbe soll für Undo gelten. Zwei Dinge machen es schwer:
Werte haben beliebige Typen (Strings, verschachtelte Structs), und flecs vergibt einer gelöschten
und wieder angelegten Entity eine neue Kennung, sodass ein Befehl, der sich die flecs-Kennung
merkt, nach Undo eines Löschens ins Leere zeigt.

## Entscheidung

- **Befehle** (`edit::Command`) mit `apply()`, `revert()` und `merge()`. Ein `CommandStack` hält sie
  mit Zeiger auf die aktuelle Stelle, einem gemerkten gespeicherten Stand (daraus „ungespeichert“)
  und einer Obergrenze. Alles, was der Editor ändert, läuft über `SceneDocument::execute()`.
- **Werte als Bytes im Binärformat der Reflection** (`capture()`/`restore()` über
  `ngine.serialize`): Ein Feldbefehl merkt sich Komponente, Feldpfad (Indizes der Felder von der
  Komponente bis zum Ziel, z. B. `Transform.position`) und alten und neuen Wert. Löschen merkt
  sich den ganzen Teilbaum mit allen speicherbaren Komponenten auf dieselbe Weise.
- **Stabile Kennungen** (`edit::EntityId`): `SceneDocument` ordnet jeder Entity eine eigene Kennung
  zu, die nie wieder vergeben wird. Befehle kennen nur diese; stellt Undo eine gelöschte Entity
  wieder her, bekommt die neue flecs-Entity dieselbe Kennung.
- **Zusammenfassen:** Aufeinanderfolgende Änderungen desselben Feldes derselben Entity verschmelzen
  zu einem Schritt, bis der Verlauf versiegelt wird (`CommandStack::seal()`, im Editor sobald kein
  Eingabefeld mehr aktiv ist). Ziehen eines Reglers über viele Frames ist so ein Schritt.
- **Gruppen** (`edit::group()`) führen mehrere Befehle als einen Schritt aus, alles oder nichts.
- Laufzeitkomponenten (`transient`) sind nicht Teil von Befehlen und Schnappschüssen; der Inspector
  zeigt sie nur an.

## Konsequenzen

- Neue Komponenten brauchen für Inspector und Undo nur ihre Registrierung in der Reflection.
- Befehle hängen nicht an flecs, nur an `ngine.world` und der Reflection, und sind ohne Fenster und
  GPU testbar.
- Dasselbe Binärformat nutzt die Replikation; Fehler im Serialisieren fallen im Editor früh auf.
- Wiederhergestellte Entities bekommen neue flecs-Kennungen. Kinder sortiert die Welt nach
  Kennung, deshalb kann sich die Reihenfolge unter Geschwistern nach Undo eines Löschens ändern.
- Wer die Welt am Dokument vorbei ändert (z. B. Systeme beim Spielen in R2.3), macht den Verlauf
  ungültig; dafür gibt es `SceneDocument::reset_history()`.
- Umhängen behält die lokale Lage (`Transform`); das Objekt springt also mit dem neuen Elternteil.
  Die Weltlage zu erhalten braucht eine Umrechnung und kommt bei Bedarf.

## Erwogene Alternativen

- **Ganze Szene vor jeder Änderung sichern:** einfach, aber Speicher und Zeit wachsen mit der Szene,
  und Zusammenfassen beim Ziehen wird teuer.
- **Befehle pro Komponente von Hand:** schneller zu schreiben für die ersten Typen, widerspricht aber
  dem Ziel „kein Code pro Komponente“.
- **Werte als JSON statt binär:** lesbarer beim Debuggen, aber langsamer und mit Rundung bei
  Gleitkommazahlen; das Binärformat gibt Werte bitgenau zurück.
- **flecs-Kennungen festhalten und gelöschte Entities nur verstecken:** spart die Zuordnung, aber
  versteckte Entities würden von Abfragen, Szenen und Systemen gesehen.
