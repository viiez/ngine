# 0018: Eigene Reflection mit Builder-API, flecs nur als Speicher

- **Status:** vorgeschlagen
- **Datum:** 2026-11-01

## Kontext

Speichern, Netzwerk, Editor (Inspector) und C#-Bindings müssen mit Komponenten umgehen, ohne ihre
Typen zu kennen: Sie brauchen Name, Größe, Felder mit Typ und Offset sowie Funktionen zum Erzeugen,
Kopieren und Zerstören. C++23 hat dafür keine Sprachmittel; statische Reflection kommt erst mit
C++26. flecs bringt ein eigenes Reflection-Addon (`meta`) mit Serialisierung nach JSON mit.

## Entscheidung

- Eigenes Modul `ngine.reflect` in der Core-Schicht, unabhängig von flecs.
- Typen werden beim Start über eine Builder-API registriert:
  `types.add<Velocity>("Velocity").field("x", &Velocity::x)`. Keine Makros, weil Module keine
  exportieren, und kein Codegenerator.
- Ein `TypeInfo` kennt Name, stabile `TypeId` (FNV-1a des Namens), Größe, Ausrichtung, Art
  (`Bool`, `Int`, `UInt`, `Float`, `String`, `Struct`), Felder und `TypeOps`. Grundtypen sind
  vorregistriert; Structs dürfen Structs enthalten.
- Reflektierbar ist, was sich standardmäßig erzeugen, kopieren und ohne Ausnahme verschieben und
  zerstören lässt (Konzept `Reflectable`).
- Die Registry wird beim Start gefüllt und danach nur gelesen; dann ist sie ohne Sperre aus allen
  Threads nutzbar.
- Die Welt (`ngine.world`) nimmt nur reflektierte Structs als Komponenten und meldet sie bei flecs
  mit Größe, Ausrichtung und Hooks aus den `TypeOps` an. Das `meta`-Addon von flecs nutzen wir
  nicht.

## Konsequenzen

- Serialisierung, Inspector und C# hängen an `ngine.reflect`, nicht an flecs. flecs bleibt
  austauschbar (ADR 0009); ein Wechsel zu EnTT oder einem eigenen ECS behielte die Reflection.
- Jede Komponente braucht eine Registrierungszeile pro Feld. Vergisst man ein Feld, wird es nicht
  gespeichert; ein vergessener Feldtyp bricht beim Start mit Meldung ab.
- Die Offsets stammen aus einem Beispielobjekt, das beim Registrieren entsteht. Das ist ohne
  undefiniertes Verhalten, setzt aber einen Standardkonstruktor voraus.
- Noch nicht unterstützt: Enums, Arrays, `std::vector`, Verweise auf Entities. Sie kommen, wenn
  ein Feature sie braucht (Editor, Prefabs, Netzwerk), mit eigenen Arten in `Kind`.
- Mit C++26 lässt sich die Registrierung aus der statischen Reflection erzeugen; die Schnittstelle
  (`TypeInfo`) bleibt dabei gleich.

## Erwogene Alternativen

- **flecs `meta`:** fertig samt JSON, bindet aber Speichern, Editor und C# an flecs und an dessen
  Typbeschreibung.
- **Makros wie `REFLECT(Velocity, x, y)`:** kürzer, aber Module exportieren keine Makros; es bräuchte
  einen weiteren Header neben `<ngine/profile.hpp>` (ADR 0016).
- **Codegenerator (wie Unreal Header Tool):** bequem für große Projekte, aber ein zusätzliches
  Werkzeug im Build, das Module verstehen müsste.
- **Bibliotheken (RTTR, refl-cpp, Boost.Describe):** RTTR ist kaum noch gepflegt und schwer;
  refl-cpp und Boost.Describe arbeiten mit Makros.
