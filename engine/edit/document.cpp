module;

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

module ngine.edit;

import ngine.core;
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
    const bool was_dirty = dirty();
    if (auto result = command->apply(*this); !result) {
        return result;
    }
    history_.push(std::move(command), merge);
    notify_dirty(was_dirty);
    return {};
}

bool SceneDocument::undo() {
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
    destroy_all();
    path_.clear();
    history_.clear();
    notify(DocumentReset{});
    notify_dirty(was_dirty);
}

Result<usize> SceneDocument::open(std::string_view path) {
    const bool was_dirty = dirty();
    destroy_all();
    history_.clear();
    path_.clear();
    auto loaded = world::load_scene_file(*world_, path);
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
    if (auto result = world::save_scene_file(*world_, path); !result) {
        return result;
    }
    const bool was_dirty = dirty();
    path_ = std::string{path};
    history_.mark_saved();
    notify_dirty(was_dirty);
    return {};
}

} // namespace ngine::edit
