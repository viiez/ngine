module;

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

module ngine.core;

namespace ngine::events {

namespace detail {

struct Entry {
    u64 id = 0;
    // Leer, sobald abgemeldet; aufgeräumt wird, wenn gerade nichts zugestellt wird.
    std::unique_ptr<Handler> handler;
};

struct BusState {
    std::unordered_map<EventKey, std::vector<Entry>> handlers;
    std::vector<std::unique_ptr<Queued>> queue;
    u64 next_id = 1;
    // Verschachtelte Zustellungen; abgemeldete Einträge bleiben stehen, solange sie läuft.
    u32 delivering = 0;
    bool dirty = false;

    void remove(EventKey key, u64 id) noexcept {
        const auto it = handlers.find(key);
        if (it == handlers.end()) {
            return;
        }
        for (Entry& entry : it->second) {
            if (entry.id == id) {
                entry.handler.reset();
                dirty = true;
            }
        }
        compact();
    }

    void compact() noexcept {
        if (delivering > 0 || !dirty) {
            return;
        }
        for (auto& [key, entries] : handlers) {
            std::erase_if(entries, [](const Entry& entry) { return entry.handler == nullptr; });
        }
        dirty = false;
    }
};

} // namespace detail

Subscription::~Subscription() {
    reset();
}

Subscription::Subscription(Subscription&& other) noexcept
    : state_{std::move(other.state_)}, key_{other.key_}, id_{std::exchange(other.id_, 0)} {}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        key_ = other.key_;
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void Subscription::reset() noexcept {
    if (id_ == 0) {
        return;
    }
    if (const auto state = state_.lock()) {
        state->remove(key_, id_);
    }
    state_.reset();
    id_ = 0;
}

bool Subscription::active() const noexcept {
    return id_ != 0 && !state_.expired();
}

EventBus::EventBus() : state_{std::make_shared<detail::BusState>()} {}
EventBus::~EventBus() = default;

Subscription EventBus::add(EventKey key, std::unique_ptr<detail::Handler> handler) {
    const u64 id = state_->next_id++;
    state_->handlers[key].push_back(detail::Entry{.id = id, .handler = std::move(handler)});
    return Subscription{state_, key, id};
}

void EventBus::deliver(EventKey key, const void* event) {
    // Der Zustand lebt weiter, falls ein Empfänger den Bus zerstört.
    const std::shared_ptr<detail::BusState> state = state_;
    const auto it = state->handlers.find(key);
    if (it == state->handlers.end()) {
        return;
    }
    ++state->delivering;
    // Über Indizes: Neue Anmeldungen hängen an und können den Vektor verschieben; sie bekommen
    // dieses Ereignis nicht mehr.
    const usize count = it->second.size();
    for (usize i = 0; i < count; ++i) {
        detail::Handler* handler = state->handlers[key][i].handler.get();
        if (handler != nullptr) {
            handler->call(event);
        }
    }
    --state->delivering;
    state->compact();
}

void EventBus::push(std::unique_ptr<detail::Queued> event) {
    state_->queue.push_back(std::move(event));
}

usize EventBus::dispatch() {
    usize delivered = 0;
    // Neu eingereihte Ereignisse kommen in der nächsten Runde dran.
    while (!state_->queue.empty()) {
        std::vector<std::unique_ptr<detail::Queued>> batch = std::move(state_->queue);
        state_->queue.clear();
        for (const auto& event : batch) {
            deliver(event->key(), event->data());
            ++delivered;
        }
    }
    return delivered;
}

usize EventBus::pending() const noexcept {
    return state_->queue.size();
}

usize EventBus::subscribers(EventKey key) const noexcept {
    const auto it = state_->handlers.find(key);
    if (it == state_->handlers.end()) {
        return 0;
    }
    return static_cast<usize>(std::ranges::count_if(
        it->second, [](const detail::Entry& entry) { return entry.handler != nullptr; }));
}

} // namespace ngine::events
