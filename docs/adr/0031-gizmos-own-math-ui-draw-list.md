# 0031: Gizmos selbst gebaut, Treffertests über Strahlen, gezeichnet über die Zeichenliste der UI

- **Status:** angenommen
- **Datum:** 2026-11-07

## Kontext

Im Editor sollen sich Objekte mit Gizmos verschieben, drehen und skalieren lassen, lokal und
global, mit Raster, und jede Bewegung soll ein Undo-Schritt sein (EDIT-05,
[R2.3](../iterations/R2.3-gizmos-und-spielen.md)). Im Plan von R2 ist entschieden, die Gizmos
selbst zu bauen statt ImGuizmo zu nehmen. Offen war, wo die Logik liegt, wie Treffer erkannt
werden und womit gezeichnet wird. Der Plan nannte das Debug-Zeichnen; dessen Linien haben aber
Tiefentest und eine feste Breite, Gizmos müssen über allem liegen und gut zu greifen sein.

## Entscheidung

- **Logik in `ngine.edit` (`:gizmo`)**, ohne UI und GPU: `edit::Gizmo` mit Modus, Raum und Raster,
  `edit::ViewCamera` für Strahl und Projektion. Eingabe ist die Mausposition in Pixeln des
  Viewports, Ausgabe ein `Transform`, das über `set_field()` als zusammenfassender Befehl ins
  Dokument geht. Ein Ziehen ist ein Schritt im Verlauf.
- **Treffertests in der Welt über Strahlen** (`ngine.math:ray`, 64 Bit): Abstand des Strahls zu
  den Achsen (Verschieben, Skalieren), Schnitt mit den Ebenen der Ringe (Drehen), Kugel in der
  Mitte (gleichmäßig skalieren). Die Größe des Gizmos folgt der Entfernung zur Kamera, damit er im
  Bild immer gleich groß ist; Schwellen sind relativ zu seiner Größe und damit etwa konstant in
  Pixeln.
- **Ziehen rechnet immer vom Start aus:** Ausgangslage, Weltlage des Elternteils und der Startwert
  auf der Achse werden beim Drücken festgehalten. Verschieben und Drehen geschehen in der Welt und
  werden über die Weltlage des Elternteils in die lokale Lage umgerechnet; Skalieren arbeitet in
  den Achsen der Entity. Drehwinkel werden in kleinen Schritten aufsummiert, damit mehr als eine
  halbe Drehung geht.
- **Gezeichnet über die Zeichenliste der UI** (`editor_ui::draw_line()` usw.): Der Editor
  projiziert die Form des Gizmos (Linien und Griffe in Weltkoordinaten) ins Bild und zeichnet sie
  über das Viewport-Bild, ohne Tiefentest und mit Breite in Pixeln. Dieselbe Zeichenliste bringt
  Text in der Welt ins Bild (REND-15).

## Konsequenzen

- Treffertests und Ziehen sind mit einer gedachten Kamera ohne Fenster und GPU testbar.
- Gizmos liegen immer über der Szene; verdeckte Teile sind nicht abgeblendet.
- Die Sandbox hat keine Gizmos (sie hat keine UI); das ist gewollt, Gizmos sind ein Werkzeug des
  Editors.
- Unter ungleichmäßig skalierten, gedrehten Eltern geht Scherung verloren (wie beim Umhängen,
  TD-28): Die lokale Drehung entsteht aus der zerlegten Weltlage des Elternteils.
- Ringe zeigen den ganzen Kreis, auch die hintere Hälfte; das kann bei schrägem Blick verwirren
  und lässt sich später durch Abblenden der Rückseite verbessern.

## Erwogene Alternativen

- **ImGuizmo:** schnell fertig, bringt aber eigene Mathematik (Matrizen in 32 Bit, eigene
  Konventionen) und koppelt die Logik an ImGui; nicht ohne UI testbar.
- **Zeichnen über das Debug-Zeichnen des Renderers:** kein zweiter Zeichenweg, aber Tiefentest und
  feste Breite; eine Variante ohne Tiefentest hätte eine weitere Pipeline gebraucht. Die
  Zeichenliste der UI gibt es schon, sie ist über der Szene und kann gefüllte Formen.
- **Treffertests im Bild (2D, projizierte Achsen):** ebenso möglich, aber das Ziehen braucht ohnehin
  Strahlen in der Welt; ein Weg für beides ist einfacher.
