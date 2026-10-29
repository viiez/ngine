# A2: Fenster und Vulkan-Dreieck

- **Phase:** Ausarbeitung
- **Status:** geplant
- **Pull Requests:** –

## Plan

**Ziel:** Die Sandbox öffnet ein Fenster und zeichnet mit Vulkan 1.3 ein Dreieck, auf der Intel-Grafik
unter Linux und auf der RTX unter Windows.

**Features:** PLAT-01, PLAT-02 (Grundlage), PLAT-03, REND-01, REND-02, REND-03, REND-04, CORE-04,
CORE-10

**Risiken:**

- Vulkan-Fehler, die nur auf echter Hardware auftreten; in der CI gibt es keine GPU.
- Die RHI-Schnittstelle wird zu früh zu groß oder zu sehr an Vulkan gebunden.
- Slang in vcpkg und unter beiden Plattformen einzubinden, kostet mehr Zeit als gedacht.

**Abnahme:**

- Dreieck sichtbar unter Linux (Intel) und Windows (RTX), Fenster lässt sich verkleinern und schließen.
- Validation Layers melden keine Fehler.
- `--headless` startet weiterhin ohne GPU; CI bleibt grün.

## Review

Folgt nach Abschluss.
