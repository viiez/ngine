# Technische Schulden

Bekannte Abkürzungen und Probleme mit Grund und Plan. Regeln und Ablauf stehen im
[Prozess](process.md#technische-schulden). Abgebaute Einträge wandern mit Datum und PR nach unten.

## Offen

| ID | Was | Warum es so ist | Plan | Frist |
| --- | --- | --- | --- | --- |
| TD-02 | Layout-Übergänge mit groben Barrieren (`ALL_COMMANDS`, `MEMORY_WRITE`) | Für ein Dreieck reicht es; feine Barrieren ohne Render-Graph wären Handarbeit an jeder Stelle | Render-Graph (REND-05) berechnet die Barrieren | R1 |
| TD-03 | Swapchain-Neuaufbau und `execute()` warten blockierend auf die GPU (`vkDeviceWaitIdle` bzw. Fence) | Einfach und sicher, solange nur die Sandbox zeichnet | Neuaufbau mit alter Swapchain (`oldSwapchain`) ohne Komplett-Stopp; Uploads über eigene Queue und Timeline Semaphores | R1 |
| TD-06 | Die CI baut die Validation Layers aus den Quellen (vcpkg, rund 25 Minuten), sobald sich ihre Pakete ändern, z. B. bei einer neuen vcpkg-Baseline | Schnellster Weg zu Layers 1.4 in A3; danach liegen sie im Cache | Eigenes CI-Container-Image (Clang, Mesa, Layers) in der GitHub Container Registry prüfen, alternativ das LunarG SDK als vorgebautes Paket im Cache | nächste Baseline-Anhebung |

## Abgebaut

| ID | Was | Wann |
| --- | --- | --- |
| TD-01 | `engine/rhi/vulkan/device.cpp` mit über 1300 Zeilen: aufgeteilt in interne Partitionen (`:common`, `:instance`, `:physical_device`, `:resources`, `:command_list`, `:swapchain`), Instanz und Swapchain als eigene Klassen | 2026-10-31, PR #5 |
| TD-04 | Validation Layers 1.3 aus Ubuntu in der CI: jetzt 1.4 über das vcpkg-Feature `validation` und `VK_ADD_LAYER_PATH` | 2026-10-31, PR #5 |
| TD-05 | `ilammy/msvc-dev-cmd` auf Node.js 20: ersetzt durch einen eigenen Schritt mit `vswhere` und `VsDevCmd.bat` | 2026-10-31, PR #5 |
