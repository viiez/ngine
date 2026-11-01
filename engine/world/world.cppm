// Spielwelt (WORLD-01 bis WORLD-05): Entities mit Komponenten in einem ECS, Hierarchie, Systeme auf
// dem Job-System und Szenen als Datei. Dahinter steckt flecs (ADR 0003); außerhalb von world.cpp
// kennt niemand die Bibliothek.
export module ngine.world;

export import :ecs;
export import :components;
export import :schedule;
export import :scene;
