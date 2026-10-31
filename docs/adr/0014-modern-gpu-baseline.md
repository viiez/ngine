# 0014: Moderne GPU als Mindestanforderung

- **Status:** angenommen
- **Datum:** 2026-10-31

## Kontext

[ADR 0004](0004-vulkan-rhi-feature-tiers.md) hat die integrierte Intel-Grafik des Notebooks zur
Messlatte gemacht: Vulkan 1.3, Mesh Shader und Raytracing nur als optionale Feature-Stufen. Damit
hätte jeder moderne Renderpfad einen Rückfallpfad gebraucht, und der Renderer wäre auf die schwächste
Hardware zugeschnitten.

Das Ziel ist aber eine seriöse und performante Engine. Eine moderne GPU ist Voraussetzung, unter
Linux genauso wie unter Windows. Das Notebook muss nicht alle Features unterstützen; Tests auf echter
Hardware laufen auf dem PC mit der RTX 3060 Ti.

## Entscheidung

Jede GPU, auf der NGine läuft, muss mitbringen:

- **Vulkan 1.4**
- **Bindless:** Descriptor Indexing mit Runtime Arrays, Partially Bound, Variable Count, Non-Uniform
  Indexing und Update After Bind
- **Buffer Device Address**, Timeline Semaphores, Scalar Block Layout
- **Dynamic Rendering**, Synchronization2, Maintenance4
- **Mesh Shader:** `VK_EXT_mesh_shader` mit Task und Mesh Shader
- **Raytracing:** `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` und
  `VK_KHR_ray_tracing_pipeline`

Das entspricht NVIDIA ab RTX 20, AMD ab RX 6000 und Intel Arc. Das Vulkan-Backend prüft die Liste
beim Start, schaltet alles ein und protokolliert für jede abgelehnte GPU, was ihr fehlt.

Die Geometrie läuft von Anfang an GPU-getrieben über Mesh Shader; einen klassischen Vertex-Pfad als
Ersatz gibt es nicht. Raytracing ist immer verfügbar. Ob ein Effekt es nutzt (Schatten, AO, GI),
entscheidet eine Qualitätseinstellung, nicht die Hardware.

## Konsequenzen

- Ein einziger Renderpfad: weniger Code, weniger Testaufwand, mehr Spielraum für Leistung.
- Das Notebook mit Intel UHD erfüllt die Anforderung nicht. Dort läuft die Engine nur über lavapipe
  (Software-Rendering auf der CPU, langsam), was für Funktionstests reicht.
- Linux bleibt gleichwertig, braucht für echte Tests aber ebenfalls eine moderne GPU, z. B. Linux auf
  dem PC mit der RTX 3060 Ti.
- Die CI läuft weiter mit lavapipe; Mesa bietet dort Vulkan 1.4, Mesh Shader und Raytracing.
- Spieler mit älteren GPUs (GTX 10/16, RX 5000 und älter) sind ausgeschlossen. Für das Ziel und den
  Zeitrahmen des Projekts nehmen wir das in Kauf.
- Ein späteres D3D12-Backend setzt entsprechend DirectX 12 Ultimate voraus.

## Erwogene Alternativen

- **Vulkan 1.3 mit Feature-Stufen (bisher, ADR 0004):** breiteste Hardware, aber jeder moderne Pfad
  braucht einen Ersatz, und der Renderer orientiert sich an der schwächsten GPU.
- **Vulkan 1.4, Mesh Shader und Raytracing optional:** schließt zusätzlich GTX 16 ein, erfordert aber
  für jeden Raytracing-Effekt einen Raster-Ersatz.
- **Nur Vulkan 1.4:** zwei Geometriepfade (Vertex und Mesh Shader) dauerhaft pflegen.
