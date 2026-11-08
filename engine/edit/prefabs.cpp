module;

#include <expected>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

module ngine.edit;

import ngine.core;
import ngine.math;
import ngine.world;

namespace ngine::edit {

namespace {

// Ein Knoten einer Instanz im Dokument.
struct LinkedNode {
    EntityId entity = 0;
    u32 node = 0;
};

void append(SceneDocument& document, u64 entity, std::vector<LinkedNode>& out) {
    out.push_back({.entity = document.id_of(entity), .node = static_cast<u32>(out.size() + 1)});
    for (const u64 child : document.world().children(entity)) {
        append(document, child, out);
    }
}

// Setzt bzw. entfernt die Verknüpfung einer Instanz.
void link(SceneDocument& document, const std::vector<LinkedNode>& nodes, u64 prefab,
          const std::string& overrides) {
    world::World& world = document.world();
    for (const LinkedNode& node : nodes) {
        if (const u64 entity = document.entity_of(node.entity); entity != 0) {
            world.entity(entity).set(world::PrefabNode{.node = node.node});
        }
    }
    if (const u64 root = document.entity_of(nodes.front().entity); root != 0) {
        world.entity(root).set(world::PrefabInstance{.prefab = prefab});
        if (!overrides.empty()) {
            world.entity(root).set(world::PrefabOverrides{.json = overrides});
        }
    }
    document.notify(EntityChanged{.entity = nodes.front().entity});
}

void unlink(SceneDocument& document, const std::vector<LinkedNode>& nodes) {
    if (const u64 root = document.entity_of(nodes.front().entity); root != 0) {
        world::unpack_instance(document.world(), root);
    }
    document.notify(EntityChanged{.entity = nodes.front().entity});
}

class LinkPrefab final : public Command {
public:
    LinkPrefab(std::vector<LinkedNode> nodes, u64 prefab)
        : nodes_{std::move(nodes)}, prefab_{prefab} {}

    [[nodiscard]] std::string label() const override {
        return "Prefab anlegen";
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        if (!document.exists(nodes_.front().entity)) {
            return std::unexpected(std::string{"Die Entity gibt es nicht mehr"});
        }
        link(document, nodes_, prefab_, {});
        return {};
    }
    void revert(SceneDocument& document) override {
        unlink(document, nodes_);
    }

private:
    std::vector<LinkedNode> nodes_;
    u64 prefab_;
};

class UnpackPrefab final : public Command {
public:
    UnpackPrefab(std::vector<LinkedNode> nodes, u64 prefab, std::string overrides)
        : nodes_{std::move(nodes)}, prefab_{prefab}, overrides_{std::move(overrides)} {}

    [[nodiscard]] std::string label() const override {
        return "Prefab aufbrechen";
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        if (!document.exists(nodes_.front().entity)) {
            return std::unexpected(std::string{"Die Entity gibt es nicht mehr"});
        }
        unlink(document, nodes_);
        return {};
    }
    void revert(SceneDocument& document) override {
        link(document, nodes_, prefab_, overrides_);
    }

private:
    std::vector<LinkedNode> nodes_;
    u64 prefab_;
    std::string overrides_;
};

} // namespace

Result<std::unique_ptr<Command>> link_prefab(SceneDocument& document, EntityId root,
                                             u64 prefab_id) {
    const u64 entity = document.entity_of(root);
    if (entity == 0) {
        return std::unexpected(std::string{"Keine Entity ausgewählt"});
    }
    if (prefab_id == 0) {
        return std::unexpected(std::string{"Prefab ohne Kennung"});
    }
    if (world::instance_root(document.world(), entity) != 0) {
        return std::unexpected(
            std::string{"Die Entity gehört schon zu einer Prefab-Instanz; erst aufbrechen"});
    }
    std::vector<LinkedNode> nodes;
    append(document, entity, nodes);
    return std::unique_ptr<Command>{new LinkPrefab{std::move(nodes), prefab_id}};
}

Result<std::unique_ptr<Command>> unpack_prefab(SceneDocument& document, EntityId root) {
    const u64 entity = document.entity_of(root);
    world::World& world = document.world();
    const auto* instance =
        entity != 0 ? world.entity(entity).get<world::PrefabInstance>() : nullptr;
    if (instance == nullptr) {
        return std::unexpected(std::string{"Die Entity ist keine Prefab-Instanz"});
    }
    std::vector<LinkedNode> nodes;
    for (const auto& [node, member] : world::instance_nodes(world, entity)) {
        nodes.push_back({.entity = document.id_of(member), .node = node});
    }
    const auto* stored = world.entity(entity).get<world::PrefabOverrides>();
    return std::unique_ptr<Command>{new UnpackPrefab{
        std::move(nodes), instance->prefab, stored != nullptr ? stored->json : std::string{}}};
}

Result<EntitySnapshot> prefab_snapshot(SceneDocument& document, const world::Prefab& prefab,
                                       u64 prefab_id, math::DVec3 position, EntityId parent) {
    // In einer leeren Welt mit denselben Typen anlegen und von dort festhalten.
    world::World scratch{document.types()};
    auto root = world::instantiate(scratch, prefab, prefab_id);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    world::Transform transform;
    if (const auto* current = root->get<world::Transform>()) {
        transform = *current;
    }
    transform.position = position;
    root->set(transform);
    SceneDocument source{scratch};
    EntitySnapshot result = snapshot(source, source.id_of(root->id()));
    // Kennungen auf freie des Dokuments umschreiben.
    std::unordered_map<EntityId, EntityId> ids;
    for (EntitySnapshot::Node& node : result.nodes) {
        const EntityId fresh = document.reserve_id();
        ids.emplace(node.id, fresh);
        node.id = fresh;
    }
    for (usize i = 0; i < result.nodes.size(); ++i) {
        EntitySnapshot::Node& node = result.nodes[i];
        node.parent = i == 0 ? parent : ids.at(node.parent);
    }
    return result;
}

} // namespace ngine::edit
