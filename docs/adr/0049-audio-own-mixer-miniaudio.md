# 0049: Audio mit eigenem Mixer über miniaudio

- **Status:** angenommen
- **Datum:** 2026-11-16

## Kontext

R4.5 bringt Audio (AUD-01, AUD-02): Klänge als Assets, geortete Quellen in der Welt, Schritte der
Spielfigur nach dem Material des Bodens. Die Engine muss ohne Gerät laufen (`--headless`, der
dedizierte Server, die CI ohne Soundkarte), Linux (PipeWire, PulseAudio, ALSA) und Windows
(WASAPI) bedienen und darf beim Streamen des Terrains nicht knacken. Alles, was hörbar ist, soll
ohne Gerät testbar sein, auch unter ThreadSanitizer. Es ist ein Lernprojekt: Wie ein Mixer
arbeitet, soll im eigenen Code sichtbar sein.

## Entscheidung

- **miniaudio nur für Gerät und Dekodieren.** `ma_device` liefert Blöcke an einen Rückruf,
  `ma_decoder` dekodiert WAV, FLAC und MP3, `stb_vorbis` (aus dem `stb`-Paket) Ogg Vorbis. Die
  Bibliothek steckt in zwei `.cpp`-Dateien des Moduls (`miniaudio_impl.cpp` mit der Umsetzung,
  `miniaudio.cpp` als Adapter); außerhalb kennt niemand sie. Die Engine von miniaudio
  (`ma_engine`, Knotengraph, Ressourcen-Manager) ist abgeschaltet.
- **Eigener Mixer im Thread des Geräts.** Bis zu 256 Stimmen, davon die 64 lautesten hörbar, der
  Rest läuft virtuell weiter; lineares Resampling (Rate der Datei, Tonhöhe, Doppler), feste Busse
  `Master`, `Music`, `Effects`, `Ambient`, `Interface`, Rampen über jeden Block für jede Änderung
  der Lautstärke, Ein- und Ausblenden über mindestens 4 ms, weicher Begrenzer am Ende.
- **Keine Sperren im Rückruf.** Der Hauptthread schickt Befehle über eine Warteschlange mit einem
  Schreiber und einem Leser (`SpscQueue`), der Mixer meldet beendete Stimmen ebenso zurück. Klänge
  ab fünf Sekunden spielen als Strom: Ein eigener Thread dekodiert voraus in einen Ringpuffer pro
  Stimme (`SampleRing`), der Mixer liest nur daraus; läuft er leer, ist die Stimme still und ein
  Zähler meldet es.
- **Räumlichkeit im Hauptthread.** Das `AudioSystem` rechnet pro Bild Abschwächung (`Inverse`,
  `Linear`, `Exponential` zwischen `min_distance` und `max_distance`, ab `max_distance` still;
  Inverse und Exponential blenden im letzten Fünftel weich aus), Panning mit gleicher
  Leistung und Doppler aus den Bewegungen von Quelle und Hörer relativ zum Hörer in `f64` und
  schickt dem Mixer nur Gains links und rechts und die Tonhöhe. Es liest die Welt nur und ändert
  die Prüfsumme der Simulation nicht.
- **Null-Gerät.** Im Echtzeit-Takt in einem eigenen Thread (`--headless`, `--no-audio`, Rückfall,
  wenn kein Gerät aufgeht) oder nur auf Aufruf (`DeviceMode::Manual`) für Tests, die blockweise
  mischen. `NGINE_WITH_AUDIO=OFF` baut das Modul ohne miniaudio, nur mit Null-Gerät; Komponenten
  und Assets bleiben bekannt, Szenen verlieren nichts.

## Konsequenzen

- Mixer, Ströme, Räumlichkeit und Busse sind ohne Gerät testbar (Tag `[audio]`, auch im Preset
  mit ThreadSanitizer), die CI braucht keine Soundkarte.
- Wir pflegen den Mixer selbst: Effekte wie Hall, Filter für Verdeckung oder HRTF fehlen und
  kämen als eigene Stufen dazu (AUD-03, AUD-04).
- Panning ist Stereo; Surround bräuchte eine andere Verteilung im Mixer.
- Das Gerät bestimmt die Abtastrate nicht: Der Mixer rechnet mit der Rate, die er beim Öffnen
  bekommt (Standard 48 kHz), miniaudio wandelt bei Bedarf.
- Klänge werden nicht gekocht; für ein Paket ohne Quellen (TD-35) braucht es später einen Weg.

## Erwogene Alternativen

- **Engine von miniaudio (`ma_engine`):** bringt Mixer, 3D und Ströme mit und wäre schneller
  fertig, versteckt aber genau das, was das Projekt lernen will, und ihr Ressourcen-Manager
  verdoppelt den der Engine.
- **OpenAL Soft:** bewährt, mit HRTF, aber LGPL und ein eigenes Modell aus Quellen und Puffern mit
  Kontext; schwerer hinter eine schmale Schnittstelle zu stellen und ohne Gerät schlecht testbar.
- **FMOD oder Wwise:** Werkzeuge für Klanggestalter, aber proprietär mit eigener Lizenz und nicht
  über vcpkg; passt nicht zu einem quelloffenen Lernprojekt.
- **SDL3-Audio als Gerät:** gäbe es ohne neue Bibliothek, aber nur im Build mit Fenster; der
  dedizierte Server und `NGINE_WITH_WINDOW=OFF` hätten dann gar kein Gerät, und Dekodieren fehlt.
