module;

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.edit;

import ngine.core;
import ngine.reflect;
import ngine.world;

namespace ngine::edit {

// --- CommandStack -------------------------------------------------------------------------------

std::string CommandStack::undo_label() const {
    return can_undo() ? commands_[cursor_ - 1]->label() : std::string{};
}

std::string CommandStack::redo_label() const {
    return can_redo() ? commands_[cursor_]->label() : std::string{};
}

void CommandStack::clear() noexcept {
    commands_.clear();
    cursor_ = 0;
    saved_ = 0;
    saved_valid_ = true;
    sealed_ = true;
}

void CommandStack::push(std::unique_ptr<Command> command, bool merge) {
    // Redo verfällt mit jeder neuen Änderung; lag der gespeicherte Stand dort, ist er weg.
    if (cursor_ < commands_.size()) {
        commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_), commands_.end());
        if (saved_valid_ && saved_ > cursor_) {
            saved_valid_ = false;
        }
    }
    if (merge && !sealed_ && cursor_ > 0 && commands_[cursor_ - 1]->merge(*command)) {
        // Der verschmolzene Befehl ändert einen Stand, der vielleicht gespeichert war.
        if (saved_valid_ && saved_ == cursor_) {
            saved_valid_ = false;
        }
        return;
    }
    commands_.push_back(std::move(command));
    ++cursor_;
    sealed_ = !merge;
    // Ältestes vergessen, wenn der Verlauf zu lang wird.
    if (commands_.size() > limit_) {
        commands_.erase(commands_.begin());
        --cursor_;
        if (saved_valid_) {
            if (saved_ == 0) {
                saved_valid_ = false;
            } else {
                --saved_;
            }
        }
    }
}

Command* CommandStack::undo_step() noexcept {
    sealed_ = true;
    if (!can_undo()) {
        return nullptr;
    }
    return commands_[--cursor_].get();
}

Command* CommandStack::redo_step() noexcept {
    sealed_ = true;
    if (!can_redo()) {
        return nullptr;
    }
    return commands_[cursor_++].get();
}

// --- SceneDocument ------------------------------------------------------------------------------

SceneDocument::SceneDocument(world::World& world, events::EventBus* bus)
    : world_{&world}, bus_{bus} {}

EntityId SceneDocument::id_of(u64 entity) {
    if (entity == 0 || !world_->alive(entity)) {
        return 0;
    }
    if (const auto it = ids_.find(entity); it != ids_.end()) {
        return it->second;
    }
    const EntityId id = reserve_id();
    bind(id, entity);
    return id;
}

u64 SceneDocument::entity_of(EntityId id) const {
    const auto it = entities_.find(id);
    if (it == entities_.end() || !world_->alive(it->second)) {
        return 0;
    }
    return it->second;
}

void SceneDocument::bind(EntityId id, u64 entity) {
    unbind(id);
    entities_[id] = entity;
    ids_[entity] = id;
}

void SceneDocument::unbind(EntityId id) {
    const auto it = entities_.find(id);
    if (it == entities_.end()) {
        return;
    }
    if (const auto back = ids_.find(it->second); back != ids_.end() && back->second == id) {
        ids_.erase(back);
    }
    entities_.erase(it);
}

void SceneDocument::notify_dirty(bool was_dirty) {
    if (dirty() != was_dirty) {
        notify(DirtyChanged{.dirty = dirty()});
    }
}

Result<void> SceneDocument::execute(std::unique_ptr<Command> command, bool merge) {
    verify(command != nullptr, "execute() ohne Befehl");
    if (read_only()) {
        return std::unexpected(std::string{"Während des Spielens lässt sich nichts ändern"});
    }
    const bool was_dirty = dirty();
    if (auto result = command->apply(*this); !result) {
        return result;
    }
    history_.push(std::move(command), merge);
    notify_dirty(was_dirty);
    return {};
}

bool SceneDocument::undo() {
    if (read_only()) {
        return false;
    }
    const bool was_dirty = dirty();
    Command* command = history_.undo_step();
    if (command == nullptr) {
        return false;
    }
    command->revert(*this);
    notify_dirty(was_dirty);
    return true;
}

bool SceneDocument::redo() {
    if (read_only()) {
        return false;
    }
    const bool was_dirty = dirty();
    Command* command = history_.redo_step();
    if (command == nullptr) {
        return false;
    }
    if (auto result = command->apply(*this); !result) {
        // Kann nur passieren, wenn jemand die Welt am Verlauf vorbei geändert hat.
        log::error("Redo von '{}' gescheitert: {}", command->label(), result.error());
        reset_history();
        return false;
    }
    notify_dirty(was_dirty);
    return true;
}

void SceneDocument::reset_history() {
    history_.clear();
    notify(DocumentReset{});
}

void SceneDocument::destroy_all() {
    // Nur Wurzeln zerstören; Kinder verschwinden mit ihren Eltern.
    for (const u64 entity : world_->entity_ids()) {
        if (world_->alive(entity) && world_->parent(entity) == 0) {
            world_->destroy(entity);
        }
    }
    entities_.clear();
    ids_.clear();
}

void SceneDocument::clear() {
    const bool was_dirty = dirty();
    drop_play();
    destroy_all();
    path_.clear();
    history_.clear();
    notify(DocumentReset{});
    notify_dirty(was_dirty);
}

Result<usize> SceneDocument::open(std::string_view path) {
    const bool was_dirty = dirty();
    drop_play();
    destroy_all();
    history_.clear();
    path_.clear();
    auto loaded = world::load_scene_file(*world_, path, prefabs_);
    if (!loaded) {
        destroy_all();
    } else {
        path_ = std::string{path};
    }
    notify(DocumentReset{});
    notify_dirty(was_dirty);
    return loaded;
}

Result<void> SceneDocument::save(std::string_view path) {
    if (playing()) {
        return std::unexpected(std::string{"Während des Spielens wird nicht gespeichert"});
    }
    if (auto result = world::save_scene_file(*world_, path, prefabs_); !result) {
        return result;
    }
    const bool was_dirty = dirty();
    path_ = std::string{path};
    history_.mark_saved();
    notify_dirty(was_dirty);
    return {};
}

// --- Spielen ------------------------------------------------------------------------------------

void SceneDocument::drop_play() {
    if (play_backup_) {
        play_backup_.reset();
        notify(PlayStateChanged{.playing = false});
    }
}

Result<void> SceneDocument::begin_play() {
    if (playing()) {
        return std::unexpected(std::string{"Das Spiel läuft schon"});
    }
    // Alle Entities in aufsteigender Reihenfolge ihrer Kennungen in der Welt; end_play() vergibt
    // neue Kennungen in derselben Reihenfolge.
    EntitySnapshot backup;
    const std::vector<u64> entities = world_->entity_ids();
    backup.nodes.reserve(entities.size());
    for (const u64 entity : entities) {
        EntitySnapshot::Node node{
            .id = id_of(entity), .parent = id_of(world_->parent(entity)), .components = {}};
        auto visit = [&](const reflect::TypeInfo& type, world::ComponentId /*component*/,
                         const void* data) {
            node.components.push_back({.type = type.id, .bytes = capture(data, type)});
        };
        world_->for_each_component(
            entity, world::ComponentVisitor{
                        .context = &visit,
                        .visit =
                            [](void* context, const reflect::TypeInfo& type,
                               world::ComponentId component, const void* data) {
                                (*static_cast<decltype(visit)*>(context))(type, component, data);
                            },
                    });
        backup.nodes.push_back(std::move(node));
    }
    history_.seal();
    play_backup_ = std::move(backup);
    notify(PlayStateChanged{.playing = true});
    return {};
}

Result<void> SceneDocument::end_play() {
    if (!play_backup_) {
        return std::unexpected(std::string{"Es läuft kein Spiel"});
    }
    EntitySnapshot backup = *std::move(play_backup_);
    play_backup_.reset();
    destroy_all();

    // Erst alle Entities anlegen und aufsteigend vergeben (die Welt verwendet Kennungen in
    // umgekehrter Reihenfolge wieder), dann Eltern, dann Komponenten.
    std::vector<u64> fresh;
    fresh.reserve(backup.nodes.size());
    for (usize i = 0; i < backup.nodes.size(); ++i) {
        fresh.push_back(world_->create().id());
    }
    std::ranges::sort(fresh);
    for (usize i = 0; i < backup.nodes.size(); ++i) {
        bind(backup.nodes[i].id, fresh[i]);
    }
    Result<void> result;
    for (usize i = 0; i < backup.nodes.size(); ++i) {
        const EntitySnapshot::Node& node = backup.nodes[i];
        if (node.parent != 0) {
            world_->set_parent(fresh[i], entity_of(node.parent));
        }
        for (const EntitySnapshot::Component& component : node.components) {
            const reflect::TypeInfo* type = types().find_id(component.type);
            if (type == nullptr) {
                continue;
            }
            const world::ComponentId id = world_->component(*type);
            void* data = world_->ensure(fresh[i], id);
            if (auto restored = restore(data, *type, component.bytes); !restored && result) {
                result = std::unexpected(restored.error());
            }
            world_->modified(fresh[i], id);
        }
    }
    notify(PlayStateChanged{.playing = false});
    notify(EntityChanged{});
    return result;
}

} // namespace ngine::edit
