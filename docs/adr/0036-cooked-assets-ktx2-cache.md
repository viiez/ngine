# 0036: Gekochte Modelle mit KTX2 und Basis Universal in einem Cache neben den Quellen

- **Status:** vorgeschlagen
- **Datum:** 2026-11-08

## Kontext

Bis R2.4 lädt die Engine Modelle aus den Quellen: fastgltf liest glTF, stb_image dekodiert PNG und
JPEG, die CPU rechnet Mip-Stufen und Tangenten. Das kostet bei jedem Start Zeit, und die Texturen
liegen unkomprimiert im GPU-Speicher. R2.5 soll eine Asset-Pipeline bringen, die Quellen in
Laufzeitformate „kocht“, nur bei Änderung neu kocht und Tangenten nach MikkTSpace erzeugt
(ASSET-02, ASSET-03, TD-21). Abnahme: Die Standardszene lädt aus dem Cache spürbar schneller als
aus den Quellen.

## Entscheidung

- **Format gekochter Modelle:** eine Binärdatei pro Modell (`.nmodel`, Kennung `NGMD`, eigene
  Version) mit dem Aufbau aus `asset::Model` (Meshes, Materialien, Sampler, Knoten), Eckpunkten und
  Indizes genau so, wie der Renderer sie hochlädt (48 Bytes pro Eckpunkt, Little Endian), und den
  Texturen als eingebettete KTX2-Dateien, je Bild und Farbraum eine. Beim Lesen werden alle
  Querverweise geprüft; kaputte Dateien sind ein Fehler, kein Absturz.
- **Texturen:** KTX2 mit Basis Universal über KTX-Software, standardmäßig UASTC mit zstd
  (fast verlustfrei, wird zu BC7). Weil alle Desktop-GPUs BC7 können, wandelt schon das Kochen
  jede Textur zusätzlich nach BC7 um und legt die fertigen Blöcke mit ins gekochte Modell; GPUs mit
  `Capabilities::bc_compression` laden sie ohne Rechnen. Ohne BC7 (z. B. lavapipe) wird beim
  Laden aus der KTX2-Datei nach RGBA8 umgewandelt. ETC1S (kleiner, sichtbar verlustbehaftet) und
  unkomprimiertes RGBA8 sind Einstellungen von `ngine_cook`.
- **Cache:** gekochte Dateien liegen in `<assets>/.cache/<Kennung hex>.nmodel`. Der Ordner ist
  versteckt (Datenbank und Dateibeobachter übergehen ihn) und steht in `.gitignore`. Jede Datei
  enthält Stempel aller Quelldateien (Pfad, Größe, Änderungszeit, FNV-1a über den Inhalt). Stimmen
  Größe und Zeit, gilt eine Quelle ohne Lesen als unverändert; sonst entscheidet die Prüfsumme,
  damit ein `git checkout` mit neuen Zeiten nicht alles neu kocht.
- **Wer kocht:** Kochen (Basis-Kodierung, MikkTSpace) steckt in `ngine.cook`, das nur Editor,
  Werkzeug (`ngine_cook`) und Tests linken. Der Editor kocht beim Laden, was fehlt oder veraltet
  ist, und legt es in den Cache. Spiel, Sandbox und Server lesen gekochte Dateien (`ngine.asset`
  enthält nur das Transkodieren) und laden aus den Quellen, wenn nichts Aktuelles im Cache liegt.
- **Tangenten:** MikkTSpace ersetzt beim Kochen die einfach gemittelten Tangenten für Teilnetze
  ohne Tangenten in der Datei; Eckpunkte an Nähten werden geteilt.
- **Bereit erst nach dem Upload (TD-30):** Lader können eine Bereitschaft melden; der
  Ressourcen-Manager tauscht einen neu geladenen Wert erst ein, wenn die Uploads fertig sind.

## Konsequenzen

- Laden aus dem Cache spart Dekodieren, Mip-Stufen, Tangenten und Umwandeln; BC7 braucht ein
  Viertel des Speichers von RGBA8. Zahlen stehen im Stand von
  [R2.5](../iterations/R2.5-kochen-ktx2-prefabs.md). Ein erster Versuch, der beim Laden aus UASTC
  nach BC7 umwandelte, war kaum schneller als die Quellen: Das Umwandeln kostete so viel wie
  Dekodieren und Mip-Stufen.
- Gekochte Modelle sind größer, weil sie jede Textur zweimal enthalten (UASTC für alle GPUs, BC7
  fertig): rund 2,3 MB pro Textur mit 1024² Pixeln samt Mip-Stufen.
- Erstes Öffnen im Editor dauert länger, weil gekocht wird (UASTC ist langsam zu kodieren). Mit
  „Alles kochen“ bzw. `ngine_cook` lässt sich das vorziehen.
- Der Cache hängt an der Formatversion: Ändert sich das Format oder das Kochen, wird
  `cooked_model_version` erhöht, und alte Dateien gelten als veraltet.
- KTX-Software ist eine große Abhängigkeit (Basis Universal, zstd). Sie steckt nur in
  `ktx_libktx.cpp` (Lesen) und `ktx_encode.cpp` (Kodieren).
- Gekochte Dateien hängen an Endianness und `sizeof(Vertex)`; beides prüft der Code beim Bauen.
- Ein gekochtes Modell für ein Spiel ohne Quellen (Paket ohne `assets/`) folgt später; heute
  braucht der Cache die Quellen für die Stempel.

## Erwogene Alternativen

- **DDS mit BC7 direkt:** schnell zu laden, aber nur für GPUs mit BC; Mobilgeräte und manche
  Treiber bräuchten eine zweite Kopie. Basis Universal deckt beides mit einer Datei ab.
- **Gekochte Dateien neben den Quellen** (z. B. `kiste.gltf.nmodel`): sichtbar im Asset-Browser
  und in Git, Konflikte beim Umbenennen. Ein Ordner nach Kennung ist robuster.
- **Hash des Inhalts als Name im Cache:** jede Änderung legt eine neue Datei an, der Cache wächst
  ohne Aufräumen. Die Kennung als Name mit Stempeln darin überschreibt einfach.
- **meshoptimizer zum Komprimieren der Eckpunkte:** lohnt sich erst bei großen Welten; das Format
  hat eine Version und kann es später aufnehmen.
