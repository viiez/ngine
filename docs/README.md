# NGine-Dokumentation

Die Dokumentation liegt als Markdown im Repository und ändert sich im selben Pull Request wie der Code
(Docs as Code, siehe [ADR 0001](adr/0001-docs-as-code.md)). Was hier steht, gilt; Planungsnotizen
außerhalb des Repositorys sind nur Vorarbeit.

| Dokument | Inhalt | Wann aktualisieren |
| --- | --- | --- |
| [Vision](vision.md) | Ziel, Leitprinzipien, Rahmenbedingungen, Spielidee | Bei jedem Phasen- oder Iterations-Review, wenn sich Ziele verschieben |
| [Entwicklungsumgebung](setup.md) | Einrichtung von Linux, Windows und VS Code Schritt für Schritt | Wenn sich Werkzeuge, Abhängigkeiten oder Build-Schritte ändern |
| [Prozess](process.md) | Vereinfachter Unified Process: Phasen, Iterationen, Definition of Done | Wenn wir die Arbeitsweise ändern |
| [Roadmap](roadmap.md) | Phasen, Meilensteine, Iterationen, Reifegrade | Bei jeder Iterationsplanung und jedem Review |
| [Feature-Katalog](features.md) | Alle Features nach Engine-Komponente, mit Status | Wenn ein Feature dazukommt, sich ändert oder fertig wird |
| [Architektur](architecture.md) | Schichten, Module, Abhängigkeitsregeln | Wenn ein Modul dazukommt oder sich Schnittstellen ändern |
| [Fremdbibliotheken](third-party.md) | Eingesetzte und geplante Bibliotheken mit Begründung | Wenn eine Bibliothek dazukommt oder ersetzt wird |
| [Entscheidungen (ADRs)](adr/README.md) | Architekturentscheidungen mit Kontext und Alternativen | Bei jeder Entscheidung, die schwer rückgängig zu machen ist |
| [Iterationen](iterations/README.md) | Plan und Review jeder Iteration | Zu Beginn und am Ende jeder Iteration |

Diagramme schreiben wir als [Mermaid](https://mermaid.js.org/) direkt in Markdown; GitHub stellt sie dar.
