# NGine

Eine moderne 3D-Engine in C++23 mit Vulkan, Voxel-Terrain im Stil von Enshrouded, C#-Scripting,
eigenem Editor und Multiplayer. Lernprojekt und Grundlage für ein eigenes Spiel.

**Stand:** Phase Ausarbeitung. Die Sandbox öffnet ein Fenster und zeichnet mit Vulkan ein Dreieck.
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
```

Unter Windows dieselben Befehle mit `windows-msvc-debug` in einer Developer-Konsole. Den Vulkan-Treiber
bringt der Grafiktreiber mit; die Validation Layers kommen mit dem Vulkan SDK.

Optionen der Sandbox: `--headless` (ohne Fenster und GPU), `--frames <n>` (nach n Frames beenden),
`--no-vsync`, `--baseline` (ohne Mesh Shader und Raytracing, auch wenn die GPU sie kann). Demos:
`--jobs-demo`, `--scene-demo`, `--script-demo` (C#-Skripte), `--net-demo` (Server und Client in
einem Prozess) und `--voxel-demo` (Voxel-Terrain, mit der Maus veränderbar; mit
`--headless` ohne GPU, mit `--image <pfad>` offscreen als Bild). Netzwerk: `--server [--port n]`
startet einen dedizierten Server ohne Fenster, `--client <adresse>` verbindet sich mit ihm. Mit `-DNGINE_WITH_WINDOW=OFF` entsteht ein Build ohne SDL3, mit `-DNGINE_WITH_DOTNET=OFF` einer ohne
C#-Assemblies.

## Dokumentation

Alles zu Vision, Prozess, Roadmap, Features und Architektur steht in [`docs/`](docs/README.md).
