# 0003: Objektmodell: ECS mit flecs und Entity-API

- **Status:** angenommen
- **Datum:** 2026-10-24

## Kontext

Das Objektmodell bestimmt, wie Dinge in der Welt existieren, und ist später am schwersten zu ändern.
Zur Wahl standen eine Objekt-Hierarchie (Unreal), GameObject mit Komponenten (klassisches Unity) und
ein Entity Component System (Unity DOTS, Bevy).

## Entscheidung

Ein ECS als Kern mit flecs als Bibliothek. Darüber liegt eine bequeme Entity-API, die sich in C++ und
C# wie ein GameObject anfühlt; die Daten liegen trotzdem im ECS.

## Konsequenzen

- Replikation, Speichern und Inspector arbeiten einheitlich auf Komponenten.
- Viele gleichartige Objekte (Pflanzen, Gegner, Bauteile) sind schnell und parallel verarbeitbar.
- Neue Features sind neue Komponenten und Systeme, ohne Bestehendes anzufassen.
- Die flecs-C-API erleichtert die C#-Anbindung.
- ECS verlangt Umdenken; die Entity-API soll die Lernkurve abflachen.

## Erwogene Alternativen

- **Objekt-Hierarchie:** intuitiv, aber starr und schwer parallel.
- **GameObject und Komponenten:** zugänglich, aber langsam bei vielen Objekten.
- **EnTT:** schlanker, aber ohne Hierarchien, Prefabs und Reflection; bleibt Ausweichlösung.
