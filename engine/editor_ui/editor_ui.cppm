// Editor-UI (EDIT-01, ADR 0029): Dear ImGui mit Docking hinter einer Engine-Schnittstelle. Nur die
// Implementierung (context.cpp, widgets.cpp) kennt ImGui; gezeichnet wird über die RHI.
//   :draw     Zeichenliste in Engine-Typen und TextureHost, ohne GPU prüfbar
//   :context  Kontext der UI: Eingaben, Frame, Zeichenliste
//   :widgets  schmale Schicht im Stil von ImGui (Fenster, Menüs, Text, Andocken …)
//   :pass     Pass im Render-Graph, der die Zeichenliste mit der RHI zeichnet
//   :console  Log-Konsole über einem log::RingSink
//   :viewport Größe des Viewport-Bildes, die dem Fenster folgt
export module ngine.editor_ui;

export import :draw;
export import :context;
export import :widgets;
export import :pass;
export import :console;
export import :viewport;
