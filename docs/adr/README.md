# Architekturentscheidungen (ADRs)

Jede Entscheidung, die schwer rückgängig zu machen ist, bekommt ein kurzes Dokument: Kontext,
Entscheidung, Konsequenzen und erwogene Alternativen. ADRs werden nicht gelöscht; eine überholte
Entscheidung bekommt den Status `ersetzt durch NNNN`.

Neue ADRs: [Vorlage](template.md) kopieren, nächste freie Nummer vergeben, im selben PR wie der
betroffene Code einreichen.

| Nr. | Entscheidung | Status |
| --- | --- | --- |
| [0001](0001-docs-as-code.md) | Dokumentation als Code im Repository | angenommen |
| [0002](0002-cpp23-modules-cmake-vcpkg.md) | C++23 mit Modulen, CMake und vcpkg | angenommen, CMake-Version ersetzt durch 0017 |
| [0003](0003-ecs-flecs.md) | Objektmodell: ECS mit flecs und Entity-API | angenommen |
| [0004](0004-vulkan-rhi-feature-tiers.md) | Vulkan 1.3 hinter einer RHI, Feature-Stufen, Slang | teilweise ersetzt durch 0014 und 0017 |
| [0005](0005-smooth-voxel-terrain.md) | Glattes Voxel-Terrain mit Dichtefeld | angenommen |
| [0006](0006-csharp-scripting-dotnet.md) | Scripting in C# über eingebettetes .NET | angenommen |
| [0007](0007-server-authoritative-multiplayer.md) | Server-autoritativer Multiplayer, Headless-Start | angenommen |
| [0008](0008-linux-first-clang.md) | Linux als Hauptplattform, Clang als Hauptcompiler | angenommen, Compiler-Version ersetzt durch 0017 |
| [0009](0009-third-party-behind-interfaces.md) | Fremdbibliotheken nur hinter Engine-Schnittstellen | angenommen |
| [0010](0010-own-logger.md) | Eigener Logger statt spdlog | angenommen |
| [0011](0011-unified-process.md) | Vereinfachter Unified Process | angenommen |
| [0012](0012-64bit-world-coordinates.md) | 64-Bit-Weltkoordinaten, kamerarelatives Rendering | angenommen |
| [0013](0013-rhi-design.md) | Aufbau der RHI: abstrakte Klassen, Backend-Fabrik, SurfaceDesc | angenommen |
| [0014](0014-modern-gpu-baseline.md) | Moderne GPU als Mindestanforderung: Vulkan 1.4, Mesh Shader, Raytracing | ersetzt durch 0017 |
| [0015](0015-own-job-system.md) | Eigenes Job-System mit Work-Stealing, ohne Fibers | angenommen |
| [0016](0016-profiling-macros.md) | Profiling mit Tracy über Makros in einem Header | angenommen |
| [0017](0017-platform-baseline-feature-tiers.md) | Plattform-Baseline Ubuntu 24.04, Vulkan 1.3 mit optionalen Feature-Stufen | angenommen |
| [0018](0018-own-reflection.md) | Eigene Reflection mit Builder-API, flecs nur als Speicher | angenommen |
| [0019](0019-systems-on-job-system.md) | Systeme auf dem eigenen Job-System statt der Pipeline von flecs | angenommen |
| [0020](0020-scene-format-json.md) | Szenen als JSON mit eigenem Parser | angenommen |
| [0021](0021-surface-nets-8bit-density.md) | Surface Nets und 8-Bit-Dichte für das Voxel-Terrain | vorgeschlagen |
| [0022](0022-vertex-pulling-bda.md) | Geometrie per Vertex Pulling über Buffer Device Address | vorgeschlagen |
