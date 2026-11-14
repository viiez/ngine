// Physik (PHYS-01, PHYS-02, PHYS-04; R4.2): starre Körper, Kollision und Abfragen als
// Komponenten der Welt, gerechnet in der festen Simulation. Dahinter steckt Jolt mit Positionen in
// doppelter Genauigkeit (ADR 0045); außerhalb der Adapter-Dateien jolt_*.cpp kennt niemand die
// Bibliothek. Läuft ohne Fenster und GPU.
export module ngine.physics;

export import :components;
export import :physics_world;
