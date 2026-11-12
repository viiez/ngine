# NGine

Eine moderne 3D-Engine in C++23 mit Vulkan, Voxel-Terrain im Stil von Enshrouded, C#-Scripting,
eigenem Editor und Multiplayer. Lernprojekt und Grundlage für ein eigenes Spiel.

**Stand:** Ausarbeitung abgeschlossen, als Nächstes folgt R1 (Rendering-Grundlagen). Die Sandbox zeigt
veränderbares Voxel-Terrain, lädt C#-Skripte und repliziert Entities vom Server zum Client.
Siehe [Roadmap](docs/roadmap.md).

## Bauen

Voraussetzungen:

- CMake 3.28 oder neuer und Ninja
- [vcpkg](https://github.com/microsoft/vcpkg), Umgebungsvariable `VCPKG_ROOT` gesetzt; vcpkg holt SDL3,
  volk, VMA, Slang, flecs, GameNetworkingSockets und Catch2 selbst
- Linux: Clang 20 oder neuer · Windows: Visual Studio (MSVC), aktuelle Version
- Eine GPU mit Vulkan 1.3 (unter Linux Mesa 24 oder neuer; z. B. Intel ab UHD 620, AMD ab RX 400,
  NVIDIA ab GTX 900). Mesh Shader, Raytracing und Vulkan 1.4 nutzt die Engine, wenn die GPU sie kann
  ([ADR 0017](docs/adr/0017-platform-baseline-feature-tiers.md)). Für Debug-Builds die Vulkan
  Validation Layers (optional).

### Ubuntu 24.04 / Linux Mint 22

Baseline ist Ubuntu 24.04. Alles kommt aus den offiziellen Paketquellen, fremde Quellen braucht es
nicht. Die ausführliche Anleitung inklusive VS Code steht in [docs/setup.md](docs/setup.md).

```sh
sudo apt update
sudo apt install git curl zip unzip tar pkg-config cmake ninja-build \
    clang-20 clang-tools-20 libstdc++-14-dev dotnet-sdk-10.0 \
    mesa-vulkan-drivers vulkan-validationlayers \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
    libxft-dev libxkbcommon-dev libwayland-dev libegl1-mesa-dev
```

Die X11- und Wayland-Pakete braucht vcpkg, um SDL3 zu bauen. Ein neueres Clang (etwa aus
[apt.llvm.org](https://apt.llvm.org)) funktioniert ebenfalls; dann im eigenen `CMakeUserPresets.json`
den Compiler eintragen. Andere Distributionen (Fedora 40+, Debian 13, Arch) brauchen dieselben
Werkzeuge in mindestens diesen Versionen.

### Bauen, testen, starten

```sh
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
./build/linux-clang-debug/apps/sandbox/ngine_sandbox
./build/linux-clang-debug/apps/editor/ngine_editor
```

Unter Windows dieselben Befehle mit `windows-msvc-debug` in einer Developer-Konsole. Den Vulkan-Treiber
bringt der Grafiktreiber mit; die Validation Layers kommen mit dem Vulkan SDK.

Optionen der Sandbox: `--headless` (ohne Fenster und GPU), `--frames <n>` (nach n Frames beenden),
`--no-vsync`, `--baseline` (ohne Mesh Shader und Raytracing, auch wenn die GPU sie kann). Ohne
Demo-Option zeigt die Sandbox ein Raster aus PBR-Kugeln auf einem Boden mit Säulen, Sonne mit
Schatten und acht kreisenden Punktlichtern, mit `--model <pfad>` ein glTF-Modell (`.gltf` oder
`.glb`); freie Kamera mit gedrückter rechter Maustaste, WASD, Q/E und Shift; F1 schaltet
Debug-Zeichnen um (von Anfang an mit `--debug-draw`), F2 färbt die Kaskaden der Schatten ein; mit
`--image <pfad>` offscreen als Bild, mit `--far` 10 000 km vom Ursprung. Demos:
`--jobs-demo`, `--scene-demo`, `--script-demo` (C#-Skripte), `--net-demo` (Server und Client in
einem Prozess) und `--voxel-demo` (endloses prozedurales Voxel-Terrain mit Materialien und LOD,
`--seed <n>` wählt den Startwert, `--view <m>` die Sichtweite (Standard 2000), G oder `--flight [m]`
startet einen Autopiloten, der Frame-Zeiten und Speicher misst, `--hover [s]` misst zum Vergleich
ohne Flug; links trägt die Maus ab, rechts auf, die mittlere
Taste glättet, das Mausrad zoomt; F schaltet den Flugmodus um, dort dreht die rechte Taste die Kamera, WASD, Q/E und Shift fliegen und die mittlere
Taste trägt auf; F1 zeigt die Grenzen der Chunks nach Stufe; mit `--headless` ohne GPU samt Messung der Zeit
pro Chunk und Streaming um drei Beobachter, mit `--image <pfad>` offscreen als Bild). Netzwerk: `--server [--port n]`
startet einen dedizierten Server ohne Fenster, `--client <adresse>` verbindet sich mit ihm. Mit `-DNGINE_WITH_WINDOW=OFF` entsteht ein Build ohne SDL3, mit `-DNGINE_WITH_DOTNET=OFF` einer ohne
C#-Assemblies.

Der Editor (ab R2.1, braucht ein Fenster) zeigt die Standardszene im Viewport-Fenster, dazu den
Szenenbaum, den Inspector, eine Konsole mit dem Log und eine Statistik; alle Fenster lassen sich
andocken und lösen, „Ansicht → Layout zurücksetzen“ stellt die Anordnung wieder her. Kamera wie in
der Sandbox mit gedrückter rechter Maustaste im Viewport, Linksklick ins Bild wählt aus. Im Baum
benennt F2 um, Ziehen hängt um, Entf löscht; Strg+Z und Strg+Y machen rückgängig und wiederholen,
Strg+N, Strg+O und Strg+S legen an, öffnen und speichern. Seit R2.3 hat die Auswahl einen Gizmo:
W verschiebt, E dreht, R skaliert (Werkzeugleiste über dem Viewport, dort auch lokale Achsen und
Raster; Strg beim Ziehen kehrt das Raster um), jedes Ziehen ist ein Undo-Schritt. F5 spielt (die
Lichter der Standardszene kreisen) und stoppt; danach steht alles wie vorher. Seit R2.4 listet
das Fenster „Assets“ alles unter `assets/`; Modelle zieht man von dort ins Viewport. Gespeicherte
Shader unter `engine/shaders/`, Texturen und Modelle lädt der Editor ohne Neustart neu. Optionen:
`--scene <pfad>` (Szene öffnen), `--save <pfad>` (beim Beenden speichern), `--model <pfad>`,
`--assets <pfad>` und `--shaders <pfad>` (Asset-Ordner und Shader-Quellen, sonst vom
Arbeitsverzeichnis aufwärts gesucht), `--insert <pfad>` (Modell aus dem Asset-Ordner einfügen),
`--frames <n>`, `--play <n>` (gleich spielen und nach n Frames stoppen), `--no-vsync`,
`--baseline`, `--no-ini` (Lage der Fenster weder laden noch speichern). Log und Lage der Fenster
liegen im Benutzerordner unter `NGine/editor.log` und `NGine/editor.ini`.

## Dokumentation

Alles zu Vision, Prozess, Roadmap, Features und Architektur steht in [`docs/`](docs/README.md).
