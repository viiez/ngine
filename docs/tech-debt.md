# Technische Schulden

Bekannte Abkürzungen und Probleme mit Grund und Plan. Regeln und Ablauf stehen im
[Prozess](process.md#technische-schulden). Abgebaute Einträge wandern mit Datum und PR nach unten.

## Offen

| ID | Was | Warum es so ist | Plan | Frist |
| --- | --- | --- | --- | --- |
| TD-01 | `engine/rhi/vulkan/device.cpp` hat über 1300 Zeilen und mischt Instanz, Geräteauswahl, Swapchain, Ressourcen und Command Lists | In A2 schnell gewachsen | In mehrere Dateien des Moduls aufteilen (z. B. `instance`, `physical_device`, `swapchain`, `resources`, `command_list`) | Beginn von A3 |
| TD-02 | Layout-Übergänge mit groben Barrieren (`ALL_COMMANDS`, `MEMORY_WRITE`) | Für ein Dreieck reicht es; feine Barrieren ohne Render-Graph wären Handarbeit an jeder Stelle | Render-Graph (REND-05) berechnet die Barrieren | R1 |
| TD-03 | Swapchain-Neuaufbau und `execute()` warten blockierend auf die GPU (`vkDeviceWaitIdle` bzw. Fence) | Einfach und sicher, solange nur die Sandbox zeichnet | Neuaufbau mit alter Swapchain (`oldSwapchain`) ohne Komplett-Stopp; Uploads über eigene Queue und Timeline Semaphores | R1 |
| TD-04 | Die CI nutzt die Validation Layers 1.3 aus Ubuntu, die Engine Vulkan 1.4 | Ubuntu 24.04 hat keine neueren Pakete | Aktuelle Layers über den vcpkg-Port `vulkan-validationlayers` oder das LunarG-SDK in der CI | A3 |
| TD-05 | `ilammy/msvc-dev-cmd@v1` läuft noch auf Node.js 20 (Warnung in der CI) | Externe Action | Auf eine Version mit Node.js 24 wechseln oder die MSVC-Umgebung selbst einrichten | A3 |

## Abgebaut

| ID | Was | Wann |
| --- | --- | --- |
| – | – | – |
