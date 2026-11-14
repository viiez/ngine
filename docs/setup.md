# Entwicklungsumgebung einrichten

Schritt für Schritt von einem frischen System bis zur laufenden Sandbox in VS Code, für Ubuntu 24.04
(gilt genauso für Linux Mint 22) und Windows 11. Die Kurzfassung steht in der [README](../README.md).
Unter Linux kommt alles aus den offiziellen Paketquellen von Ubuntu 24.04, der Baseline nach
[ADR 0017](adr/0017-platform-baseline-feature-tiers.md).

Was wir brauchen und woher es kommt:

| Werkzeug | Linux | Windows |
| --- | --- | --- |
| C++-Compiler | Clang 20 (`clang-20`, neuere gehen auch) | MSVC aus Visual Studio (aktuelle Version) |
| CMake 3.28+ und Ninja | `cmake`, `ninja-build` | winget (oder in Visual Studio enthalten) |
| vcpkg | Git-Klon im Home-Verzeichnis | Git-Klon unter `C:\dev\vcpkg` |
| SDL3, volk, Vulkan-Header, VMA, Slang, Tracy, flecs, Catch2 | holt vcpkg beim ersten Konfigurieren | holt vcpkg beim ersten Konfigurieren |
| .NET-SDK 10 (C#-Scripting) | `dotnet-sdk-10.0` | winget (`Microsoft.DotNet.SDK.10`) |
| Vulkan-Treiber | Mesa 24 oder neuer (`mesa-vulkan-drivers`) | Grafiktreiber (NVIDIA, AMD, Intel) |
| Validation Layers | `vulkan-validationlayers` | Vulkan SDK von LunarG |
| Editor | VS Code mit CMake Tools und clangd | VS Code mit CMake Tools und C/C++ |

Slang (`slangc`) muss nicht separat installiert werden; vcpkg bringt es mit.

## Ubuntu 24.04 und Linux Mint 22

Alle Befehle im Terminal. Linux Mint 22 basiert auf Ubuntu 24.04 und nutzt dieselben Pakete.

### 1. Grundpakete und CMake

```sh
sudo apt update
sudo apt install git curl zip unzip tar pkg-config python3 gdb cmake ninja-build
cmake --version      # 3.28 oder neuer
```

`curl`, `zip`, `unzip`, `tar` und `pkg-config` braucht vcpkg, `gdb` den Debugger in VS Code.

### 2. Clang 20

```sh
sudo apt install clang-20 clang-tools-20 clangd-20 libstdc++-14-dev
clang++-20 --version
```

- `clang-tools-20` enthält `clang-scan-deps`, ohne das CMake keine C++-Module bauen kann.
- `clangd-20` ist der Sprachserver für VS Code (Autovervollständigung, Fehler im Editor).
- `libstdc++-14-dev` liefert die Standardbibliothek; die Version 13, die Ubuntu sonst nimmt, kann
  Clang mit `std::expected` nicht übersetzen.
- Das Standard-Clang von Ubuntu 24.04 (`clang`, Version 18) ist zu alt. CMake bricht dann mit einer
  Meldung ab.

### 3. Vulkan-Treiber und Werkzeuge

```sh
sudo apt install mesa-vulkan-drivers vulkan-tools vulkan-validationlayers
vulkaninfo --summary
vkcube
```

`vulkaninfo --summary` listet die GPUs mit ihrer `apiVersion` und der Mesa-Version (`driverInfo`).
NGine braucht Vulkan 1.3 ([ADR 0017](adr/0017-platform-baseline-feature-tiers.md)); das liefert Mesa
24 oder neuer auf Intel-Grafik ab Gen9 (z. B. UHD 620) und auf AMD-GPUs. Mesh Shader und Raytracing
sind optional: Fehlen sie, wie bei der Intel-Grafik des Notebooks, läuft die Engine ohne sie. Im Log
steht beim Start, welche Stufen aktiv sind („Optionale Stufen: …“).

Neben der echten GPU zeigt `vulkaninfo` oft „llvmpipe“, den Software-Renderer aus
`mesa-vulkan-drivers`. Die Engine nimmt ihn nur, wenn keine echte GPU passt. `vkcube` öffnet ein
Fenster mit einem drehenden Würfel. Klappt das, funktioniert Vulkan.

### 3b. .NET-SDK für C#-Scripting

```sh
sudo apt install dotnet-sdk-10.0
dotnet --version      # 10.0.x
```

Das SDK baut die C#-Assemblies (`NGine.Scripting`, Skripte); die Engine startet zur Laufzeit die
.NET-Laufzeit aus `/usr/lib/dotnet`. Ohne SDK baut die Engine trotzdem, nur ohne C#.

### 4. Bibliotheken für SDL3

vcpkg baut SDL3 selbst und braucht dafür die Entwicklerpakete von X11 und Wayland:

```sh
sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
  libxss-dev libxft-dev libxkbcommon-dev libwayland-dev libegl1-mesa-dev
```

### 5. vcpkg

```sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.profile
```

Danach **einmal ab- und wieder anmelden**, damit `VCPKG_ROOT` überall gesetzt ist, auch in VS Code,
wenn es aus dem Startmenü kommt. Prüfen mit `echo $VCPKG_ROOT`.

### 6. Repository und Pull Request holen

```sh
mkdir -p ~/dev && cd ~/dev
git clone https://github.com/viiez/ngine.git
cd ngine
git switch claude/a2-window-triangle     # Branch von PR #3
```

Mit der GitHub-CLI geht es auch so: `gh pr checkout 3`. Ein später aktualisierter PR kommt mit
`git pull` nach.

### 7. Einmal im Terminal bauen

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

Neues Terminal öffnen und prüfen: `cmake --version` (3.28 oder neuer), `ninja --version`.

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

### 4b. .NET-SDK für C#-Scripting

```powershell
winget install --id Microsoft.DotNet.SDK.10 -e
```

Danach ein neues Terminal öffnen und mit `dotnet --version` prüfen (10.0.x).

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
  "clangd.path": "clangd-20",
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

## A3 testen: Job-System und Tracy

1. **Tests:** `ctest` meldet alle Tests als bestanden, auch `[jobs]` und `[memory]`.
2. **Job-Demo:** `ngine_sandbox --jobs-demo` rechnet 120 Frames erst mit einem Thread, dann mit allen
   Kernen, und meldet die Zeit pro Frame und die Beschleunigung. Mit `--frames <n>` läuft sie länger.
   Im Release-Build sind die Zahlen aussagekräftiger als im Debug-Build.
3. **Tracy:** Den Profiler in derselben Version wie der Client (0.14.1) von den
   [Tracy-Releases](https://github.com/wolfpld/tracy/releases) holen und starten. Die Datei ist
   nicht signiert, deshalb blockiert Windows sie oft: im SmartScreen-Dialog „Weitere Informationen“
   und „Trotzdem ausführen“ wählen oder den Profiler mit `vcpkg install tracy[gui-tools]` selbst
   bauen. Dann `ngine_sandbox --jobs-demo --frames 300` starten und im Profiler „Connect“ wählen.
   Die Demo rechnet zuerst nur auf dem Hauptthread; die Worker erscheinen erst in der zweiten Hälfte
   des Laufs. Zu sehen sind
   „Hauptthread“ und „Worker 1“ bis „Worker n“ mit den Zonen „Job“, „Dichte erzeugen“ und
   „Oberfläche suchen“, die Frames und der Plot „Frame-Speicher (Bytes)“. Auch die Sandbox mit
   Fenster setzt Frame-Marken.
4. **Ohne Tracy bauen:** `-DNGINE_WITH_TRACY=OFF` beim Konfigurieren; danach läuft alles wie vorher.

## A4 testen: Welt, Szenen und Speicher in Tracy

1. **Tests:** `ctest` meldet alle Tests als bestanden, auch `[reflect]`, `[serialize]`,
   `[filesystem]` und `[world]`.
2. **Szenen-Demo:** `ngine_sandbox --scene-demo` baut ein Sonnensystem mit gut 2000 Entities
   (Planeten, Monde, Asteroiden als Kinder), lässt zwei Systeme 120 Frames lang parallel laufen,
   speichert die Szene und lädt sie in eine neue Welt. Am Ende steht „Szene geladen: … gleich dem
   Original“. Die Datei liegt unter `~/.local/share/NGine/sandbox.scene.json` bzw.
   `%APPDATA%\NGine\sandbox.scene.json`; mit `--scene <pfad>` an einer anderen Stelle. Ein Blick
   hinein zeigt das Format.
3. **Tracy:** `ngine_sandbox --scene-demo --frames 2000` und im Profiler verbinden. Zu sehen sind
   die Zonen „Schedule“ und „System“ auf mehreren Threads und die Plots „Speicher Welt (Bytes)“ usw.
   für jede Kategorie, auch bei `--jobs-demo` und der Sandbox mit Fenster.

## D1 testen: Voxel-Terrain

1. **Tests:** `ctest` meldet alle Tests als bestanden, auch `[voxel]` und den neuen GPU-Test
   „Vulkan zeichnet indiziert mit Tiefe, Puffern und Push Constants“.
2. **Voxel-Demo:** `ngine_sandbox --voxel-demo` zeigt einen grünen Hügel auf einer Ebene, um den
   die Kamera kreist. Linke Maustaste gedrückt halten trägt an der Mausposition ab (darunter
   kommt braune Erde zum Vorschein), rechte trägt grauen Fels auf, die mittlere glättet. Das
   Mausrad zoomt, die Leertaste hält die Kamera an. Alle drei Sekunden steht im Log, wie viele
   Dreiecke das Netz hat und wie lange das Meshing dauert.
3. **Ohne optionale Stufen:** `ngine_sandbox --voxel-demo --baseline` sieht genauso aus.
4. **Ohne GPU und offscreen:** `ngine_sandbox --voxel-demo --headless` misst das Meshing für 32³,
   64³ und 128³ Voxel; `ngine_sandbox --voxel-demo --image hügel.ppm` speichert ein Bild nach 60
   automatischen Änderungen.
5. **Tracy (optional):** Die Zonen „VoxelScene::remesh“, „voxel::Mesher::build“ und
   „VoxelScene::upload“ zeigen, wie viel Zeit eine Änderung kostet.

## D2 testen: C#-Skripte

1. **Tests:** `ctest` meldet auch die `[script]`-Tests als bestanden. Fehlt .NET, werden sie
   übersprungen; dann `dotnet --version` prüfen.
2. **Skript-Demo:** `ngine_sandbox --script-demo` startet .NET (Pfad zu hostfxr steht im Log), lädt
   `Sandbox.Scripts.dll` und zeigt „Hallo aus C#: Orbit startet“ in derselben Konsole wie die
   C++-Ausgaben. Danach entlädt sie die Skripte, lädt sie neu und meldet die Kosten eines Aufrufs
   in beide Richtungen.
3. **Eigenes Skript (optional):** In `apps/sandbox/scripts/Orbit.cs` die Logzeile in `Start()`
   ändern, neu bauen und die Demo starten.

## D3 testen: Server und Client

1. **Tests:** `ctest` meldet die `[net]`- und `[replication]`-Tests als bestanden. Sie nutzen nur
   localhost.
2. **Demo in einem Prozess:** `ngine_sandbox --net-demo` startet Server und Client, lässt sie drei
   Sekunden laufen und meldet am Ende „Server und Client stimmen überein“, dazu die Größe eines
   Schnappschusses.
3. **Zwei Prozesse:** In einem Terminal `ngine_sandbox --server`, in einem zweiten
   `ngine_sandbox --client 127.0.0.1`. Der Client meldet jede Sekunde, wie viele Entities er
   spiegelt; der Server alle fünf Sekunden Ping und Datenrate. Server mit Strg+C beenden.
4. **Zwei Rechner (optional):** Auf dem einen `--server`, auf dem anderen
   `--client <IP des Servers>`. Die Firewall muss UDP-Port 27020 durchlassen (Linux z. B.
   `sudo ufw allow 27020/udp`; Windows fragt beim ersten Start).

## R4.1 testen: Spielschleife und Eingabe

1. **Tests:** `ctest` meldet auch `[input]`, `[commands]` und `[simulation]` als bestanden.
2. **Ohne Fenster:** `ngine_sandbox --headless --frames 600` rechnet die Welt der Szenen-Demo über
   die Spielschleife und meldet am Ende „600 feste Schritte (10.00 s simuliert)“.
3. **Gleich schnell bei jeder Bildrate:** `ngine_sandbox` einmal mit VSync (60 oder 144 Hz) und einmal
   mit `--no-vsync` starten. Die acht Lichter kreisen gleich schnell und gleiten ohne Ruckeln.
4. **Gamepad:** Ein Gamepad anstecken (Xbox, PlayStation oder ein anderes, das SDL3 kennt); im Log
   steht „Gamepad angesteckt: …“. Linker Stick fliegt, rechter dreht, Schultertasten hoch und
   runter, rechter Trigger schneller. Tastatur und Maus fliegen wie bisher.
5. **Belegung im laufenden Betrieb:** `assets/eingabe/standard.input` ändern, z. B. bei `move` den
   Eintrag `Key.W` durch `Key.Up` ersetzen, und speichern. Im Log steht „Eingabe: Belegung neu
   geladen“, die Pfeiltaste fliegt sofort vorwärts.
6. **Editor:** Spielen (F5), „Pause“ ankreuzen, „Schritt“ drücken: Die Lichter rücken genau einen
   Schritt weiter. Stoppen stellt die Welt wieder her.
7. **Kürzel der Pinsel:** Im Editor ein Terrain einfügen und auswählen, ins Bild klicken. B schaltet
   den Pinsel um, 1 bis 5 wählen den Modus, `[` und `]` (auf der deutschen Tastatur Ü und +)
   ändern den Radius.

## R4.2 testen: Physik mit Jolt

1. **Tests:** `ctest` meldet auch `[physics]` als bestanden, unter ThreadSanitizer ebenso.
2. **Spielplatz:** `ngine_sandbox --playground` zeigt einen Boden mit einer Rampe, vier Türmen aus
   Kisten, einem schiefen Turm, der nach dem Start umfällt, und drei Kugeln. Die linke Maustaste
   (am Gamepad X bzw. Quadrat) wirft eine Kugel in Blickrichtung. F3 zeigt die Kollisionsformen:
   grün wach, blau schlafend, grau unbewegt. Eine kleine rote Kugel mit Normale zeigt, was der
   Strahl aus der Bildmitte trifft.
3. **Weit weg vom Ursprung:** `ngine_sandbox --playground --far` baut denselben Spielplatz 10 000 km
   vom Ursprung entfernt. Die Kisten liegen genauso ruhig wie am Ursprung und zittern nicht.
4. **Ohne Fenster:** `ngine_sandbox --headless --playground` rechnet 300 Schritte und meldet am Ende
   eine „Prüfsumme der Lagen“. Zwei Läufe mit demselben Build ergeben dieselbe Summe.
5. **Messung:** Im Release-Build `ngine_sandbox --headless --playground --bodies 2000 --frames 600`.
   Das Log meldet den Schritt der Physik (Median und 99. Perzentil), Abgleich, Rückschrieb und
   `update_transforms()`. Die Zahlen gehören ins Iterationsdokument.
6. **Editor:** Im Menü „Physik“ die „Kollisionsformen“ einschalten. Eine Entity mit `RigidBody` und
   `Collider` anlegen (oder die Szene des Spielplatzes laden), Spielen (F5): Die Körper fallen.
   Stoppen stellt die Welt bitgenau wieder her; zweimal Spielen ergibt dasselbe Bild.

Aktuelle Validation Layers lassen sich auch über vcpkg bauen, wenn die Pakete des Systems zu alt
sind: beim Konfigurieren `-DVCPKG_MANIFEST_FEATURES=validation` angeben und `VK_ADD_LAYER_PATH` auf
`build/<preset>/vcpkg_installed/<triplet>/share/vulkan/explicit_layer.d` setzen (unter Windows auf
`.../bin`). Die CI macht das unter Linux so.

## Häufige Probleme

| Meldung | Ursache und Lösung |
| --- | --- |
| `Could not find toolchain file: /scripts/buildsystems/vcpkg.cmake` | `VCPKG_ROOT` ist nicht gesetzt. Linux: ab- und anmelden; Windows: neues Terminal oder `cmake.configureEnvironment`. Danach das Build-Verzeichnis löschen. |
| `CMAKE_CXX_COMPILER ... clang++-20 not found` | `clang-20` installieren. Anderes Clang (ab 20) über ein eigenes Preset in `CMakeUserPresets.json`. |
| `NGine braucht Clang 20 oder neuer` | Ein zu altes Clang wurde gefunden, etwa das Standard-Clang 18 von Ubuntu. `clang-20` installieren, Build-Verzeichnis löschen. |
| `clang-scan-deps` nicht gefunden | `clang-tools-23` installieren. |
| Fehler in `<expected>` | `libstdc++-14-dev` fehlt. |
| vcpkg: „builtin-baseline … requires a git clone“ | Das vcpkg von Visual Studio wird benutzt. `VCPKG_ROOT` auf `C:\dev\vcpkg` setzen. |
| `Keine GPU erfüllt die Mindestanforderung` | Treiber zu alt oder GPU ohne Vulkan 1.3. Das Log nennt pro GPU, was fehlt. Treiber aktualisieren (Linux: Mesa 24 oder neuer). |
| `Layer VK_LAYER_KHRONOS_validation uses API version 1.3 which is older …` | Nur eine Warnung: Die Validation Layers von Ubuntu 24.04 kennen Vulkan 1.4 noch nicht. Sie prüfen trotzdem. |
| „Vulkan Validation Layers sind nicht installiert“ | Nur eine Warnung. Linux: `vulkan-validationlayers`; Windows: Vulkan SDK. |
| clangd zeigt Fehler bei `import ngine.…` | Einmal bauen, damit die Moduldateien existieren, dann „clangd: Restart language server“. |
| Nach einem Preset- oder Werkzeugwechsel seltsame Fehler | „CMake: Delete Cache and Reconfigure“ oder `build/<preset>` löschen. |
