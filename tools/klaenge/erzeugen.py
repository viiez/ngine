#!/usr/bin/env python3
"""Erzeugt die Klänge der Demo (R4.5) unter assets/klaenge.

Alle Klänge entstehen hier aus Rauschen und Sinustönen; sie sind damit eigene Werke unter der
Lizenz des Projekts (Abstimmung zu R4.5). Echte Aufnahmen (CC0) können sie jederzeit ersetzen:
Datei austauschen, .meta behalten, dann bleiben Szenen, Klangsätze und Materialtabelle gültig.

Aufruf aus der Wurzel des Repositorys:  python3 tools/klaenge/erzeugen.py
Lange Klänge (Radio, Wind) werden mit oggenc (Paket vorbis-tools) zu Ogg Vorbis; ohne oggenc
bleiben sie WAV. Der Zufall hat einen festen Startwert, das Ergebnis ist also reproduzierbar.
Bestehende .meta-Dateien bleiben, neue bekommen eine zufällige Kennung.
"""

import json
import math
import os
import random
import secrets
import shutil
import struct
import subprocess
import sys
import wave

RATE = 22050
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "klaenge")


def meta(path):
    """Kennung aus der .meta-Datei, sonst neue .meta mit zufälliger Kennung."""
    meta_path = path + ".meta"
    if os.path.exists(meta_path):
        with open(meta_path, encoding="utf-8") as f:
            return json.load(f)["id"]
    asset_id = secrets.randbits(63) or 1
    with open(meta_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps({"format": "ngine.meta", "version": 1, "id": asset_id}, indent=2) + "\n")
    return asset_id


def write_wav(path, samples, rate=RATE):
    peak = max(1e-9, max(abs(s) for s in samples))
    scale = min(1.0, 0.89 / peak)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * scale)) * 32767))
                               for s in samples))


def write_long(path_without_extension, samples):
    """Ogg Vorbis mit oggenc, sonst WAV. Liefert den Pfad."""
    wav = path_without_extension + ".wav"
    write_wav(wav, samples)
    if shutil.which("oggenc") is None:
        return wav
    ogg = path_without_extension + ".ogg"
    subprocess.run(["oggenc", "-Q", "-q", "3", "-o", ogg, wav], check=True)
    os.remove(wav)
    return ogg


class Filter:
    """Einpoliger Tief- bzw. Hochpass."""

    def __init__(self, cutoff, high=False):
        self.a = math.exp(-2.0 * math.pi * cutoff / RATE)
        self.y = 0.0
        self.x = 0.0
        self.high = high

    def __call__(self, x):
        self.y = (1.0 - self.a) * x + self.a * self.y
        return x - self.y if self.high else self.y


def envelope(t, attack, decay):
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / decay)


def footstep(rng, material):
    """Ein Schritt: gefiltertes Rauschen mit Hüllkurve, je Material anders."""
    length = {"gras": 0.22, "fels": 0.12, "erde": 0.16, "sand": 0.24, "holz": 0.18}[material]
    n = int(length * RATE)
    out = []
    if material == "gras":
        lp, hp = Filter(rng.uniform(3500, 5000)), Filter(400, high=True)
        for i in range(n):
            t = i / RATE
            rustle = 0.6 + 0.4 * math.sin(2 * math.pi * rng.uniform(25, 35) * t)
            out.append(hp(lp(rng.uniform(-1, 1))) * envelope(t, 0.02, 0.06) * rustle)
    elif material == "fels":
        lp = Filter(rng.uniform(2500, 3500))
        f = rng.uniform(900, 1300)
        for i in range(n):
            t = i / RATE
            click = math.sin(2 * math.pi * f * t) * envelope(t, 0.001, 0.012)
            grit = lp(rng.uniform(-1, 1)) * envelope(t, 0.002, 0.03)
            out.append(0.6 * click + 0.8 * grit)
    elif material == "erde":
        lp = Filter(rng.uniform(500, 800))
        f = rng.uniform(70, 95)
        for i in range(n):
            t = i / RATE
            thud = math.sin(2 * math.pi * f * t) * envelope(t, 0.004, 0.04)
            out.append(0.9 * thud + 1.5 * lp(rng.uniform(-1, 1)) * envelope(t, 0.005, 0.05))
    elif material == "sand":
        lp, hp = Filter(rng.uniform(5000, 7000)), Filter(1200, high=True)
        for i in range(n):
            t = i / RATE
            grain = rng.uniform(-1, 1) if rng.random() < 0.35 else 0.0
            out.append(hp(lp(grain)) * envelope(t, 0.03, 0.07) * 1.6)
    else:  # holz: Kisten und Ebene
        f1, f2 = rng.uniform(180, 220), rng.uniform(410, 470)
        lp = Filter(2000)
        for i in range(n):
            t = i / RATE
            body = math.sin(2 * math.pi * f1 * t) + 0.5 * math.sin(2 * math.pi * f2 * t)
            out.append(0.7 * body * envelope(t, 0.002, 0.035) +
                       0.5 * lp(rng.uniform(-1, 1)) * envelope(t, 0.001, 0.01))
    return out


def radio(rng):
    """Acht Sekunden Schleife: kleine Melodie mit Rauschen, wie aus einem alten Radio."""
    seconds = 8.0
    n = int(seconds * RATE)
    notes = [0, 3, 7, 10, 7, 3, 5, 8, 12, 8, 5, 3, 0, -2, 0, 3]
    step = seconds / len(notes)
    band_lo, band_hi = Filter(300, high=True), Filter(3000)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / RATE
        k = int(t / step)
        local = t - k * step
        f = 220.0 * 2 ** (notes[k] / 12.0)
        phase += 2 * math.pi * f / RATE
        tone = (math.sin(phase) + 0.3 * math.sin(2 * phase) + 0.15 * math.sin(3 * phase))
        tone *= envelope(local, 0.01, 0.25)
        hiss = 0.08 * rng.uniform(-1, 1)
        hum = 0.05 * math.sin(2 * math.pi * 50 * t)
        out.append(band_hi(band_lo(0.6 * tone + hiss + hum)))
    # Weich verbinden: Ende in den Anfang blenden, damit die Schleife nicht knackt.
    return crossfade_loop(out, int(0.05 * RATE))


def wind(rng):
    """Zwanzig Sekunden Wind: Rauschen mit langsam wanderndem Tiefpass."""
    seconds = 20.0
    n = int(seconds * RATE)
    lp, lp2 = Filter(400), Filter(400)
    out = []
    for i in range(n):
        t = i / RATE
        gust = 0.55 + 0.45 * math.sin(2 * math.pi * t / seconds * 3) * math.sin(
            2 * math.pi * t / seconds * 2 + 1.0)
        cutoff = 250 + 700 * gust
        lp.a = math.exp(-2.0 * math.pi * cutoff / RATE)
        lp2.a = lp.a
        out.append(lp2(lp(rng.uniform(-1, 1))) * (0.4 + 0.6 * gust) * 3.0)
    return crossfade_loop(out, int(0.5 * RATE))


def hum(rng):
    """Zwei Sekunden Summen für die bewegte Quelle (Doppler hört man an der Tonhöhe)."""
    n = int(2.0 * RATE)
    f = 165.0  # passt genau 330 Perioden in zwei Sekunden: nahtlose Schleife
    out = []
    for i in range(n):
        t = i / RATE
        p = 2 * math.pi * f * t
        out.append(0.6 * math.sin(p) + 0.3 * math.sin(2 * p) + 0.15 * math.sin(3 * p) +
                   0.05 * math.sin(5 * p))
    return out


def crossfade_loop(samples, overlap):
    body = samples[:len(samples) - overlap]
    tail = samples[len(samples) - overlap:]
    for i in range(overlap):
        w = i / overlap
        body[i] = body[i] * w + tail[i] * (1.0 - w)
    return body


def main():
    rng = random.Random(4505)
    os.makedirs(os.path.join(ROOT, "schritte"), exist_ok=True)
    os.makedirs(os.path.join(ROOT, "welt"), exist_ok=True)

    sets = {}
    for material in ["gras", "fels", "erde", "sand", "holz"]:
        ids = []
        for variant in range(1, 5):
            path = os.path.join(ROOT, "schritte", f"{material}_{variant}.wav")
            write_wav(path, footstep(rng, material))
            ids.append(meta(path))
        set_path = os.path.join(ROOT, "schritte", f"{material}.soundset")
        with open(set_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps({"format": "ngine.soundset", "version": 1, "sounds": ids,
                                "volume_variation": 0.25, "pitch_variation": 0.08},
                               indent=2) + "\n")
        sets[material] = meta(set_path)

    for name, make, long in [("radio", radio, True), ("wind", wind, True), ("summen", hum, False)]:
        base = os.path.join(ROOT, "welt", name)
        samples = make(rng)
        path = write_long(base, samples) if long else (write_wav(base + ".wav", samples) or
                                                      base + ".wav")
        print(f"{os.path.relpath(path, ROOT)}: {meta(path)}")

    for material, asset_id in sets.items():
        print(f"schritte/{material}.soundset: {asset_id}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
