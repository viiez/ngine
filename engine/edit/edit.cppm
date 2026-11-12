// Editor-Logik ohne UI (R2.2): Szenen-Dokument mit stabilen Entity-IDs, Befehle mit Undo und Redo
// (EDIT-07, ADR 0030), Auswahl, Inspector-Modell aus der Reflection (EDIT-04) und Szenenbaum
// (EDIT-03). Alles ohne Fenster und GPU testbar; die Fenster des Editors sind dünn darüber.
//   :document  SceneDocument, Befehlsstapel, Ereignisse des Dokuments, Spielen (R2.3)
//   :commands  Befehle: Feld setzen, umbenennen, umhängen, anlegen, löschen, Komponenten
//   :selection Auswahl einer Entity
//   :inspector Zeilen und Eingaben des Inspectors, allein aus der Reflection
//   :tree      Zeilen des Szenenbaums
//   :gizmo     Gizmos zum Verschieben, Drehen und Skalieren, Kamera des Viewports (R2.3)
//   :prefabs   Prefabs anlegen, aufbrechen und zum Einfügen vorbereiten (R2.5)
//   :terrain   Voxel-Werkzeuge: Pinsel als Befehle mit Deltas für Undo (R3.4)
export module ngine.edit;

export import :document;
export import :commands;
export import :selection;
export import :inspector;
export import :tree;
export import :gizmo;
export import :prefabs;
export import :terrain;
