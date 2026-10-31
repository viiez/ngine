# NGine

Eine moderne 3D-Engine in C++23 mit Vulkan, Voxel-Terrain im Stil von Enshrouded, C#-Scripting,
eigenem Editor und Multiplayer. Lernprojekt und Grundlage für ein eigenes Spiel.

**Stand:** Phase Ausarbeitung. Die Sandbox öffnet ein Fenster und zeichnet mit Vulkan ein Dreieck.
Siehe [Roadmap](docs/roadmap.md).

## Bauen

Voraussetzungen:

- CMake 3.30 oder neuer und Ninja
- [vcpkg](https://github.com/microsoft/vcpkg), Umgebungsvariable `VCPKG_ROOT` gesetzt; vcpkg holt SDL3,
  volk, VMA, Slang und Catch2 selbst
- Linux: Clang 23 (Mindestversion 20) · Windows: Visual Studio (MSVC), aktuelle Version
- Ein Vulkan-1.3-Treiber; für Debug-Builds die Vulkan Validation Layers (optional)

### Linux Mint 22 / Ubuntu 24.04

Wir nutzen aktuelle Werkzeuge. Das mitgelieferte CMake ist zu alt (Kitware-Paketquelle oder
`pipx install cmake`), und Clang 23 kommt aus der LLVM-Paketquelle ([apt.llvm.org](https://apt.llvm.org)).
Die ausführliche Anleitung inklusive VS Code steht in [docs/setup.md](docs/setup.md).

```sh
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-23 main" | sudo tee /etc/apt/sources.list.d/llvm.list
sudo apt update
sudo apt install ninja-build clang-23 clang-tools-23 libstdc++-14-dev \
    mesa-vulkan-drivers vulkan-validationlayers \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
    libxft-dev libxkbcommon-dev libwayland-dev libegl1-mesa-dev
```

Die X11- und Wayland-Pakete braucht vcpkg, um SDL3 zu bauen. Clang 20 aus den normalen Paketquellen
funktioniert ebenfalls; dann im eigenen `CMakeUserPresets.json` `clang++-20` eintragen.

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
`--no-vsync`. Mit `-DNGINE_WITH_WINDOW=OFF` entsteht ein Build ohne SDL3.

## Dokumentation

Alles zu Vision, Prozess, Roadmap, Features und Architektur steht in [`docs/`](docs/README.md).
