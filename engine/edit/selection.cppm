module;

export module ngine.edit:selection;

import ngine.core;
import :document;

export namespace ngine::edit {

// Die Auswahl hat sich geändert (Szenenbaum, Klick im Viewport).
struct SelectionChanged {
    EntityId previous = 0;
    EntityId current = 0;
};

// Ausgewählte Entity des Editors. Die Auswahl gehört nicht zum Verlauf: Undo ändert sie nicht.
class Selection {
public:
    explicit Selection(events::EventBus* bus = nullptr) noexcept : bus_{bus} {}

    [[nodiscard]] EntityId current() const noexcept {
        return current_;
    }
    [[nodiscard]] bool selected(EntityId entity) const noexcept {
        return entity != 0 && entity == current_;
    }

    void select(EntityId entity) {
        if (entity == current_) {
            return;
        }
        const SelectionChanged change{.previous = current_, .current = entity};
        current_ = entity;
        if (bus_ != nullptr) {
            bus_->publish(change);
        }
    }
    void clear() {
        select(0);
    }
    // Hebt die Auswahl auf, wenn es die Entity nicht mehr gibt (gelöscht, Undo des Anlegens).
    void validate(const SceneDocument& document) {
        if (current_ != 0 && !document.exists(current_)) {
            clear();
        }
    }

private:
    events::EventBus* bus_;
    EntityId current_ = 0;
};

} // namespace ngine::edit
