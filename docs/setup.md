# Entwicklungsumgebung einrichten

Schritt für Schritt von einem frischen System bis zur laufenden Sandbox in VS Code, für Linux Mint 22
(gilt genauso für Ubuntu 24.04) und Windows 11. Die Kurzfassung steht in der [README](../README.md).

Was wir brauchen und woher es kommt:

| Werkzeug | Linux | Windows |
| --- | --- | --- |
| C++-Compiler | Clang 23 aus apt.llvm.org (mindestens 20) | MSVC aus Visual Studio (aktuelle Version) |
| CMake 3.30+ und Ninja | Kitware-Paketquelle, `ninja-build` | winget (oder in Visual Studio enthalten) |
| vcpkg | Git-Klon im Home-Verzeichnis | Git-Klon unter `C:\dev\vcpkg` |
| SDL3, volk, VMA, Slang, Catch2 | holt vcpkg beim ersten Konfigurieren | holt vcpkg beim ersten Konfigurieren |
| Vulkan-Treiber | Mesa (`mesa-vulkan-drivers`) | NVIDIA-Treiber |
| Validation Layers | `vulkan-validationlayers` | Vulkan SDK von LunarG |
| Editor | VS Code mit CMake Tools und clangd | VS Code mit CMake Tools und C/C++ |

Slang (`slangc`) muss nicht separat installiert werden; vcpkg bringt es mit.

## Linux Mint 22

Alle Befehle im Terminal. Linux Mint hat eigene Codenamen (z. B. „wilma“), die Paketquellen von LLVM
und Kitware brauchen aber den Ubuntu-Codenamen. Deshalb steht unten fest `noble` (Ubuntu 24.04, die
Basis von Mint 22).

### 1. Grundpakete

```sh
sudo apt update
sudo apt install git curl wget zip unzip tar pkg-config python3 gpg ninja-build gdb
```

`curl`, `zip`, `unzip`, `tar` und `pkg-config` braucht vcpkg, `gdb` den Debugger in VS Code.

### 2. CMake (aktuelle Version)

Das CMake aus den Mint-Paketquellen (3.28) ist zu alt. Kitware bietet eine eigene Paketquelle:

```sh
wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
  | gpg --dearmor | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg > /dev/null
echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main" \
  | sudo tee /etc/apt/sources.list.d/kitware.list
sudo apt update
sudo apt install cmake
cmake --version      # 3.30 oder neuer
```

Alternative ohne Paketquelle: `sudo apt install pipx && pipx install cmake` (danach `~/.local/bin` im
`PATH`). `pip install --user cmake` lehnt Mint 22 ab („externally-managed-environment“).

### 3. Clang 23

```sh
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-23 main" | sudo tee /etc/apt/sources.list.d/llvm.list
sudo apt update
sudo apt install clang-23 clang-tools-23 clangd-23 libstdc++-14-dev
clang++-23 --version
```

- `clang-tools-23` enthält `clang-scan-deps`, ohne das CMake keine C++-Module bauen kann.
- `clangd-23` ist der Sprachserver für VS Code (Autovervollständigung, Fehler im Editor).
- `libstdc++-14-dev` liefert die Standardbibliothek; die Version 13 aus Mint kann Clang mit
  `std::expected` nicht übersetzen.

### 4. Vulkan-Treiber und Werkzeuge

```sh
sudo apt install mesa-vulkan-drivers vulkan-tools vulkan-validationlayers
vulkaninfo --summary
vkcube
```

`vulkaninfo --summary` sollte die Intel-Grafik mit `apiVersion` 1.3 oder höher zeigen (dazu oft
„llvmpipe“, das ist der Software-Renderer; die Engine wählt die echte GPU). `vkcube` öffnet ein Fenster
mit einem drehenden Würfel. Klappt das, funktioniert Vulkan.

### 5. Bibliotheken für SDL3

vcpkg baut SDL3 selbst und braucht dafür die Entwicklerpakete von X11 und Wayland:

```sh
sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
  libxss-dev libxft-dev libxkbcommon-dev libwayland-dev libegl1-mesa-dev
```

### 6. vcpkg

```sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.profile
```

Danach **einmal ab- und wieder anmelden**, damit `VCPKG_ROOT` überall gesetzt ist, auch in VS Code,
wenn es aus dem Startmenü kommt. Prüfen mit `echo $VCPKG_ROOT`.

### 7. Repository und Pull Request holen

```sh
mkdir -p ~/dev && cd ~/dev
git clone https://github.com/viiez/ngine.git
cd ngine
git switch claude/a2-window-triangle     # Branch von PR #3
```

Mit der GitHub-CLI geht es auch so: `gh pr checkout 3`. Ein später aktualisierter PR kommt mit
`git pull` nach.

### 8. Einmal im Terminal bauen

Bevor VS Code dazukommt, lohnt sich ein Lauf im Terminal: Fehlt etwas, ist die Meldung hier am
klarsten.

```sh
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
./build/linux-clang-debug/apps/sandbox/ngine_sandbox
```

Der erste `cmake --preset`-Aufruf dauert lange (etwa 10 bis 30 Minuten), weil vcpkg alle Bibliotheken
baut. Danach liegen sie im Cache unter `~/.cache/vcpkg` und jeder weitere Lauf ist schnell.

Weiter mit [VS Code](#vs-code-einrichten).

## Windows 11

Die Befehle laufen in PowerShell oder in der Eingabeaufforderung. `winget` ist in Windows 11 enthalten.

### 1. Git

```powershell
winget install --id Git.Git -e
```

### 2. Visual Studio mit MSVC

```powershell
winget install --id Microsoft.VisualStudio.Community -e
```

Danach den **Visual Studio Installer** öffnen, bei Visual Studio auf „Ändern“ klicken und die Workload
**„Desktopentwicklung mit C++“** auswählen. Sie enthält den MSVC-Compiler, das Windows SDK, CMake und
Ninja. Mehr ist nicht nötig. Statt der Community-Edition reichen auch die „Build Tools für Visual
Studio“ mit derselben Workload, wenn die IDE nicht gebraucht wird.

### 3. CMake und Ninja (aktuelle Versionen)

Visual Studio bringt beide mit, aber nur in der Developer-Konsole und nicht immer in der neuesten
Version. Systemweit installieren:

```powershell
winget install --id Kitware.CMake -e
winget install --id Ninja-build.Ninja -e
```

Neues Terminal öffnen und prüfen: `cmake --version` (3.30 oder neuer), `ninja --version`.

### 4. Grafiktreiber und Vulkan SDK

1. Aktuellen NVIDIA-Treiber installieren (GeForce Experience/NVIDIA App oder nvidia.com). Er enthält
   den Vulkan-Treiber.
2. Vulkan SDK für die Validation Layers und Werkzeuge:

   ```powershell
   winget install --id KhronosGroup.VulkanSDK -e
   ```

   Alternativ den Installer von [vulkan.lunarg.com](https://vulkan.lunarg.com/sdk/home) nehmen. Von den
   optionalen Komponenten wird keine gebraucht.
3. Neues Terminal öffnen und prüfen: `vulkaninfo --summary` zeigt die RTX 3060 Ti, `vkcube` einen
   drehenden Würfel.

### 5. vcpkg

Kurzer Pfad, weil vcpkg tief verschachtelte Build-Verzeichnisse anlegt:

```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
setx VCPKG_ROOT C:\dev\vcpkg
```

`setx` wirkt erst in neu geöffneten Terminals und Programmen.

**Achtung:** Visual Studio bringt ein eigenes vcpkg mit und setzt `VCPKG_ROOT` in seiner
Developer-Konsole darauf. Dieses vcpkg kann mit der festen `builtin-baseline` in unserer `vcpkg.json`
nichts anfangen. Wer in einer Developer-Konsole arbeitet, prüft mit `echo %VCPKG_ROOT%` und setzt bei
Bedarf `set VCPKG_ROOT=C:\dev\vcpkg`.

### 6. Repository und Pull Request holen

```powershell
cd C:\dev
git clone https://github.com/viiez/ngine.git
cd ngine
git switch claude/a2-window-triangle
```

### 7. Einmal in der Developer-Konsole bauen

Im Startmenü **„x64 Native Tools Command Prompt for VS“** öffnen (nicht die x86-Variante), dann:

```bat
set VCPKG_ROOT=C:\dev\vcpkg
cd C:\dev\ngine
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
build\windows-msvc-debug\apps\sandbox\ngine_sandbox.exe
```

Auch hier dauert der erste Konfigurationslauf lange, weil vcpkg die Bibliotheken baut.

## VS Code einrichten

### 1. VS Code und Erweiterungen

VS Code installieren (Linux: `.deb` von [code.visualstudio.com](https://code.visualstudio.com) oder
`sudo apt install code` nach Einrichtung der Microsoft-Paketquelle; Windows:
`winget install --id Microsoft.VisualStudioCode -e`).

Dann den Ordner `ngine` öffnen. VS Code schlägt die Erweiterungen aus `.vscode/extensions.json` vor
(„Empfohlene Erweiterungen installieren“). Sonst von Hand in der Erweiterungsansicht suchen:

| Erweiterung | Wofür | Linux | Windows |
| --- | --- | --- | --- |
| CMake Tools (`ms-vscode.cmake-tools`) | Presets wählen, bauen, testen, starten | ja | ja |
| clangd (`llvm-vs-code-extensions.vscode-clangd`) | Sprachserver, versteht C++-Module | ja | nein |
| C/C++ (`ms-vscode.cpptools`) | Debugger; unter Windows auch IntelliSense | ja (nur Debugger) | ja |
| Slang (`shader-slang.slang-language-extension`) | Syntax und Fehler in `.slang`-Shadern | ja | ja |
| Markdown Mermaid (`bierner.markdown-mermaid`) | Diagramme in der Markdown-Vorschau | optional | optional |

### 2. Benutzereinstellungen

`.vscode/settings.json` ist nicht im Repository, weil die Einstellungen vom Rechner abhängen. Mit
„Preferences: Open Workspace Settings (JSON)“ anlegen.

**Linux:**

```json
{
  "cmake.useCMakePresets": "always",
  "cmake.configureOnOpen": true,
  "clangd.path": "clangd-23",
  "clangd.arguments": [
    "--compile-commands-dir=${workspaceFolder}/build/linux-clang-debug",
    "--experimental-modules-support",
    "--header-insertion=never",
    "--background-index"
  ],
  "C_Cpp.intelliSenseEngine": "disabled",
  "editor.formatOnSave": true
}
```

`C_Cpp.intelliSenseEngine` schaltet IntelliSense der C/C++-Erweiterung ab, damit es nicht mit clangd
streitet; ihr Debugger bleibt nutzbar. clangd liest `compile_commands.json` aus dem Build-Verzeichnis,
das CMake dank der Presets automatisch schreibt. Die Modul-Unterstützung von clangd ist noch
experimentell: Nach größeren Änderungen hilft „clangd: Restart language server“.

**Windows:**

```json
{
  "cmake.useCMakePresets": "always",
  "cmake.configureOnOpen": true,
  "cmake.configureEnvironment": { "VCPKG_ROOT": "C:\\dev\\vcpkg" },
  "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools",
  "editor.formatOnSave": true
}
```

CMake Tools richtet die MSVC-Umgebung für die Windows-Presets selbst ein; VS Code muss also nicht aus
der Developer-Konsole gestartet werden. `cmake.configureEnvironment` stellt sicher, dass unser vcpkg
genommen wird und nicht das von Visual Studio. Falls clangd installiert wurde, die Erweiterung für
diesen Workspace deaktivieren.

### 3. Konfigurieren, bauen, testen, starten

CMake Tools zeigt seine Bedienelemente in der Statusleiste und in der CMake-Ansicht links.

1. **Preset wählen:** Befehlspalette (`Strg+Umschalt+P`) → „CMake: Select Configure Preset“ →
   „Linux, Clang, Debug“ bzw. „Windows, MSVC, Debug“. CMake konfiguriert danach automatisch; die
   Ausgabe steht im Panel „Output“ → „CMake/Build“.
2. **Bauen:** `F7` oder „CMake: Build“.
3. **Testen:** „CMake: Run Tests“ oder die Testansicht (Reagenzglas-Symbol) links. Dort erscheinen die
   Catch2-Tests einzeln.
4. **Startziel wählen:** „CMake: Set Launch/Debug Target“ → `ngine_sandbox`.
5. **Starten ohne Debugger:** `Umschalt+F5` (CMake Tools) bzw. „CMake: Run Without Debugging“.
6. **Debuggen:** Run-and-Debug-Ansicht → „Sandbox (Linux, gdb)“ bzw. „Sandbox (Windows, MSVC)“ → `F5`.
   Die Konfigurationen stehen in `.vscode/launch.json`; Programmargumente wie `--no-vsync` kommen dort
   in `args`.

## PR #3 testen: was prüfen?

Ziel von A2 ist ein Fenster mit einem farbigen Dreieck auf beiden Rechnern ohne Validierungsfehler.

1. **Tests:** `ctest` meldet alle Tests als bestanden. Die beiden Vulkan-Tests laufen auf der echten GPU.
2. **Sandbox (Debug):** Ein Fenster „NGine Sandbox“ mit dunkelblauem Hintergrund und einem Dreieck mit
   Farbverlauf (Spitze oben). Im Log stehen GPU-Name, Vulkan-Version und „Vulkan Validation Layers
   aktiv“.
3. **Fenstergröße ändern und maximieren:** Das Dreieck skaliert mit, ohne Flackern oder Fehler im Log.
4. **Minimieren und wiederherstellen:** Kein Absturz, kein Dauerfeuer von Meldungen.
5. **Beenden:** `Esc` oder Fenster schließen. Am Ende stehen „Beendet“ und keine Zeile „Die Validation
   Layers haben … Fehler gemeldet“. Der Exit-Code ist 0 (`echo $?` bzw. `echo %ERRORLEVEL%`).
6. **Ohne VSync:** `ngine_sandbox --no-vsync --frames 2000` läuft deutlich schneller durch.
7. **Ohne Fenster:** `ngine_sandbox --headless` startet und beendet sich sofort.
8. **Release:** Dasselbe mit dem Preset `linux-clang-release` bzw. `windows-msvc-release`.

Bei Problemen hilft die komplette Konsolenausgabe von Konfigurieren, Bauen oder Sandbox am meisten.

## Häufige Probleme

| Meldung | Ursache und Lösung |
| --- | --- |
| `Could not find toolchain file: /scripts/buildsystems/vcpkg.cmake` | `VCPKG_ROOT` ist nicht gesetzt. Linux: ab- und anmelden; Windows: neues Terminal oder `cmake.configureEnvironment`. Danach das Build-Verzeichnis löschen. |
| `CMAKE_CXX_COMPILER ... clang++-23 not found` | Clang 23 fehlt. Alternativ Clang 20 mit einem eigenen Preset in `CMakeUserPresets.json`. |
| `clang-scan-deps` nicht gefunden | `clang-tools-23` installieren. |
| Fehler in `<expected>` | `libstdc++-14-dev` fehlt. |
| vcpkg: „builtin-baseline … requires a git clone“ | Das vcpkg von Visual Studio wird benutzt. `VCPKG_ROOT` auf `C:\dev\vcpkg` setzen. |
| `Kein nutzbares Vulkan-Gerät` | Treiber fehlt oder ist zu alt (Vulkan 1.3 nötig). `vulkaninfo --summary` prüfen. |
| „Vulkan Validation Layers sind nicht installiert“ | Nur eine Warnung. Linux: `vulkan-validationlayers`; Windows: Vulkan SDK. |
| clangd zeigt Fehler bei `import ngine.…` | Einmal bauen, damit die Moduldateien existieren, dann „clangd: Restart language server“. |
| Nach einem Preset- oder Werkzeugwechsel seltsame Fehler | „CMake: Delete Cache and Reconfigure“ oder `build/<preset>` löschen. |
