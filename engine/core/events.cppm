module;

#include <memory>
#include <utility>

// Ereignisse (CORE-11): lose gekoppelte Nachrichten zwischen Teilen eines Programms, etwa zwischen
// den Fenstern des Editors (Auswahl, Änderungen). Ein Ereignis ist ein beliebiger Struct; wer es
// empfangen will, meldet eine Funktion für genau diesen Typ an. Entweder sofort zustellen
// (publish) oder einreihen und später gesammelt zustellen (enqueue, dispatch), z. B. am Ende eines
// Frames. Ein EventBus gehört einem Thread.
//
//   events::EventBus bus;
//   auto subscription = bus.subscribe<SelectionChanged>([](const SelectionChanged& e) { ... });
//   bus.publish(SelectionChanged{.entity = 7});
export module ngine.core:events;

import :types;

namespace ngine::events::detail {

// Eindeutige Adresse pro Ereignistyp, ohne RTTI.
template <typename E>
inline constexpr char event_tag = 0;

} // namespace ngine::events::detail

export namespace ngine::events {

using EventKey = const void*;

template <typename E>
[[nodiscard]] constexpr EventKey event_key() noexcept {
    return &detail::event_tag<E>;
}

namespace detail {

// Typgelöschte Empfängerfunktion bzw. eingereihtes Ereignis.
struct Handler {
    virtual ~Handler() = default;
    virtual void call(const void* event) = 0;
};

template <typename E, typename F>
struct HandlerOf final : Handler {
    explicit HandlerOf(F function) : function_{std::move(function)} {}
    void call(const void* event) override {
        function_(*static_cast<const E*>(event));
    }
    F function_;
};

struct Queued {
    virtual ~Queued() = default;
    [[nodiscard]] virtual EventKey key() const noexcept = 0;
    [[nodiscard]] virtual const void* data() const noexcept = 0;
};

template <typename E>
struct QueuedOf final : Queued {
    explicit QueuedOf(E event) : event_{std::move(event)} {}
    [[nodiscard]] EventKey key() const noexcept override {
        return event_key<E>();
    }
    [[nodiscard]] const void* data() const noexcept override {
        return &event_;
    }
    E event_;
};

struct BusState;

} // namespace detail

// Anmeldung einer Empfängerfunktion; meldet sie beim Zerstören ab. Darf den EventBus überleben.
class Subscription {
public:
    Subscription() noexcept = default;
    ~Subscription();
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    // Meldet sofort ab; danach ruft der Bus die Funktion nicht mehr auf, auch nicht im laufenden
    // Zustellen.
    void reset() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    friend class EventBus;
    Subscription(std::weak_ptr<detail::BusState> state, EventKey key, u64 id) noexcept
        : state_{std::move(state)}, key_{key}, id_{id} {}

    std::weak_ptr<detail::BusState> state_;
    EventKey key_ = nullptr;
    u64 id_ = 0;
};

class EventBus {
public:
    EventBus();
    ~EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Meldet function für Ereignisse vom Typ E an. Funktionen eines Typs laufen in der
    // Reihenfolge ihrer Anmeldung. Wer während des Zustellens anmeldet, bekommt erst das nächste
    // Ereignis.
    template <typename E, typename F>
    [[nodiscard]] Subscription subscribe(F&& function) {
        return add(event_key<E>(), std::make_unique<detail::HandlerOf<E, std::decay_t<F>>>(
                                       std::forward<F>(function)));
    }

    // Stellt das Ereignis sofort allen Empfängern zu.
    template <typename E>
    void publish(const E& event) {
        deliver(event_key<E>(), &event);
    }

    // Reiht das Ereignis für das nächste dispatch() ein.
    template <typename E>
    void enqueue(E event) {
        push(std::make_unique<detail::QueuedOf<E>>(std::move(event)));
    }

    // Stellt alle eingereihten Ereignisse in der Reihenfolge des Einreihens zu, auch solche, die
    // Empfänger dabei neu einreihen. Liefert die Zahl der zugestellten Ereignisse.
    usize dispatch();
    [[nodiscard]] usize pending() const noexcept;
    // Zahl der Empfänger für E.
    template <typename E>
    [[nodiscard]] usize subscribers() const noexcept {
        return subscribers(event_key<E>());
    }

private:
    Subscription add(EventKey key, std::unique_ptr<detail::Handler> handler);
    void deliver(EventKey key, const void* event);
    void push(std::unique_ptr<detail::Queued> event);
    [[nodiscard]] usize subscribers(EventKey key) const noexcept;

    std::shared_ptr<detail::BusState> state_;
};

} // namespace ngine::events
