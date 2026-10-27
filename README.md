# NGine

Eine moderne 3D-Engine in C++23 mit Vulkan, Voxel-Terrain im Stil von Enshrouded, C#-Scripting,
eigenem Editor und Multiplayer. Lernprojekt und Grundlage für ein eigenes Spiel.

**Stand:** Stufe 0 (Fundament). Bisher gibt es das Build-System und das Core-Modul mit Logging.

## Bauen

Voraussetzungen:

- CMake 3.30 oder neuer und Ninja
- [vcpkg](https://github.com/microsoft/vcpkg), Umgebungsvariable `VCPKG_ROOT` gesetzt
- Linux: Clang 18 oder neuer · Windows: Visual Studio 2022 (MSVC)

Unter Linux Mint 22 bzw. Ubuntu 24.04 ist das mitgelieferte CMake zu alt; ein aktuelles gibt es
z. B. über `pip install cmake` oder die Kitware-Paketquelle.

```sh
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
```

Unter Windows dieselben Befehle mit `windows-msvc-debug` in einer Developer-Konsole.
