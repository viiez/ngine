module;

#include <format>
#include <string>
#include <vector>

module ngine.edit;

import ngine.core;
import ngine.world;

namespace ngine::edit {

namespace {

void append(SceneDocument& document, u64 entity, u32 depth, std::vector<TreeRow>& rows) {
    world::World& world = document.world();
    const std::vector<u64> children = world.children(entity);
    const EntityId id = document.id_of(entity);
    rows.push_back(TreeRow{.id = id,
                           .depth = depth,
                           .has_children = !children.empty(),
                           .name = display_name(world, entity, id)});
    for (const u64 child : children) {
        append(document, child, depth + 1, rows);
    }
}

} // namespace

std::vector<TreeRow> build_tree(SceneDocument& document) {
    std::vector<TreeRow> rows;
    const world::World& world = document.world();
    for (const u64 entity : world.entity_ids()) {
        if (world.parent(entity) == 0) {
            append(document, entity, 0, rows);
        }
    }
    return rows;
}

std::string display_name(world::World& world, u64 entity, EntityId id) {
    const world::ComponentId name_component = world.component<world::Name>();
    if (world.has(entity, name_component)) {
        const auto* name = static_cast<const world::Name*>(world.get(entity, name_component));
        if (!name->value.empty()) {
            return name->value;
        }
    }
    return std::format("Entity {}", id);
}

bool can_reparent(const SceneDocument& document, EntityId child, EntityId parent) {
    const world::World& world = document.world();
    const u64 child_entity = document.entity_of(child);
    if (child_entity == 0) {
        return false;
    }
    const u64 parent_entity = document.entity_of(parent);
    if (parent != 0 && parent_entity == 0) {
        return false;
    }
    if (world.parent(child_entity) == parent_entity) {
        return false;
    }
    for (u64 ancestor = parent_entity; ancestor != 0; ancestor = world.parent(ancestor)) {
        if (ancestor == child_entity) {
            return false;
        }
    }
    return true;
}

} // namespace ngine::edit
