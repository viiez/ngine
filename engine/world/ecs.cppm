module;

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module ngine.world:ecs;

import ngine.core;
import ngine.reflect;

export namespace ngine::world {

// Kennung einer Komponente in der Welt. Gilt nur für diese Welt und diesen Programmlauf.
using ComponentId = u64;

// Höchstzahl der Komponenten in einer Abfrage.
inline constexpr usize max_query_terms = 16;

class World;
class Schedule;

// Griff auf eine Entity: Welt und 64-Bit-Kennung, so klein wie zwei Zeiger. Wie eine Referenz in
// Java, nur dass die Entity nicht weiterlebt, solange es Griffe gibt: Nach destroy() liefert
// alive() false, und andere Aufrufe sind Fehler. Kennungen werden mit neuer Generation
// wiederverwendet, ein alter Griff zeigt also nie auf eine fremde Entity.
class Entity {
public:
    Entity() noexcept = default;
    Entity(World& world, u64 id) noexcept : world_{&world}, id_{id} {}

    [[nodiscard]] u64 id() const noexcept {
        return id_;
    }
    [[nodiscard]] World* world() const noexcept {
        return world_;
    }
    // Ob der Griff überhaupt gesetzt ist. Ob die Entity noch lebt, sagt alive().
    [[nodiscard]] explicit operator bool() const noexcept {
        return world_ != nullptr && id_ != 0;
    }
    [[nodiscard]] bool alive() const noexcept;
    void destroy();

    // Fügt eine Komponente hinzu oder ersetzt sie.
    template <typename T>
    Entity& set(T value);
    template <typename T>
    [[nodiscard]] const T* get() const;
    // Zeiger zum Ändern. Gültig bis zur nächsten Strukturänderung der Welt.
    template <typename T>
    [[nodiscard]] T* get_mut();
    template <typename T>
    [[nodiscard]] bool has() const;
    template <typename T>
    Entity& remove();

    // Hängt die Entity unter parent; ein leerer Griff löst sie vom Elternteil. Zerstört man das
    // Elternteil, verschwinden die Kinder mit.
    Entity& set_parent(Entity parent);
    [[nodiscard]] Entity parent() const;
    [[nodiscard]] std::vector<Entity> children() const;
    // Inhalt der Komponente Name, leer ohne sie.
    [[nodiscard]] std::string_view name() const;

    friend bool operator==(const Entity&, const Entity&) noexcept = default;

private:
    World* world_ = nullptr;
    u64 id_ = 0;
};

namespace detail {

struct Term {
    ComponentId component = 0;
    usize size = 0;
    bool read_only = false;
};

// Ein zusammenhängender Block von Entities mit denselben Komponenten (eine Tabelle in flecs).
// columns[i] zeigt auf das Array der i-ten Komponente der Abfrage.
struct Chunk {
    u32 count = 0;
    const u64* entities = nullptr;
    std::array<void*, max_query_terms> columns{};
};

// Verweis auf ein aufrufbares Objekt ohne Kopie und ohne Heap (wie function_ref).
struct ChunkVisitor {
    void* context;
    void (*visit)(void* context, const Chunk& chunk);
};

template <typename F>
[[nodiscard]] ChunkVisitor make_chunk_visitor(F& function) noexcept {
    return ChunkVisitor{
        .context = std::addressof(function),
        .visit = [](void* context, const Chunk& chunk) { (*static_cast<F*>(context))(chunk); },
    };
}

// Abfrage ohne Typen: hält die Abfrage von flecs und liefert Chunks.
class QueryBase {
public:
    QueryBase(World& world, std::span<const Term> terms);
    ~QueryBase();
    QueryBase(QueryBase&& other) noexcept;
    QueryBase& operator=(QueryBase&& other) noexcept;
    QueryBase(const QueryBase&) = delete;
    QueryBase& operator=(const QueryBase&) = delete;

    // Strukturänderungen in visitor (Entities anlegen, Komponenten hinzufügen) werden gesammelt
    // und erst nach der Schleife ausgeführt.
    void for_each_chunk(ChunkVisitor visitor) const;
    // Hängt alle aktuellen Chunks an out an. Die Zeiger gelten bis zur nächsten Strukturänderung.
    void collect(std::vector<Chunk>& out) const;
    [[nodiscard]] usize count() const;

    [[nodiscard]] World& world() const noexcept {
        return *world_;
    }
    [[nodiscard]] std::span<const Term> terms() const noexcept {
        return terms_;
    }

private:
    World* world_;
    void* query_ = nullptr;
    std::vector<Term> terms_;
};

template <typename... Ts>
[[nodiscard]] std::array<Term, sizeof...(Ts)> make_terms(World& world);

// Ruft function für eine Zeile auf, mit oder ohne Entity als erstem Argument.
template <typename... Ts, typename F, usize... I>
void invoke_row(F& function, World& world, const Chunk& chunk, u32 row,
                std::index_sequence<I...> /*indices*/) {
    if constexpr (std::is_invocable_v<F&, Entity, Ts&...>) {
        function(Entity{world, chunk.entities[row]}, static_cast<Ts*>(chunk.columns[I])[row]...);
    } else {
        static_assert(std::is_invocable_v<F&, Ts&...>,
                      "Die Funktion muss (Entity, Komponenten&...) oder (Komponenten&...) nehmen");
        function(static_cast<Ts*>(chunk.columns[I])[row]...);
    }
}

} // namespace detail

// Abfrage über alle Entities, die alle Komponenten Ts haben. const T heißt: nur lesen. Das
// entspricht einem Stream über passende Objekte in Java, läuft aber über dicht gepackte Arrays.
//
//   auto query = world.query<const Velocity, Position>();
//   query.each([](const Velocity& v, Position& p) { p.x += v.x; });
//
// Eine Abfrage merkt sich die passenden Tabellen; man legt sie einmal an und nutzt sie oft.
template <typename... Ts>
class Query {
public:
    static_assert(sizeof...(Ts) > 0 && sizeof...(Ts) <= max_query_terms,
                  "Eine Abfrage braucht 1 bis max_query_terms Komponenten");

    explicit Query(World& world) : base_{world, detail::make_terms<Ts...>(world)} {}

    template <typename F>
    void each(F&& function) const {
        World& world = base_.world();
        auto visit = [&](const detail::Chunk& chunk) {
            for (u32 row = 0; row < chunk.count; ++row) {
                detail::invoke_row<Ts...>(function, world, chunk, row,
                                          std::index_sequence_for<Ts...>{});
            }
        };
        base_.for_each_chunk(detail::make_chunk_visitor(visit));
    }

    [[nodiscard]] usize count() const {
        return base_.count();
    }

    [[nodiscard]] const detail::QueryBase& base() const noexcept {
        return base_;
    }

private:
    detail::QueryBase base_;
};

// Besuch einer Komponente: Typ, Kennung in der Welt und Daten.
struct ComponentVisitor {
    void* context;
    void (*visit)(void* context, const reflect::TypeInfo& type, ComponentId component,
                  const void* data);
};

// Besuch eines Chunks in der Reihenfolge der Hierarchie (World::for_each_parent_first): count
// Entities mit je einer gelesenen und einer geschriebenen Komponente, dazu die Komponente des
// Elternteils, die für alle Entities des Chunks dieselbe ist (nullptr ohne).
struct HierarchyVisitor {
    void* context;
    void (*visit)(void* context, u32 count, const void* own, void* out, const void* parent);
};

// Die Welt besitzt alle Entities und ihre Komponenten. Komponenten sind Structs, die in der
// Reflection registriert sind (siehe ADR 0018); die Welt meldet sie beim ersten Gebrauch bei flecs
// an. Eine Welt gehört einem Thread; nur Systeme (Schedule) arbeiten parallel auf ihren Daten.
class World {
public:
    // types muss die Welt überleben und world::register_types() schon gesehen haben.
    explicit World(const reflect::TypeRegistry& types);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    [[nodiscard]] Entity create();
    // Mit der Komponente Name.
    [[nodiscard]] Entity create(std::string_view name);
    [[nodiscard]] Entity entity(u64 id) noexcept {
        return Entity{*this, id};
    }
    [[nodiscard]] usize entity_count() const;
    // Kennungen aller Entities, aufsteigend sortiert.
    [[nodiscard]] std::vector<u64> entity_ids() const;

    [[nodiscard]] const reflect::TypeRegistry& types() const noexcept {
        return *types_;
    }

    template <typename T>
    [[nodiscard]] ComponentId component() {
        return component(types_->get<T>());
    }
    // Meldet den Typ bei Bedarf als Komponente an. Er muss aus types() stammen.
    [[nodiscard]] ComponentId component(const reflect::TypeInfo& type);
    // Typ einer angemeldeten Komponente, sonst nullptr.
    [[nodiscard]] const reflect::TypeInfo* component_type(ComponentId component) const noexcept;

    template <typename... Ts>
    [[nodiscard]] Query<Ts...> query() {
        return Query<Ts...>{*this};
    }
    // Einmalige Abfrage; für häufige Abfragen lohnt sich ein Query-Objekt.
    template <typename... Ts, typename F>
    void each(F&& function) {
        query<Ts...>().each(std::forward<F>(function));
    }

    // Zugriff ohne Templates, für Szene, Editor und Netzwerk. entity ist eine Kennung aus
    // Entity::id(); Komponenten müssen über component() angemeldet sein.
    [[nodiscard]] bool alive(u64 entity) const noexcept;
    void destroy(u64 entity);
    // Liefert die Komponente und legt sie mit Standardwert an, wenn sie fehlt. Nach dem Ändern
    // modified() aufrufen.
    [[nodiscard]] void* ensure(u64 entity, ComponentId component);
    void modified(u64 entity, ComponentId component);
    [[nodiscard]] const void* get(u64 entity, ComponentId component) const;
    [[nodiscard]] void* get_mut(u64 entity, ComponentId component);
    [[nodiscard]] bool has(u64 entity, ComponentId component) const;
    void remove(u64 entity, ComponentId component);
    // parent 0 löst die Entity vom Elternteil. Zyklen sind Fehler.
    void set_parent(u64 child, u64 parent);
    // 0 ohne Elternteil.
    [[nodiscard]] u64 parent(u64 entity) const;
    // Kennungen der Kinder, aufsteigend sortiert.
    [[nodiscard]] std::vector<u64> children(u64 entity) const;
    // Besucht alle Entities mit den Komponenten own (lesen) und out (schreiben), Eltern vor ihren
    // Kindern, und reicht die Komponente parent des nächsten Vorfahren mit, der sie hat. Grundlage
    // für Berechnungen entlang der Hierarchie wie die Transform-Propagation. visitor darf nur out
    // ändern.
    void for_each_parent_first(ComponentId own, ComponentId out, ComponentId parent,
                               HierarchyVisitor visitor);

    // Wer component bekommt, bekommt required automatisch mit (mit Standardwert).
    void require(ComponentId component, ComponentId required);

    // Besucht alle reflektierten Komponenten einer Entity in einer festen Reihenfolge. visitor
    // darf die Welt nicht verändern.
    void for_each_component(u64 entity, ComponentVisitor visitor) const;

    // Während Systeme laufen, sind Strukturänderungen verboten (siehe Schedule).
    [[nodiscard]] bool running_systems() const noexcept {
        return running_systems_;
    }

private:
    friend class detail::QueryBase;
    friend class Schedule;

    void check_structural_change() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    const reflect::TypeRegistry* types_;
    bool running_systems_ = false;
};

namespace detail {

template <typename... Ts>
std::array<Term, sizeof...(Ts)> make_terms(World& world) {
    return {Term{
        .component = world.component<std::remove_const_t<Ts>>(),
        .size = sizeof(Ts),
        .read_only = std::is_const_v<Ts>,
    }...};
}

} // namespace detail

template <typename T>
Entity& Entity::set(T value) {
    const ComponentId component = world_->component<T>();
    *static_cast<T*>(world_->ensure(id_, component)) = std::move(value);
    world_->modified(id_, component);
    return *this;
}

template <typename T>
const T* Entity::get() const {
    return static_cast<const T*>(world_->get(id_, world_->component<T>()));
}

template <typename T>
T* Entity::get_mut() {
    return static_cast<T*>(world_->get_mut(id_, world_->component<T>()));
}

template <typename T>
bool Entity::has() const {
    return world_->has(id_, world_->component<T>());
}

template <typename T>
Entity& Entity::remove() {
    world_->remove(id_, world_->component<T>());
    return *this;
}

} // namespace ngine::world
