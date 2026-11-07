module;

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

module ngine.edit;

import ngine.core;
import ngine.reflect;
import ngine.serialize;
import ngine.world;

namespace ngine::edit {

namespace {

std::byte* at(void* base, usize offset) noexcept {
    return static_cast<std::byte*>(base) + offset;
}

// Entity der Welt und angemeldete Komponente zu Kennung und Typ.
struct Target {
    u64 entity = 0;
    world::ComponentId component = 0;
    const reflect::TypeInfo* type = nullptr;
};

Result<Target> find_target(SceneDocument& document, EntityId id, reflect::TypeId component) {
    const u64 entity = document.entity_of(id);
    if (entity == 0) {
        return std::unexpected(std::format("Entity {} gibt es nicht", id));
    }
    const reflect::TypeInfo* type = document.types().find_id(component);
    if (type == nullptr || type->kind != reflect::Kind::Struct) {
        return std::unexpected(std::string{"Komponente ist nicht registriert"});
    }
    return Target{.entity = entity, .component = document.world().component(*type), .type = type};
}

Result<void> write_field(SceneDocument& document, EntityId id, const FieldPath& path,
                         std::span<const std::byte> bytes) {
    auto resolved = resolve(document.types(), path);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    auto target = find_target(document, id, path.component);
    if (!target) {
        return std::unexpected(target.error());
    }
    world::World& world = document.world();
    if (!world.has(target->entity, target->component)) {
        return std::unexpected(
            std::format("Entity {} hat keine Komponente {}", id, target->type->name));
    }
    void* data = world.get_mut(target->entity, target->component);
    if (auto result = restore(at(data, resolved->offset), *resolved->type, bytes); !result) {
        return result;
    }
    world.modified(target->entity, target->component);
    document.notify(EntityChanged{.entity = id, .component = path.component});
    return {};
}

// --- Feld setzen ---------------------------------------------------------------------------------

class SetField final : public Command {
public:
    SetField(EntityId entity, FieldPath path, std::string label, std::vector<std::byte> before,
             std::vector<std::byte> after)
        : entity_{entity}, path_{std::move(path)}, label_{std::move(label)},
          before_{std::move(before)}, after_{std::move(after)} {}

    [[nodiscard]] std::string label() const override {
        return std::format("{} ändern", label_);
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        return write_field(document, entity_, path_, after_);
    }
    void revert(SceneDocument& document) override {
        if (auto result = write_field(document, entity_, path_, before_); !result) {
            log::error("Undo von '{}' gescheitert: {}", label(), result.error());
        }
    }
    [[nodiscard]] bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const SetField*>(&next);
        if (other == nullptr || other->entity_ != entity_ || other->path_ != path_) {
            return false;
        }
        after_ = other->after_;
        return true;
    }

private:
    EntityId entity_;
    FieldPath path_;
    std::string label_;
    std::vector<std::byte> before_;
    std::vector<std::byte> after_;
};

// --- Umbenennen ------------------------------------------------------------------------------

class Rename final : public Command {
public:
    Rename(EntityId entity, std::string name) : entity_{entity}, name_{std::move(name)} {}

    [[nodiscard]] std::string label() const override {
        return std::format("Umbenennen in '{}'", name_);
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        const u64 entity = document.entity_of(entity_);
        if (entity == 0) {
            return std::unexpected(std::format("Entity {} gibt es nicht", entity_));
        }
        world::Entity handle = document.world().entity(entity);
        const world::Name* name = handle.get<world::Name>();
        had_name_ = name != nullptr;
        old_name_ = had_name_ ? name->value : std::string{};
        handle.set(world::Name{name_});
        document.notify(EntityChanged{.entity = entity_});
        return {};
    }
    void revert(SceneDocument& document) override {
        const u64 entity = document.entity_of(entity_);
        if (entity == 0) {
            return;
        }
        world::Entity handle = document.world().entity(entity);
        if (had_name_) {
            handle.set(world::Name{old_name_});
        } else {
            handle.remove<world::Name>();
        }
        document.notify(EntityChanged{.entity = entity_});
    }

private:
    EntityId entity_;
    std::string name_;
    std::string old_name_;
    bool had_name_ = false;
};

// --- Umhängen --------------------------------------------------------------------------------

class Reparent final : public Command {
public:
    Reparent(EntityId entity, EntityId parent) : entity_{entity}, parent_{parent} {}

    [[nodiscard]] std::string label() const override {
        return parent_ == 0 ? std::string{"Vom Elternteil lösen"} : std::string{"Umhängen"};
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        world::World& world = document.world();
        const u64 entity = document.entity_of(entity_);
        if (entity == 0) {
            return std::unexpected(std::format("Entity {} gibt es nicht", entity_));
        }
        const u64 parent = document.entity_of(parent_);
        if (parent_ != 0 && parent == 0) {
            return std::unexpected(std::format("Elternteil {} gibt es nicht", parent_));
        }
        for (u64 ancestor = parent; ancestor != 0; ancestor = world.parent(ancestor)) {
            if (ancestor == entity) {
                return std::unexpected(
                    std::string{"Eine Entity kann nicht unter sich selbst hängen"});
            }
        }
        old_parent_ = document.id_of(world.parent(entity));
        world.set_parent(entity, parent);
        document.notify(EntityChanged{.entity = entity_});
        return {};
    }
    void revert(SceneDocument& document) override {
        const u64 entity = document.entity_of(entity_);
        if (entity == 0) {
            return;
        }
        document.world().set_parent(entity, document.entity_of(old_parent_));
        document.notify(EntityChanged{.entity = entity_});
    }

private:
    EntityId entity_;
    EntityId parent_;
    EntityId old_parent_ = 0;
};

// --- Anlegen und Löschen -----------------------------------------------------------------------

class CreateEntity final : public Command {
public:
    CreateEntity(EntityId id, std::string name, EntityId parent)
        : id_{id}, name_{std::move(name)}, parent_{parent} {}

    [[nodiscard]] std::string label() const override {
        return std::format("'{}' anlegen", name_);
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        world::World& world = document.world();
        const u64 parent = document.entity_of(parent_);
        if (parent_ != 0 && parent == 0) {
            return std::unexpected(std::format("Elternteil {} gibt es nicht", parent_));
        }
        world::Entity entity = world.create(name_);
        entity.set(world::Transform{});
        if (parent != 0) {
            world.set_parent(entity.id(), parent);
        }
        document.bind(id_, entity.id());
        document.notify(EntityChanged{.entity = id_});
        return {};
    }
    void revert(SceneDocument& document) override {
        if (const u64 entity = document.entity_of(id_); entity != 0) {
            document.world().destroy(entity);
        }
        document.unbind(id_);
        document.notify(EntityChanged{.entity = id_});
    }

private:
    EntityId id_;
    std::string name_;
    EntityId parent_;
};

// Kennungen eines Teilbaums lösen, nachdem die Welt ihn zerstört hat.
void unbind_all(SceneDocument& document, const EntitySnapshot& snapshot) {
    for (const EntitySnapshot::Node& node : snapshot.nodes) {
        document.unbind(node.id);
    }
}

class DestroyEntity final : public Command {
public:
    explicit DestroyEntity(EntityId entity) : entity_{entity} {}

    [[nodiscard]] std::string label() const override {
        return name_.empty() ? std::string{"Entity löschen"} : std::format("'{}' löschen", name_);
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        const u64 entity = document.entity_of(entity_);
        if (entity == 0) {
            return std::unexpected(std::format("Entity {} gibt es nicht", entity_));
        }
        name_ = std::string{document.world().entity(entity).name()};
        saved_ = snapshot(document, entity_);
        document.world().destroy(entity);
        unbind_all(document, saved_);
        document.notify(EntityChanged{.entity = entity_});
        return {};
    }
    void revert(SceneDocument& document) override {
        if (auto result = restore(document, saved_); !result) {
            log::error("Undo von '{}' gescheitert: {}", label(), result.error());
        }
        document.notify(EntityChanged{.entity = entity_});
    }

private:
    EntityId entity_;
    std::string name_;
    EntitySnapshot saved_;
};

// --- Komponenten -------------------------------------------------------------------------------

class AddComponent final : public Command {
public:
    AddComponent(EntityId entity, reflect::TypeId component)
        : entity_{entity}, component_{component} {}

    [[nodiscard]] std::string label() const override {
        return std::format("{} hinzufügen", type_name_);
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        auto target = find_target(document, entity_, component_);
        if (!target) {
            return std::unexpected(target.error());
        }
        type_name_ = target->type->name;
        world::World& world = document.world();
        if (world.has(target->entity, target->component)) {
            return std::unexpected(std::format("{} ist schon vorhanden", type_name_));
        }
        (void)world.ensure(target->entity, target->component);
        world.modified(target->entity, target->component);
        document.notify(EntityChanged{.entity = entity_});
        return {};
    }
    void revert(SceneDocument& document) override {
        if (auto target = find_target(document, entity_, component_)) {
            document.world().remove(target->entity, target->component);
            document.notify(EntityChanged{.entity = entity_});
        }
    }

private:
    EntityId entity_;
    reflect::TypeId component_;
    std::string type_name_ = "Komponente";
};

class RemoveComponent final : public Command {
public:
    RemoveComponent(EntityId entity, reflect::TypeId component)
        : entity_{entity}, component_{component} {}

    [[nodiscard]] std::string label() const override {
        return std::format("{} entfernen", type_name_);
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        auto target = find_target(document, entity_, component_);
        if (!target) {
            return std::unexpected(target.error());
        }
        type_name_ = target->type->name;
        world::World& world = document.world();
        if (!world.has(target->entity, target->component)) {
            return std::unexpected(std::format("{} ist nicht vorhanden", type_name_));
        }
        bytes_ = capture(world.get(target->entity, target->component), *target->type);
        world.remove(target->entity, target->component);
        document.notify(EntityChanged{.entity = entity_});
        return {};
    }
    void revert(SceneDocument& document) override {
        auto target = find_target(document, entity_, component_);
        if (!target) {
            return;
        }
        world::World& world = document.world();
        void* data = world.ensure(target->entity, target->component);
        if (auto result = restore(data, *target->type, bytes_); !result) {
            log::error("Undo von '{}' gescheitert: {}", label(), result.error());
        }
        world.modified(target->entity, target->component);
        document.notify(EntityChanged{.entity = entity_});
    }

private:
    EntityId entity_;
    reflect::TypeId component_;
    std::string type_name_ = "Komponente";
    std::vector<std::byte> bytes_;
};

// --- Gruppe ----------------------------------------------------------------------------------

class Group final : public Command {
public:
    Group(std::string label, std::vector<std::unique_ptr<Command>> commands)
        : label_{std::move(label)}, commands_{std::move(commands)} {}

    [[nodiscard]] std::string label() const override {
        return label_;
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        for (usize i = 0; i < commands_.size(); ++i) {
            if (auto result = commands_[i]->apply(document); !result) {
                // Was schon lief, zurücknehmen: Die Gruppe wirkt ganz oder gar nicht.
                for (usize j = i; j > 0; --j) {
                    commands_[j - 1]->revert(document);
                }
                return result;
            }
        }
        return {};
    }
    void revert(SceneDocument& document) override {
        for (usize i = commands_.size(); i > 0; --i) {
            commands_[i - 1]->revert(document);
        }
    }

private:
    std::string label_;
    std::vector<std::unique_ptr<Command>> commands_;
};

void append_subtree(SceneDocument& document, u64 entity, EntityId parent,
                    EntitySnapshot& snapshot) {
    world::World& world = document.world();
    EntitySnapshot::Node node{.id = document.id_of(entity), .parent = parent, .components = {}};
    auto visit = [&](const reflect::TypeInfo& type, world::ComponentId /*component*/,
                     const void* data) {
        node.components.push_back({.type = type.id, .bytes = capture(data, type)});
    };
    world.for_each_component(entity, world::ComponentVisitor{
                                         .context = &visit,
                                         .visit =
                                             [](void* context, const reflect::TypeInfo& type,
                                                world::ComponentId component, const void* data) {
                                                 (*static_cast<decltype(visit)*>(context))(
                                                     type, component, data);
                                             },
                                     });
    const EntityId id = node.id;
    snapshot.nodes.push_back(std::move(node));
    for (const u64 child : world.children(entity)) {
        append_subtree(document, child, id, snapshot);
    }
}

} // namespace

// --- Werte und Wege --------------------------------------------------------------------------

Result<ResolvedField> resolve(const reflect::TypeRegistry& types, const FieldPath& path) {
    const reflect::TypeInfo* component = types.find_id(path.component);
    if (component == nullptr) {
        return std::unexpected(std::string{"Komponente ist nicht registriert"});
    }
    ResolvedField resolved{.component = component, .type = component};
    for (const u32 index : path.fields) {
        if (index >= resolved.type->fields.size()) {
            return std::unexpected(
                std::format("Feld {} gibt es in {} nicht", index, resolved.type->name));
        }
        resolved.field = &resolved.type->fields[index];
        resolved.offset += resolved.field->offset;
        resolved.type = resolved.field->type;
    }
    return resolved;
}

std::string to_string(const reflect::TypeRegistry& types, const FieldPath& path) {
    const reflect::TypeInfo* type = types.find_id(path.component);
    if (type == nullptr) {
        return "?";
    }
    std::string text = type->name;
    for (const u32 index : path.fields) {
        if (index >= type->fields.size()) {
            return text + ".?";
        }
        text += "." + type->fields[index].name;
        type = type->fields[index].type;
    }
    return text;
}

std::vector<std::byte> capture(const void* value, const reflect::TypeInfo& type) {
    serialize::BinaryWriter writer;
    serialize::write_binary(writer, value, type);
    return writer.take();
}

Result<void> restore(void* target, const reflect::TypeInfo& type,
                     std::span<const std::byte> bytes) {
    serialize::BinaryReader reader{bytes};
    if (auto result = serialize::read_binary(reader, target, type); !result) {
        return std::unexpected(result.error());
    }
    if (reader.remaining() != 0) {
        return std::unexpected(std::format("Zu viele Bytes für '{}'", type.name));
    }
    return {};
}

EntitySnapshot snapshot(SceneDocument& document, EntityId root) {
    EntitySnapshot result;
    const u64 entity = document.entity_of(root);
    if (entity == 0) {
        return result;
    }
    append_subtree(document, entity, document.id_of(document.world().parent(entity)), result);
    return result;
}

Result<void> restore(SceneDocument& document, const EntitySnapshot& snapshot) {
    world::World& world = document.world();
    for (const EntitySnapshot::Node& node : snapshot.nodes) {
        if (document.exists(node.id)) {
            return std::unexpected(std::format("Entity {} gibt es schon", node.id));
        }
        const u64 parent = document.entity_of(node.parent);
        if (node.parent != 0 && parent == 0) {
            return std::unexpected(std::format("Elternteil {} gibt es nicht", node.parent));
        }
        const world::Entity entity = world.create();
        document.bind(node.id, entity.id());
        if (parent != 0) {
            world.set_parent(entity.id(), parent);
        }
        for (const EntitySnapshot::Component& component : node.components) {
            const reflect::TypeInfo* type = document.types().find_id(component.type);
            if (type == nullptr) {
                continue;
            }
            const world::ComponentId id = world.component(*type);
            void* data = world.ensure(entity.id(), id);
            if (auto result = restore(data, *type, component.bytes); !result) {
                return result;
            }
            world.modified(entity.id(), id);
        }
    }
    return {};
}

// --- Fabriken ----------------------------------------------------------------------------------

Result<std::unique_ptr<Command>> set_field(SceneDocument& document, EntityId entity,
                                           const FieldPath& path, const void* value) {
    auto resolved = resolve(document.types(), path);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    auto target = find_target(document, entity, path.component);
    if (!target) {
        return std::unexpected(target.error());
    }
    const world::World& world = document.world();
    if (!world.has(target->entity, target->component)) {
        return std::unexpected(
            std::format("Entity {} hat keine Komponente {}", entity, target->type->name));
    }
    const auto* data = static_cast<const std::byte*>(world.get(target->entity, target->component));
    std::vector<std::byte> before = capture(data + resolved->offset, *resolved->type);
    std::vector<std::byte> after = capture(value, *resolved->type);
    if (before == after) {
        return std::unique_ptr<Command>{};
    }
    return std::unique_ptr<Command>{std::make_unique<SetField>(
        entity, path, to_string(document.types(), path), std::move(before), std::move(after))};
}

std::unique_ptr<Command> rename(EntityId entity, std::string name) {
    return std::make_unique<Rename>(entity, std::move(name));
}

std::unique_ptr<Command> reparent(EntityId entity, EntityId parent) {
    return std::make_unique<Reparent>(entity, parent);
}

CreatedEntity create_entity(SceneDocument& document, std::string name, EntityId parent) {
    const EntityId id = document.reserve_id();
    return {.command = std::make_unique<CreateEntity>(id, std::move(name), parent), .id = id};
}

std::unique_ptr<Command> destroy_entity(EntityId entity) {
    return std::make_unique<DestroyEntity>(entity);
}

std::unique_ptr<Command> add_component(EntityId entity, reflect::TypeId component) {
    return std::make_unique<AddComponent>(entity, component);
}

std::unique_ptr<Command> remove_component(EntityId entity, reflect::TypeId component) {
    return std::make_unique<RemoveComponent>(entity, component);
}

std::unique_ptr<Command> group(std::string label, std::vector<std::unique_ptr<Command>> commands) {
    return std::make_unique<Group>(std::move(label), std::move(commands));
}

} // namespace ngine::edit
