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
| Shader | Slang (`slangc` beim Bauen, seit R2.4 auch als Bibliothek über die COM-API, vcpkg-Port `shader-slang`) | `ngine.shaders`; `ngine.shader_compiler` zur Laufzeit (nur Editor und Tests, [ADR 0034](adr/0034-hot-reload-polling-slang-runtime.md)) | im Einsatz | Eine Sprache für SPIR-V, DXIL, Metal; Module, Reflection | HLSL mit DXC, GLSL mit glslang |
| Mathematik | eigene Typen; GLM nur in den Tests als Vergleich | `ngine.math` ([ADR 0025](adr/0025-own-math-mixed-precision.md)) | im Einsatz (R1.1) | Eigene Typen passen zu Reflection, Modulen und 64-Bit-Weltlage; GLM ist erprobt und taugt als Orakel | GLM hinter Aliasen (erste Fassung von R1.1, ersetzt), DirectXMath |
| JSON | eigener Parser | `ngine::json` | im Einsatz | Klein, geordnete Objekte, genaue Zahlen, Fehler mit Position ([ADR 0020](adr/0020-scene-format-json.md)) | nlohmann/json, glaze, simdjson, yyjson |
| Profiling | Tracy (On-Demand) | `ngine.profile`, Makros in `<ngine/profile.hpp>` ([ADR 0016](adr/0016-profiling-macros.md)) | im Einsatz | CPU- und GPU-Zeiten inkl. Vulkan, live | Optick, Superluminal |
| ECS | flecs 4 (nur der Kern, `ecs_mini`) | `ngine::world::World`, `Entity`, `Query` | im Einsatz | Hierarchien, Prefabs, C-API für C# ([ADR 0003](adr/0003-ecs-flecs.md)); Reflection und Systeme sind eigene ([ADR 0018](adr/0018-own-reflection.md), [ADR 0019](adr/0019-systems-on-job-system.md)) | EnTT, eigenes ECS |
| C#-Laufzeit | .NET 10 über hostfxr (dynamisch geladen, ohne nethost) | `ngine::script::ScriptRuntime` | im Einsatz (D2) | Offizieller Weg für aktuelles .NET, Hot Reload ([ADR 0006](adr/0006-csharp-scripting-dotnet.md), [ADR 0023](adr/0023-dotnet-hosting-hostfxr.md)) | Mono, NativeAOT, nethost, Coral als Vorlage |
| Netzwerk | GameNetworkingSockets (vcpkg, ohne ICE) | `ngine::net::Transport` | im Einsatz (D3) | UDP mit Zuverlässigkeit und Verschlüsselung, BSD | ENet, yojimbo |
| Editor-UI | Dear ImGui 1.93 (Docking-Zweig, ohne Backends) | `ngine.editor_ui` (Adapter `context.cpp`, `widgets.cpp`), gezeichnet über die RHI ([ADR 0029](adr/0029-editor-imgui-over-rhi.md)) | im Einsatz (R2.1) | Standard für Engine-Werkzeuge, Docking, Texturen über `RendererHasTextures`; MIT | Qt, RmlUi, eigenes UI; Backends von ImGui für SDL3 und Vulkan |
| glTF | fastgltf 0.9 (mit simdjson) | `ngine::asset::load_gltf()`, Adapter `gltf_fastgltf.cpp` ([ADR 0027](adr/0027-gltf-assets-material-store.md)) | im Einsatz (R1.3) | Sehr schnell, modernes C++, typsichere Accessoren; MIT | cgltf, tinygltf, Assimp |
| Bilder | stb_image | `ngine::asset::decode_image()`, Adapter `image_stb.cpp` ([ADR 0027](adr/0027-gltf-assets-material-store.md)) | im Einsatz (R1.3) | PNG und JPEG als Quelle, eine Datei, Public Domain | libpng und libjpeg-turbo, WIC |
| Texturen | KTX-Software 4 (mit Basis Universal und zstd, vcpkg-Port `ktx`) | `ngine::asset::decode_ktx2()` (Adapter `ktx_libktx.cpp`), `ngine::cook::encode_ktx2()` (Adapter `ktx_encode.cpp`, [ADR 0036](adr/0036-cooked-assets-ktx2-cache.md)) | im Einsatz (R2.5) | Offizielle Referenz für KTX2, kodiert und transkodiert Basis Universal (UASTC nach BC7 fast verlustfrei); Apache 2.0 | DDS mit DirectXTex, basisu allein, Compressonator |
| Meshlets | meshoptimizer (vcpkg-Port `meshoptimizer`) | `ngine::cook::build_meshlets()`, Adapter `meshlets_meshoptimizer.cpp`; zur Laufzeit der eigene Builder `ngine::meshlet::build()` | im Einsatz (R3.5) | Verbreitete Referenz für Vertex-Cache-Optimierung und Meshlets mit Kegeln, MIT; nur beim Kochen, Hüllen rechnet die Engine selbst ([ADR 0043](adr/0043-gpu-driven-meshlets.md)) | eigener Builder allein (bleibt Rückfall), DirectXMesh |
| Tangenten | MikkTSpace (vcpkg-Port `mikktspace`) | `ngine::cook::generate_tangents()`, Adapter `tangents_mikktspace.cpp` | im Einsatz (R2.5) | Standard, mit dem Werkzeuge Normalenkarten backen; zwei Dateien, zlib-artige Lizenz | eigene Tangenten pro Dreieck (bis R2.4, TD-21) |
| Physik | Jolt Physics 5.6 | `PhysicsWorld` | in Nutzung (R4.2): eigener Overlay-Port `cmake/vcpkg-ports/joltphysics` mit doppelter Genauigkeit, nur SSE2, RTTI an ([ADR 0045](adr/0045-physics-jolt-double-precision.md)) | Modern, mehrkernfähig, MIT | PhysX 5, Bullet |
| Audio | miniaudio | `AudioDevice` | geplant (R4) | Eine Datei, Public Domain, 3D-Klang | FMOD, Wwise, OpenAL Soft |
