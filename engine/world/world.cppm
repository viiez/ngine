// Spielwelt (WORLD-01 bis WORLD-06): Entities mit Komponenten in einem ECS, Hierarchie mit
// Transforms in 64 Bit, Systeme auf dem Job-System und Szenen als Datei. Seit R3.1 auch
// Voxel-Terrain als Komponente (:terrain, Chunks und Netze aus ngine.voxel). Seit R4.1 Phasen,
// Befehlspuffer und die Spielschleife mit festem Zeitschritt (:commands, :simulation). Dahinter
// steckt flecs (ADR 0003); außerhalb von world.cpp kennt niemand die Bibliothek.
export module ngine.world;

// Transforms bestehen aus Typen von ngine.math; wer die Welt nutzt, rechnet mit dessen Funktionen.
// Prefabs halten ihre Komponenten als JSON (ngine.serialize).
export import ngine.math;
export import ngine.serialize;

export import :ecs;
export import :components;
export import :transform;
export import :commands;
export import :schedule;
export import :simulation;
export import :prefab;
export import :scene;
export import :terrain;
