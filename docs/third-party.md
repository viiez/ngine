# Fremdbibliotheken

Jede Bibliothek steckt hinter einer Engine-Schnittstelle und ist damit austauschbar
([ADR 0009](adr/0009-third-party-behind-interfaces.md)). Auswahlkriterien: läuft gleich gut unter Linux
und Windows, freie Lizenz ohne Gebühren, aktiv gepflegt, schmale API.

**Status:** `im Einsatz` · `geplant` · `verworfen`

| Bereich | Bibliothek | Schnittstelle | Status | Warum diese | Erwogene Alternativen |
| --- | --- | --- | --- | --- | --- |
| Paketverwaltung | vcpkg | – | im Einsatz | Gleiche Pakete auf Linux und Windows, Versionen fest im Manifest | Conan, CPM.cmake / FetchContent |
| Tests | Catch2 3 | – | im Einsatz | Verbreitet, gute CMake-Integration | GoogleTest, doctest |
| Logging | eigener Logger | `ngine::log` | im Einsatz | Mit `std::format` in wenigen Zeilen machbar ([ADR 0010](adr/0010-own-logger.md)) | spdlog (verworfen, als Sink nachrüstbar), quill |
| Fenster, Eingabe | SDL3 | `ngine::platform::Window` | im Einsatz | Wayland und X11, Gamepads, Vulkan-Oberfläche, zlib-Lizenz | GLFW, eigene Plattformschicht |
| Vulkan-Laden | volk | Vulkan-RHI | im Einsatz | Lädt Funktionen direkt vom Treiber; ohne volk linkt NGine direkt gegen den Loader | Loader direkt linken, Vulkan-Hpp |
| GPU-Speicher | Vulkan Memory Allocator | Vulkan-RHI | im Einsatz | De-facto-Standard | Eigener Allokator (später) |
| Shader | Slang (`slangc` beim Bauen) | `ngine.shaders`; später `ShaderCompiler` zur Laufzeit | im Einsatz | Eine Sprache für SPIR-V, DXIL, Metal; Module, Reflection | HLSL mit DXC, GLSL mit glslang |
| Mathematik | GLM | eigene Typen | geplant (R1) | Schneller Start, Shader-Schreibweise | DirectXMath, eigene SIMD-Bibliothek |
| Profiling | Tracy (On-Demand) | `ngine.profile`, Makros in `<ngine/profile.hpp>` ([ADR 0016](adr/0016-profiling-macros.md)) | im Einsatz | CPU- und GPU-Zeiten inkl. Vulkan, live | Optick, Superluminal |
| ECS | flecs | `World`, `Entity` | geplant (A4) | Hierarchien, Prefabs, Reflection, C-API für C# ([ADR 0003](adr/0003-ecs-flecs.md)) | EnTT, eigenes ECS |
| C#-Laufzeit | .NET über hostfxr | `ScriptRuntime` | geplant (D2) | Offizieller Weg für aktuelles .NET, Hot Reload ([ADR 0006](adr/0006-csharp-scripting-dotnet.md)) | Mono, NativeAOT, Coral als Vorlage |
| Netzwerk | GameNetworkingSockets | `Transport` | geplant (D3) | UDP mit Zuverlässigkeit und Verschlüsselung, BSD | ENet, yojimbo |
| Editor-UI | Dear ImGui (Docking) | `EditorUI` | geplant (R2) | Standard für Engine-Werkzeuge | Qt, RmlUi, eigenes UI |
| glTF | fastgltf | Importer | geplant (R1) | Sehr schnell, modernes C++ | cgltf, tinygltf, Assimp |
| Texturen | KTX-Software, stb_image | Importer | geplant (R1) | KTX2 mit Basis Universal; PNG/JPG als Quelle | DDS mit DirectXTex |
| Physik | Jolt Physics | `PhysicsWorld` | geplant (R4) | Modern, mehrkernfähig, MIT | PhysX 5, Bullet |
| Audio | miniaudio | `AudioDevice` | geplant (R4) | Eine Datei, Public Domain, 3D-Klang | FMOD, Wwise, OpenAL Soft |
