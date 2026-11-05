module;

#include <string>
#include <vector>

export module ngine.edit:tree;

import ngine.core;
import ngine.world;
import :document;

export namespace ngine::edit {

// Eine Zeile des Szenenbaums.
struct TreeRow {
    EntityId id = 0;
    // Tiefe in der Hierarchie, 0 für Wurzeln.
    u32 depth = 0;
    bool has_children = false;
    std::string name;
};

// Alle Entities der Welt, Eltern vor ihren Kindern, in derselben Reihenfolge wie in der
// Szenendatei. Vergibt bei Bedarf Kennungen.
[[nodiscard]] std::vector<TreeRow> build_tree(SceneDocument& document);

// Anzeigename: die Komponente Name oder „Entity <Kennung>“.
[[nodiscard]] std::string display_name(world::World& world, u64 entity, EntityId id);

// Ob child unter parent hängen darf (0: als Wurzel): nicht unter sich selbst oder einen eigenen
// Nachfahren und nicht dort, wo es schon hängt.
[[nodiscard]] bool can_reparent(const SceneDocument& document, EntityId child, EntityId parent);

} // namespace ngine::edit
