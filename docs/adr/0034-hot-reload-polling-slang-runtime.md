# 0034: Hot Reload über Polling und Slang zur Laufzeit

- **Status:** vorgeschlagen
- **Datum:** 2026-11-07

## Kontext

R2.4 soll Shader, Texturen und Modelle zur Laufzeit neu laden (PLAT-06): Datei speichern, Bild
ändert sich ohne Neustart. Dafür braucht es zweierlei: geänderte Dateien erkennen und Shader aus
Slang-Quellen zur Laufzeit übersetzen. Bisher übersetzt `slangc` die Shader beim Bauen, und sie
stecken als SPIR-V im Programm (`ngine.shaders`).

## Entscheidung

- **Polling statt Benachrichtigungen:** `filesystem::FileWatcher` vergleicht alle 0,25 s Größe und
  Änderungszeit aller Dateien unter einem Ordner. Eine neue oder geänderte Datei meldet er erst,
  wenn sie einen Durchlauf lang gleich geblieben ist (Editoren schreiben oft in mehreren
  Schritten); gelöschte sofort. Versteckte Einträge und Dateien auf `.tmp` oder `~` zählen nicht.
- **Slang als Bibliothek:** Neues Modul `ngine.shader_compiler` übersetzt über die COM-API von Slang
  mit denselben Optionen wie beim Bauen (SPIR-V 1.5, Namen der Einstiegspunkte). Jeder Aufruf
  nutzt eine neue Sitzung, damit geänderte importierte Module neu gelesen werden. Nur Editor und
  Tests linken es; Sandbox und Server bleiben bei den eingebetteten Shadern.
- **Renderer:** `Renderer::reload_shaders(source)` übersetzt alle Shader, baut dann alle Pipelines
  neu und tauscht erst, wenn alles geklappt hat. Bei einem Fehler bleibt der alte Stand und der
  Editor zeigt die Meldung von Slang mit Datei und Zeile im Log. Der Renderer kennt Slang nicht;
  er bekommt eine Funktion, die SPIR-V liefert.
- **Assets:** Der Editor gibt geänderte Dateien unter dem Asset-Ordner an
  `ResourceManager::reload_files()` weiter (samt Abhängigkeiten wie `.bin` und Bildern einer
  glTF-Datei) und durchsucht die Datenbank neu, wenn Dateien dazukommen oder verschwinden.

## Konsequenzen

- Kein Plattformcode, gleiches Verhalten unter Linux und Windows. Der Preis sind einige
  Dateiabfragen pro Sekunde; bei einigen hundert Assets ist das vernachlässigbar, bei vielen
  tausend wird es teuer (dann Benachrichtigungen des Betriebssystems nachrüsten).
- Eine Änderung wird nach 0,25 bis 0,5 s sichtbar.
- Der Editor lädt die Slang-Bibliothek (einige zehn MB) beim ersten Shader-Reload. Unter Windows
  müssen ihre DLLs neben dem Programm liegen (`ngine_copy_slang_runtime()`).
- Neu geladene Modelle sind für ein, zwei Frames nicht zu sehen, bis ihre Uploads fertig sind
  ([ADR 0035](0035-transfer-queue-concurrent-sharing.md)).

## Erwogene Alternativen

- **inotify und ReadDirectoryChangesW:** sofort und ohne Abfragen, aber zwei Plattform-Adapter,
  Sonderfälle bei Editoren, die Dateien ersetzen statt schreiben, und bei Netzlaufwerken.
- **efsw oder eine ähnliche Bibliothek:** nimmt uns die Adapter ab, ist aber eine weitere
  Abhängigkeit für ein Problem, das Polling für unsere Größe löst.
- **`slangc` als Prozess starten:** keine Bibliothek im Editor, aber ein Prozess pro Shader,
  Pfad zum Werkzeug zur Laufzeit und Fehler nur als Text auf stderr.
