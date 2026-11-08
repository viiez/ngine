# 0033: Asset-IDs in .meta-Dateien, Ressourcen-Manager mit Handles und Laden im Hintergrund

- **Status:** angenommen
- **Datum:** 2026-11-07

## Kontext

Bis R2.3 verwiesen `MeshRenderer` über Indizes in die Speicher des Renderers. Szenen mit Modellen
ließen sich deshalb nicht vollständig speichern; der Editor brauchte beim Öffnen den Modellpfad
(Plan von [R2](../iterations/R2-welt-und-editor.md)). R2.4 soll Szenen mit Modellen speicherbar
machen, Modelle im Hintergrund laden und zur Laufzeit neu laden (ASSET-04, ASSET-05). Die Kennung
muss Umbenennen und Verschieben überstehen, in Git gut aussehen und später für gekochte Daten
(R2.5) taugen.

## Entscheidung

- **Kennung pro Quelldatei:** Jede Datei bekannter Art unter dem Asset-Ordner (`.gltf`, `.glb`,
  Bilder, `.slang`, `.json`) bekommt eine zufällige 64-Bit-Kennung in einer `.meta`-Datei daneben
  (`{"format": "ngine.meta", "version": 1, "id": …}`). Die `AssetDatabase` legt fehlende
  `.meta`-Dateien beim Durchsuchen an und vergibt bei doppelten Kennungen (kopierte
  `.meta`-Datei) der späteren Datei eine neue. `.meta`-Dateien gehören ins Repository.
- **Verweis auf Teile:** `asset::AssetRef{asset, index}` zeigt auf einen Teil eines Assets, z. B.
  das dritte Mesh einer glTF-Datei. `MeshRenderer{mesh, material}` besteht aus zwei solchen
  Verweisen und wird mit Szenen gespeichert.
- **Erzeugte Assets:** Modelle, die ein Programm selbst baut (Standardszene, `--model`), bekommen
  eine feste Kennung aus einem Hash ihres Namens (`generated_id()`), damit auch sie in Szenen
  stehen können.
- **Ressourcen-Manager:** `ResourceManager::load<T>(id)` liefert ein `Handle<T>` sofort; Lader
  bestehen aus zwei Teilen: `read()` läuft im Hintergrund auf dem Job-System (Datei lesen,
  dekodieren, Mip-Stufen rechnen), `finish()` im Hauptthread in `update()` (Hochladen). Handles
  zählen Referenzen; was niemand mehr hält, gibt `update()` nach `keep_updates` Aufrufen frei.
  Lader melden die Dateien, die sie gelesen haben (glTF: `.bin`, Bilder); `reload_files()` lädt
  alles neu, was davon abhängt. Scheitert ein Neuladen, bleibt der alte Stand.
- **Renderer:** Er hält die Modelle, auf die `MeshRenderer` verweisen, über Handles und lässt sie
  los, wenn sie zwei Frames lang nicht gezeichnet wurden. Was noch lädt, zählt als „wartet“ in der
  Statistik und wird nicht gezeichnet.

## Konsequenzen

- Szenen mit Modellen sind vollständig speicherbar; Verschieben und Umbenennen samt `.meta`-Datei
  ändert nichts an ihnen.
- Neben jeder Quelldatei liegt eine zweite Datei. Wer eine Datei ohne ihre `.meta`-Datei
  verschiebt, bekommt eine neue Kennung, und Szenen verlieren den Verweis. Das zeigt der Editor als
  „wartet“ bzw. im Log.
- `AssetRef::index` zählt in der Reihenfolge der Quelldatei. Wer Meshes darin umsortiert,
  verschiebt die Verweise; stabile Namen für Teile kommen bei Bedarf mit dem Kochen (R2.5).
- Generische Ressourcen (`Handle<T>`) sind typlos gespeichert; der Typ steckt in einem Schlüssel
  pro Typ, den nur Code mit derselben Instanziierung kennt.

## Erwogene Alternativen

- **Pfade als Kennung:** einfach, aber jedes Umbenennen bricht Szenen; Pfadvergleiche hängen vom
  Betriebssystem ab.
- **Hash des Inhalts:** ändert sich mit jeder Änderung, taugt also nicht für Verweise, nur für den
  Cache gekochter Daten.
- **Eine zentrale Datenbankdatei statt `.meta`-Dateien:** weniger Dateien, aber Konflikte in Git,
  sobald zwei Leute Assets anlegen, und Kennungen gehen beim Kopieren von Ordnern verloren.
- **UUIDs mit 128 Bit:** unnötig groß für ein Projekt; 64 Zufallsbits machen Kollisionen
  praktisch unmöglich und erkannte Doppelte werden ersetzt.
