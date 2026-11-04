# 0028: Lichter als Komponenten, Cascaded Shadow Maps mit eingerasteten Kugeln, Lichtliste vorwärts

- **Status:** vorgeschlagen
- **Datum:** 2026-11-04

## Kontext

R1.4 bringt Licht und Schatten (REND-09): eine Sonne mit Schatten über große Entfernungen und
einige bewegte Punkt- und Spotlichter. Die Welt hat 64-Bit-Positionen und wird relativ zur Kamera
gezeichnet ([ADR 0012](0012-64bit-world-coordinates.md)); der Renderer ist vorwärts
([ADR 0027](0027-gltf-assets-material-store.md)) und kennt nur die RHI. Später kommen glatte
Voxel-Landschaften mit weiter Sicht (R3), viele Lichter in Höhlen und Gebäuden und ein
Horror-Spiel, in dem flimmernde oder ausgefranste Schatten sofort auffallen.

## Entscheidung

- **Lichter sind Komponenten:** `DirectionalLight`, `PointLight`, `SpotLight`, angelehnt an glTF
  `KHR_lights_punctual` (Farbe, Stärke, Reichweite mit weichem Ende, innerer und äußerer
  Kegelwinkel). Richtung ist die lokale -z-Achse aus `WorldTransform`. Das erste aktive gerichtete
  Licht ist die Sonne und wirft Schatten.
- **Lichtliste vorwärts:** Alle anderen Lichter stehen pro Frame in einem Puffer (bis 256,
  kamerarelativ); der Vorwärts-Pass geht für jedes Pixel alle durch. Kein Clustering, kein
  Deferred Shading.
- **Cascaded Shadow Maps:** vier Kaskaden, aufgeteilt zwischen logarithmisch und gleichmäßig,
  jede eine eigene Tiefentextur im Render-Graph. Jede Kaskade umschließt ihren Ausschnitt des
  Kamerastumpfs mit einer Kugel (Radius unabhängig von der Blickrichtung); ihr Mittelpunkt rastet
  in 64-Bit-Weltkoordinaten auf ganze Texel des Lichtraums ein. Gefiltert wird mit PCF über
  `Gather` (3 × 3 bilinear verglichene Proben) ohne Vergleichs-Sampler. Gegen Akne wirken
  Tiefenversatz in der Pipeline und Versatz entlang der Normale um Texel der Kaskade; am Ende jeder
  Kaskade geht sie weich in die nächste über.
- **RHI:** Pipelines und Rendering nur mit Tiefe, Tiefenversatz, lesbare Tiefentexturen. Mehr
  nicht: keine Textur-Arrays, kein Depth Clamp, keine Vergleichs-Sampler.

## Konsequenzen

- Schattenkanten flimmern nicht beim Drehen oder Bewegen der Kamera, auch 10 000 km vom
  Ursprung; der Preis ist Auflösung, weil die Kugel größer ist als ein eng angepasster Quader.
- Die Kaskaden sind reine Rechnung ohne GPU und deshalb mit Tests abgesichert (Abdeckung des
  Stumpfs, Einrasten auf Texel).
- Das Bild weit vom Ursprung ist nicht mehr byte-gleich: Die Schattenkarten rasten dort auf ein
  anderes Texelgitter ein. Die CI vergleicht deshalb mit Toleranz.
- Die Lichtliste kostet pro Pixel alle Lichter. Für Szenen mit wenigen Dutzend Lichtern reicht
  das; für viele Lichter braucht es Clustered Shading (TD-24).
- Punkt- und Spotlichter werfen keine Schatten (TD-25). Durchsichtige Materialien werfen keine.
- Vier Pässe zeichnen die Werfer viermal; das Culling pro Kaskade läuft auf der CPU über
  Hüllkugeln. GPU-Culling (REND-10) kann das später übernehmen.

## Erwogene Alternativen

- **Eng angepasste Kaskaden (Quader um die Ecken des Ausschnitts):** bessere Auflösung, aber die
  Größe ändert sich mit der Blickrichtung, die Kanten flimmern beim Drehen.
- **Eine Atlas-Textur oder ein Textur-Array für alle Kaskaden:** spart Bindungen, braucht aber
  Viewports pro Pass bzw. Arrays in der RHI; vier einzelne Texturen passen ohne Erweiterung in
  Graph und Bindless.
- **Vergleichs-Sampler (Hardware-PCF):** schneller, braucht aber eine zweite Art Sampler im
  bindlosen Satz. `Gather` mit Vergleich im Shader liefert dasselbe Ergebnis ohne RHI-Änderung.
- **Deferred oder Clustered Shading jetzt:** lohnt sich erst mit vielen Lichtern und kostet einen
  G-Buffer bzw. einen Compute-Pass zum Einsortieren; vorwärts bleibt einfacher und passt zu
  durchsichtigen Materialien.
- **Virtual Shadow Maps:** beste Qualität für große Welten, aber viel Aufwand (Seiten-Verwaltung,
  GPU-Culling); Kandidat für R3, wenn die Landschaft es braucht.
