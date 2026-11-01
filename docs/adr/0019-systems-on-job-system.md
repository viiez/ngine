# 0019: Systeme auf dem eigenen Job-System statt der Pipeline von flecs

- **Status:** angenommen
- **Datum:** 2026-11-01

## Kontext

Systeme verarbeiten jeden Frame alle Entities mit bestimmten Komponenten. flecs bringt dafür eine
Pipeline mit eigenen Worker-Threads mit. Die Engine hat aber schon ein Job-System
([ADR 0015](0015-own-job-system.md)), auf dem auch Voxel-Meshing, Rendering und Physik laufen
sollen. Zwei Thread-Pools würden um die Kerne konkurrieren, und Tracy zeigte zwei Welten.

## Entscheidung

- Die Welt nutzt nur den Kern von flecs (`ecs_mini`), ohne Pipeline, Systeme und Threads.
- `world::Schedule` sammelt Systeme. Ein System ist eine Funktion über einer Abfrage, etwa
  `add<const Velocity, Position>(name, f)`. `const` heißt lesen, sonst schreiben.
- Zwei Systeme stehen in Konflikt, wenn eines eine Komponente schreibt, die das andere liest oder
  schreibt. Ein System wartet auf alle früher hinzugefügten, mit denen es in Konflikt steht; alle
  anderen laufen parallel. Daraus entsteht ein `jobs::TaskGraph`, der einmal gebaut und jeden Frame
  ausgeführt wird.
- Vor jedem Lauf sammelt der aufrufende Thread die passenden Chunks (Tabellen mit Spaltenzeigern).
  In den Jobs ruft niemand flecs auf; die Systeme arbeiten nur auf diesen Spalten. Große Systeme
  teilen ihre Chunks zusätzlich mit `parallel_for` auf (`SystemOptions::grain`).
- Während der Systeme sind Änderungen über die Welt verboten (`verify`): keine neuen Entities, keine
  Komponenten hinzufügen oder entfernen. Das kommt nach `run()`.

## Konsequenzen

- Ein Thread-Pool für die ganze Engine; Systeme erscheinen in Tracy wie alle anderen Jobs.
- flecs muss nicht threadsicher genutzt werden; ThreadSanitizer prüft unseren Teil.
- Die Reihenfolge bei Konflikten ist die von `add()`. Feinere Steuerung (Phasen, explizites
  „vor“/„nach“) fehlt noch und kommt mit dem Gameplay (R4).
- Strukturänderungen aus Systemen heraus (z. B. Gegner spawnen) brauchen später einen
  Befehlspuffer, der nach `run()` abgearbeitet wird. Bis ein Feature ihn braucht, bleibt es beim
  Verbot.
- Was flecs-Systeme zusätzlich bieten (Zeitgeber, Rate-Filter, Observer in der Pipeline), müssen wir
  bei Bedarf selbst bauen.

## Erwogene Alternativen

- **Pipeline und Threads von flecs:** fertig und erprobt, aber ein zweiter Thread-Pool neben
  `ngine.jobs` und eine enge Bindung an flecs.
- **flecs-Pipeline mit eigenem Task-Backend (`ecs_os_api` Tasks):** flecs startet dann Tasks über
  unsere Funktionen, plant aber selbst; die Planung kennte unsere Konflikte nicht.
- **Alle Systeme nacheinander, nur innerhalb parallel:** einfacher, verschenkt aber Parallelität
  zwischen unabhängigen Systemen.
