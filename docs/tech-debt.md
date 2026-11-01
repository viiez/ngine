# Technische Schulden

Bekannte Abkürzungen und Probleme mit Grund und Plan. Regeln und Ablauf stehen im
[Prozess](process.md#technische-schulden). Abgebaute Einträge wandern mit Datum und PR nach unten.

## Offen

| ID | Was | Warum es so ist | Plan | Frist |
| --- | --- | --- | --- | --- |
| TD-02 | Layout-Übergänge mit groben Barrieren (`ALL_COMMANDS`, `MEMORY_WRITE`) | Für ein Dreieck reicht es; feine Barrieren ohne Render-Graph wären Handarbeit an jeder Stelle | Render-Graph (REND-05) berechnet die Barrieren | R1 |
| TD-03 | Swapchain-Neuaufbau und `execute()` warten blockierend auf die GPU (`vkDeviceWaitIdle` bzw. Fence) | Einfach und sicher, solange nur die Sandbox zeichnet | Neuaufbau mit alter Swapchain (`oldSwapchain`) ohne Komplett-Stopp; Uploads über eigene Queue und Timeline Semaphores | R1 |
| TD-06 | Die CI baut die Validation Layers aus den Quellen (vcpkg, rund 25 Minuten), sobald sich ihre Pakete ändern, z. B. bei einer neuen vcpkg-Baseline | Schnellster Weg zu Layers 1.4 in A3; danach liegen sie im Cache | Eigenes CI-Container-Image (Clang, Mesa, Layers) in der GitHub Container Registry prüfen, alternativ das LunarG SDK als vorgebautes Paket im Cache | nächste Baseline-Anhebung |
| TD-08 | Speicher, den flecs selbst anfordert (Tabellen, Abfragen), erscheint nicht im Budget der Kategorie Welt | flecs fordert über `ecs_os_api` an und gibt ohne Größe frei; für die Größe bräuchte jede Allokation einen Kopf | `ecs_os_api` (malloc, realloc, calloc, free) auf eigene Funktionen mit Größenkopf umleiten, die in `Category::World` verbuchen | R2 |
| TD-09 | `Entity::set/get<T>()` sucht Typ und Komponente jedes Mal in zwei Hashtabellen (Reflection und Welt) | Einfach und für A4 schnell genug; vor Optimierung erst messen | Index pro Typ in der Registry vergeben und die Komponenten-ID der Welt in einem Array danach ablegen; mit einem Benchmark in Tracy prüfen | R2 |
| TD-10 | `ngine.voxel` hat einen eigenen kleinen `Vec3`, die Sandbox eigene Matrizen und eine Kamera | Die Mathematik-Bibliothek (CORE-10) kommt erst in R1; D1 durfte Wegwerf-Code enthalten | Auf die Typen aus CORE-10 umstellen, Kamera in den Renderer (REND-08) | R1 |
| TD-11 | GPU-Puffer liegen immer im CPU-sichtbaren Speicher (ohne Resizable BAR im Systemspeicher) | Einfachster Weg für D1; Hochladen über Staging braucht eine Transfer-Queue und Synchronisation | Puffer für statische Geometrie im GPU-Speicher, Upload über Staging-Puffer und Timeline Semaphores (zusammen mit TD-03) | R1 |
| TD-12 | Sandbox und Tests finden die C#-Assemblies über einen fest einkompilierten Pfad ins Build-Verzeichnis (`NGINE_DOTNET_DIR`) | Für den Durchstich der kürzeste Weg; es gibt noch kein Paket und keine Asset-Pipeline | Assemblies neben die ausführbare Datei kopieren und relativ zu ihr suchen; später über die Asset-Pipeline (R2) | R2 |
| TD-13 | Die Replikation schickt in jedem Tick den vollen Zustand aller replizierten Entities, Gleitkommazahlen ungepackt; schon 33 Entities brauchen rund 1,2 KB pro Schnappschuss (etwa 38 KB/s pro Client bei 30 Hz) und überschreiten damit ein UDP-Paket | Einfachster Weg für D3; Verluste heilen von selbst, und die Messung zeigt, wo es eng wird | Deltas gegen den letzten bestätigten Schnappschuss, quantisierte Positionen, Relevanz pro Client (NET-04 in R5) | R5 |
| TD-14 | `ngine.net` hat keinen Test für Paketverlust und Latenz; die CI prüft nur localhost | Für den Durchstich reichen localhost und zwei Prozesse | Die Simulation von GameNetworkingSockets (`FakePacketLoss`, `FakePacketLag`) über eine Option des Transports freischalten und die Replikation damit testen | R5 |

## Abgebaut

| ID | Was | Wann |
| --- | --- | --- |
| TD-07 | Budget-Zähler ohne Cache-Line-Abstand, Verbrauch nicht in Tracy: Zähler pro Kategorie mit `alignas(64)`, die Sandbox meldet `memory::stats()` in jedem Frame als Plot pro Kategorie | 2026-11-01, A4 |
| TD-01 | `engine/rhi/vulkan/device.cpp` mit über 1300 Zeilen: aufgeteilt in interne Partitionen (`:common`, `:instance`, `:physical_device`, `:resources`, `:command_list`, `:swapchain`), Instanz und Swapchain als eigene Klassen | 2026-10-31, PR #5 |
| TD-04 | Validation Layers 1.3 aus Ubuntu in der CI: jetzt 1.4 über das vcpkg-Feature `validation` und `VK_ADD_LAYER_PATH` | 2026-10-31, PR #5 |
| TD-05 | `ilammy/msvc-dev-cmd` auf Node.js 20: ersetzt durch einen eigenen Schritt mit `vswhere` und `VsDevCmd.bat` | 2026-10-31, PR #5 |
