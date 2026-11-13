# 0017: Plattform-Baseline Ubuntu 24.04, Vulkan 1.3 mit optionalen Feature-Stufen

- **Status:** angenommen
- **Datum:** 2026-11-01

## Kontext

Zwei frühere Entscheidungen haben die Einrichtung unter Linux schwer gemacht und das Notebook
ausgeschlossen:

- [ADR 0014](0014-modern-gpu-baseline.md) hat Vulkan 1.4, Mesh Shader und Raytracing zur Pflicht
  gemacht. Die Intel-Grafik des Notebooks (Linux Mint 22, keine dedizierte GPU) lief damit nur über
  lavapipe, also in Software auf der CPU.
- [ADR 0008](0008-linux-first-clang.md) und [ADR 0002](0002-cpp23-modules-cmake-vcpkg.md) haben Clang
  23 aus apt.llvm.org und CMake 3.30 verlangt. Beides liefert Ubuntu 24.04 nicht; nötig waren zwei
  fremde Paketquellen und eine Sonderbehandlung der Mint-Codenamen.

Die Bibliotheken selbst waren nie das Problem: SDL3, volk, Vulkan-Headers, VMA, Slang, Tracy und
Catch2 kommen schon über vcpkg. Es geht um Compiler, CMake und die GPU-Anforderung.

Ziel bleibt eine moderne Engine, die starke Hardware ausnutzt. Sie soll aber auf jeder halbwegs
aktuellen GPU funktionieren und sich auf einem frischen Ubuntu ohne Umwege bauen lassen.

## Entscheidung

### Plattform

- **Linux-Baseline ist Ubuntu 24.04 LTS.** Alles zum Bauen kommt aus den offiziellen Paketquellen:
  Clang 20 (`clang-20`, `clang-tools-20`), `libstdc++-14-dev`, CMake 3.28, Ninja, Mesa und die
  Entwicklerpakete für SDL3. Distributionen auf dieser Basis (Linux Mint 22, Pop!_OS 24.04) und
  neuere Distributionen (Fedora 40+, Debian 13, Arch) erfüllen die Baseline ebenfalls.
- **Mindestversionen:** CMake 3.28, Clang 20, GCC 14, aktuelles MSVC. CMake prüft die Compiler-Version
  beim Konfigurieren und bricht mit einer klaren Meldung ab.
- **Entscheidend ist der Grafiktreiber, nicht die Distribution:** Mesa 24 oder neuer liefert Vulkan 1.3
  auf Intel- (ab Gen9, z. B. UHD 620) und AMD-GPUs. Ubuntu 24.04 bringt Mesa über seine Updates mit.
- Neuere Werkzeuge (Clang 23, aktuelles CMake) sind erlaubt, aber nicht nötig. Die Linux-CI baut mit
  der Baseline, damit sie nicht unbemerkt bricht.
- Bibliotheken kommen weiterhin über vcpkg im Manifest-Modus, nicht aus der Distribution.

### Vulkan

- **Pflicht (Mindestanforderung):** Vulkan 1.3 mit Dynamic Rendering, Synchronization2, Maintenance4,
  Bindless (Descriptor Indexing mit Runtime Arrays, Partially Bound, Variable Count, Non-Uniform
  Indexing, Update After Bind), Buffer Device Address, Timeline Semaphores, Scalar Block Layout und
  Draw Parameters. Seit R1.2 dazu für den bindlosen Satz mit Storage-Texturen: Update After Bind
  und Non-Uniform Indexing auch für Storage-Texturen, Update Unused While Pending sowie Lesen und
  Schreiben von Storage-Texturen ohne Format ([ADR 0026](0026-render-graph-bindless.md)). Seit
  R3.5 Multi Draw Indirect und Draw Indirect Count für den Rückfallpfad des GPU-getriebenen
  Zeichnens ([ADR 0043](0043-gpu-driven-meshlets.md)). Fehlt
  etwas, startet die Engine nicht und nennt pro GPU, was fehlt.
- **Optionale Feature-Stufen:** werden beim Start erkannt und nur eingeschaltet, wenn die GPU sie kann.
  Fürs Erste genau zwei:
  - **Mesh Shader:** `VK_EXT_mesh_shader` mit Task und Mesh Shader
  - **Raytracing:** `VK_KHR_acceleration_structure`, `VK_KHR_deferred_host_operations` und
    `VK_KHR_ray_query`
- **Vulkan 1.4** wird genutzt, wenn der Treiber es anbietet; das Device läuft mit der Version der GPU,
  höchstens 1.4. Gebaut wird gegen die aktuellen Vulkan-Header aus vcpkg.
- Die RHI meldet die eingeschalteten Stufen in `Capabilities::features`. Über
  `DeviceDesc::disabled_features` (Sandbox: `--baseline`) lassen sie sich abschalten, um die
  Rückfallpfade auf starker Hardware zu prüfen.
- Die Auswahl der GPU bevorzugt dedizierte vor integrierten GPUs; bei gleichem Typ gewinnt die mit mehr
  Stufen.
- Jedes Feature, das eine Stufe nutzt, braucht einen Rückfallpfad auf der Mindestanforderung, z. B.
  klassische Vertex-Pipeline statt Mesh Shader und Shadow Maps statt Raytracing-Schatten. Neue Stufen
  kommen erst dazu, wenn ein Feature sie braucht.

## Konsequenzen

- Einrichtung unter Ubuntu 24.04 und Linux Mint 22 mit einem einzigen `apt install` ohne fremde
  Paketquellen.
- Die Engine läuft auf dem Notebook mit der Intel-Grafik in Hardware, ohne Mesh Shader und Raytracing.
- Die Engine läuft auf deutlich mehr GPUs: unter anderem NVIDIA ab GTX 900, AMD ab RX 400 und Intel
  ab Gen9, jeweils mit aktuellem Treiber.
- Doppelter Aufwand bei Geometrie und Beleuchtung: Mesh-Shader- und Raytracing-Pfade brauchen einen
  Raster-Ersatz, und beide Pfade müssen getestet werden. Deshalb wenige Stufen und Tests ohne Stufen
  (CI: Sandbox mit `--baseline`).
- Der Vulkan-Loader 1.3.275 aus Ubuntu 24.04 reicht weiterhin. Auch dessen Validation Layers (1.3)
  funktionieren; bei einer GPU mit Vulkan 1.4 warnen sie nur, dass sie älter sind. Die CI nutzt
  aktuelle Layers aus vcpkg.
- Neueste Sprach- und Bibliotheksfunktionen aus Clang 21 bis 23 nutzen wir erst, wenn die Baseline sie
  mitbringt. Bisher braucht der Code nichts davon.
- Ein späteres D3D12-Backend setzt entsprechend Feature Level 12_1 mit optionalem DirectX 12 Ultimate
  voraus.

## Erwogene Alternativen

- **Moderne GPU als Pflicht (ADR 0014):** ein einziger Renderpfad, aber das Notebook und viele Spieler
  bleiben außen vor.
- **Vulkan 1.4 als Pflicht, Stufen optional:** schließt Intel Gen9 unter Windows und ältere Treiber
  aus, ohne dass wir ein 1.4-Feature zwingend brauchen.
- **Viele feine Stufen (Variable Rate Shading, Work Graphs usw.):** jede Stufe kostet einen
  Rückfallpfad und Tests; erst bei Bedarf.
- **Clang 23 und CMake 3.30 behalten:** aktueller, aber zwei fremde Paketquellen für jeden Entwickler.
- **Abhängigkeiten über CMake FetchContent statt vcpkg:** löst das eigentliche Problem nicht
  (Compiler und CMake) und baut jede Bibliothek in jedem Build-Verzeichnis neu.
