# Architekturentscheidungen (ADRs)

Jede Entscheidung, die schwer rückgängig zu machen ist, bekommt ein kurzes Dokument: Kontext,
Entscheidung, Konsequenzen und erwogene Alternativen. ADRs werden nicht gelöscht; eine überholte
Entscheidung bekommt den Status `ersetzt durch NNNN`.

Neue ADRs: [Vorlage](template.md) kopieren, nächste freie Nummer vergeben, im selben PR wie der
betroffene Code einreichen.

| Nr. | Entscheidung | Status |
| --- | --- | --- |
| [0001](0001-docs-as-code.md) | Dokumentation als Code im Repository | angenommen |
| [0002](0002-cpp23-modules-cmake-vcpkg.md) | C++23 mit Modulen, CMake und vcpkg | angenommen |
| [0003](0003-ecs-flecs.md) | Objektmodell: ECS mit flecs und Entity-API | angenommen |
| [0004](0004-vulkan-rhi-feature-tiers.md) | Vulkan 1.3 hinter einer RHI, Feature-Stufen, Slang | angenommen |
| [0005](0005-smooth-voxel-terrain.md) | Glattes Voxel-Terrain mit Dichtefeld | angenommen |
| [0006](0006-csharp-scripting-dotnet.md) | Scripting in C# über eingebettetes .NET | angenommen |
| [0007](0007-server-authoritative-multiplayer.md) | Server-autoritativer Multiplayer, Headless-Start | angenommen |
| [0008](0008-linux-first-clang.md) | Linux als Hauptplattform, Clang als Hauptcompiler | angenommen |
| [0009](0009-third-party-behind-interfaces.md) | Fremdbibliotheken nur hinter Engine-Schnittstellen | angenommen |
| [0010](0010-own-logger.md) | Eigener Logger statt spdlog | angenommen |
| [0011](0011-unified-process.md) | Vereinfachter Unified Process | angenommen |
| [0012](0012-64bit-world-coordinates.md) | 64-Bit-Weltkoordinaten, kamerarelatives Rendering | angenommen |
