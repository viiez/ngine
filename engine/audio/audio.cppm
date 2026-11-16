// Audio (AUD-01, AUD-02; R4.5): Klänge als Assets, ein eigener Mixer im Thread des Geräts mit
// Bussen, Strömen für lange Klänge und 3D-Klang mit Abschwächung, Panning und Doppler. Gerät und
// Dekodieren kommen von miniaudio; außer dem Adapter miniaudio.cpp kennt niemand die Bibliothek
// (ADR 0049). Ohne Gerät läuft ein Null-Gerät (--headless, Server, CI).
export module ngine.audio;

export import :types;
export import :queue;
export import :spatial;
export import :decoder;
export import :stream;
export import :mixer;
export import :device;
export import :sound;
export import :engine;
export import :components;
export import :system;
