// Editor-Logik ohne UI (R2.2): Szenen-Dokument mit stabilen Entity-IDs, Befehle mit Undo und Redo
// (EDIT-07, ADR 0030), Auswahl, Inspector-Modell aus der Reflection (EDIT-04) und Szenenbaum
// (EDIT-03). Alles ohne Fenster und GPU testbar; die Fenster des Editors sind dünn darüber.
//   :document  SceneDocument, Befehlsstapel, Ereignisse des Dokuments
//   :commands  Befehle: Feld setzen, umbenennen, umhängen, anlegen, löschen, Komponenten
//   :selection Auswahl einer Entity
//   :inspector Zeilen und Eingaben des Inspectors, allein aus der Reflection
//   :tree      Zeilen des Szenenbaums
export module ngine.edit;

export import :document;
export import :commands;
export import :selection;
export import :inspector;
export import :tree;
