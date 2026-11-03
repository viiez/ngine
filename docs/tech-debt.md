# Technische Schulden

Bekannte Abkürzungen und Probleme mit Grund und Plan. Regeln und Ablauf stehen im
[Prozess](process.md#technische-schulden). Abgebaute Einträge wandern mit Datum und PR nach unten.

## Offen

| ID | Was | Warum es so ist | Plan | Frist |
| --- | --- | --- | --- | --- |
| TD-06 | Die CI baut die Validation Layers aus den Quellen (vcpkg, rund 25 Minuten), sobald sich ihre Pakete ändern, z. B. bei einer neuen vcpkg-Baseline | Schnellster Weg zu Layers 1.4 in A3; danach liegen sie im Cache | Eigenes CI-Container-Image (Clang, Mesa, Layers) in der GitHub Container Registry prüfen, alternativ das LunarG SDK als vorgebautes Paket im Cache | nächste Baseline-Anhebung |
| TD-08 | Speicher, den flecs selbst anfordert (Tabellen, Abfragen), erscheint nicht im Budget der Kategorie Welt | flecs fordert über `ecs_os_api` an und gibt ohne Größe frei; für die Größe bräuchte jede Allokation einen Kopf | `ecs_os_api` (malloc, realloc, calloc, free) auf eigene Funktionen mit Größenkopf umleiten, die in `Category::World` verbuchen | R2 |
| TD-09 | `Entity::set/get<T>()` sucht Typ und Komponente jedes Mal in zwei Hashtabellen (Reflection und Welt) | Einfach und für A4 schnell genug; vor Optimierung erst messen | Index pro Typ in der Registry vergeben und die Komponenten-ID der Welt in einem Array danach ablegen; mit einem Benchmark in Tracy prüfen | R2 |
| TD-10 | Die Kamera der Voxel-Demo lebt in der Sandbox | D1 durfte Wegwerf-Code enthalten; seit R1.1 nutzen Voxel und Sandbox `ngine.math` statt eigener Vektoren und Matrizen | Kamera als Komponente in den Renderer (REND-08) | R1.3 |
| TD-12 | Sandbox und Tests finden die C#-Assemblies über einen fest einkompilierten Pfad ins Build-Verzeichnis (`NGINE_DOTNET_DIR`) | Für den Durchstich der kürzeste Weg; es gibt noch kein Paket und keine Asset-Pipeline | Assemblies neben die ausführbare Datei kopieren und relativ zu ihr suchen; später über die Asset-Pipeline (R2) | R2 |
| TD-13 | Die Replikation schickt in jedem Tick den vollen Zustand aller replizierten Entities, Gleitkommazahlen ungepackt; schon 33 Entities brauchen rund 1,2 KB pro Schnappschuss (etwa 38 KB/s pro Client bei 30 Hz) und überschreiten damit ein UDP-Paket | Einfachster Weg für D3; Verluste heilen von selbst, und die Messung zeigt, wo es eng wird | Deltas gegen den letzten bestätigten Schnappschuss, quantisierte Positionen, Relevanz pro Client (NET-04 in R5) | R5 |
| TD-14 | `ngine.net` hat keinen Test für Paketverlust und Latenz; die CI prüft nur localhost | Für den Durchstich reichen localhost und zwei Prozesse | Die Simulation von GameNetworkingSockets (`FakePacketLoss`, `FakePacketLag`) über eine Option des Transports freischalten und die Replikation damit testen | R5 |
| TD-15 | `update_transforms()` berechnet bei jedem Aufruf alle Weltlagen neu, in einem Thread; rund 6 ms für 101 000 Entities (Release) | Einfachster korrekter Weg für R1.1; die Sandbox hat wenige Entities | Änderungserkennung (nur geänderte Teilbäume neu rechnen) und Ebenen der Hierarchie parallel über das Job-System; vorher mit Tracy an einer großen Szene messen | R2 |
| TD-16 | Transiente Texturen des Render-Graphs teilen keinen Speicher (kein Aliasing) und leben den ganzen Frame; der `TexturePool` sucht linear nach gleicher Beschreibung | Reicht für wenige Passes in R1; Aliasing braucht Speicherbarrieren und Platzierung nach Lebensdauer, die der Plan schon kennt | Lebensdauer aus `Plan::lifetimes` nutzen, Texturen mit disjunkter Lebensdauer in denselben Speicher legen (VMA-Aliasing); erst messen, wie viel Speicher es spart | R3 |
| TD-17 | Uploads laufen auf der Grafik-Queue, mit je einer vollen Barriere vor und nach den Kopien | Einfach und korrekt; eine Transfer-Queue braucht Übergaben der Queue-Familie | Eigene Transfer-Queue mit Timeline Semaphore für große Uploads (Streaming von Assets und Terrain), feinere Barrieren pro Ziel | R2 |
| TD-18 | Die alte Swapchain und ihre Semaphoren werden zwei Abgaben nach dem Neuaufbau freigegeben, weil die Timeline die Präsentation nicht abdeckt | Ohne `VK_EXT_swapchain_maintenance1` meldet Vulkan nicht, wann eine Präsentation fertig ist; der Abstand reicht in der Praxis, ist aber nicht garantiert | `VK_EXT_swapchain_maintenance1` (Present-Fences) nutzen, wo vorhanden; vorher auf dem PC mit Validation Layers beobachten | R2 |

## Abgebaut

| ID | Was | Wann |
| --- | --- | --- |
| TD-02 | Layout-Übergänge mit groben Barrieren: Die RHI setzt keine Barrieren mehr selbst; `CommandList::barrier()` mit `ResourceState`, die der Render-Graph pro Pass berechnet ([ADR 0026](adr/0026-render-graph-bindless.md)) | 2026-11-03, R1.2 |
| TD-03 | Blockierender Swapchain-Neuaufbau: Neuaufbau mit `oldSwapchain`, alte Swapchain über die Löschliste (Rest siehe TD-18); `execute()` wartet weiterhin, ist aber nur noch für Werkzeuge und Tests, Uploads laufen ohne Warten | 2026-11-03, R1.2 |
| TD-11 | GPU-Puffer nur im CPU-sichtbaren Speicher: `MemoryUsage::Gpu` mit `Device::upload()` über einen Staging-Ring und die Timeline (Transfer-Queue siehe TD-17) | 2026-11-03, R1.2 |
| TD-07 | Budget-Zähler ohne Cache-Line-Abstand, Verbrauch nicht in Tracy: Zähler pro Kategorie mit `alignas(64)`, die Sandbox meldet `memory::stats()` in jedem Frame als Plot pro Kategorie | 2026-11-01, A4 |
| TD-01 | `engine/rhi/vulkan/device.cpp` mit über 1300 Zeilen: aufgeteilt in interne Partitionen (`:common`, `:instance`, `:physical_device`, `:resources`, `:command_list`, `:swapchain`), Instanz und Swapchain als eigene Klassen | 2026-10-31, PR #5 |
| TD-04 | Validation Layers 1.3 aus Ubuntu in der CI: jetzt 1.4 über das vcpkg-Feature `validation` und `VK_ADD_LAYER_PATH` | 2026-10-31, PR #5 |
| TD-05 | `ilammy/msvc-dev-cmd` auf Node.js 20: ersetzt durch einen eigenen Schritt mit `vswhere` und `VsDevCmd.bat` | 2026-10-31, PR #5 |
