// Eingabe als Aktionen (PLAT-02, R4.1): Tasten, Maus und Gamepad werden auf benannte Aktionen und
// Achsen abgebildet (move, look, jump). Die Belegung kommt aus einer JSON-Datei und lässt sich im
// laufenden Betrieb neu laden. Das Modul braucht kein Fenster: Es liest nur die Ereignistypen von
// ngine.platform, sodass Tests und später der Server Ereignisse direkt einspeisen.
export module ngine.input;

export import :actions;
