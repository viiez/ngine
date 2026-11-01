module;

#include <string>

export module ngine.world:components;

import ngine.reflect;

export namespace ngine::world {

// Anzeigename einer Entity. Eine eigene Komponente statt flecs-Namen, weil flecs Namen unter einem
// Elternteil eindeutig hält und Spielobjekte oft gleich heißen („Baum“).
struct Name {
    std::string value;
};

// Registriert die Komponenten der Welt in der Reflection. Muss vor dem ersten World-Objekt laufen;
// mehrfaches Aufrufen schadet nicht.
void register_types(reflect::TypeRegistry& types);

} // namespace ngine::world
