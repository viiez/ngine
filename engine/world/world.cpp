module;

#include <flecs.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Adapter für flecs (ADR 0003, ADR 0009). Nur diese Datei kennt die Bibliothek.
module ngine.world;

import ngine.core;
import ngine.reflect;

namespace ngine::world {

namespace {

// --- Lebenszyklus der Komponenten -----------------------------------------------------------
// flecs ruft diese Funktionen für ganze Bereiche auf. Das TypeInfo der Reflection steckt im
// Kontext der Hooks.

const reflect::TypeInfo& type_of(const ecs_type_info_t* info) noexcept {
    return *static_cast<const reflect::TypeInfo*>(info->hooks.ctx);
}

std::byte* element(void* base, i32 index, usize size) noexcept {
    return static_cast<std::byte*>(base) + static_cast<usize>(index) * size;
}
const std::byte* element(const void* base, i32 index, usize size) noexcept {
    return static_cast<const std::byte*>(base) + static_cast<usize>(index) * size;
}

void hook_ctor(void* ptr, i32 count, const ecs_type_info_t* info) {
    const auto& type = type_of(info);
    for (i32 i = 0; i < count; ++i) {
        type.ops.construct(element(ptr, i, type.size));
    }
}

void hook_dtor(void* ptr, i32 count, const ecs_type_info_t* info) {
    const auto& type = type_of(info);
    for (i32 i = 0; i < count; ++i) {
        type.ops.destroy(element(ptr, i, type.size));
    }
}

void hook_copy(void* dst, const void* src, i32 count, const ecs_type_info_t* info) {
    const auto& type = type_of(info);
    for (i32 i = 0; i < count; ++i) {
        type.ops.copy_assign(element(dst, i, type.size), element(src, i, type.size));
    }
}

void hook_move(void* dst, void* src, i32 count, const ecs_type_info_t* info) {
    const auto& type = type_of(info);
    for (i32 i = 0; i < count; ++i) {
        type.ops.move_assign(element(dst, i, type.size), element(src, i, type.size));
    }
}

void hook_copy_ctor(void* dst, const void* src, i32 count, const ecs_type_info_t* info) {
    const auto& type = type_of(info);
    for (i32 i = 0; i < count; ++i) {
        type.ops.copy_construct(element(dst, i, type.size), element(src, i, type.size));
    }
}

void hook_move_ctor(void* dst, void* src, i32 count, const ecs_type_info_t* info) {
    const auto& type = type_of(info);
    for (i32 i = 0; i < count; ++i) {
        type.ops.move_construct(element(dst, i, type.size), element(src, i, type.size));
    }
}

// --- Log von flecs ins Engine-Log ------------------------------------------------------------

void flecs_log(i32 level, const char* file, i32 line, const char* message) {
    if (level <= -4) {
        log::fatal("flecs: {} ({}:{})", message, file != nullptr ? file : "?", line);
    } else if (level == -3) {
        log::error("flecs: {} ({}:{})", message, file != nullptr ? file : "?", line);
    } else if (level == -2) {
        log::warn("flecs: {}", message);
    } else {
        log::debug("flecs: {}", message);
    }
}

void install_flecs_os_api() {
    // Die OS-API von flecs ist global. Einmal setzen, bevor die erste Welt entsteht.
    static std::once_flag once;
    std::call_once(once, [] {
        ecs_os_set_api_defaults();
        ecs_os_api_t api = ecs_os_get_api();
        api.log_ = &flecs_log;
        ecs_os_set_api(&api);
    });
}

} // namespace

struct World::Impl {
    ecs_world_t* world = nullptr;
    // Markiert alle Entities, die über World::create() entstanden sind. So unterscheiden sich
    // Spiel-Entities von den internen Entities, die flecs selbst anlegt (Komponenten usw.).
    ecs_entity_t tag = 0;
    std::unordered_map<const reflect::TypeInfo*, ComponentId> components;
    std::unordered_map<ComponentId, const reflect::TypeInfo*> types;
};

World::World(const reflect::TypeRegistry& types) : impl_{std::make_unique<Impl>()}, types_{&types} {
    verify(types.find<Name>() != nullptr,
           "world::register_types() muss vor dem ersten World-Objekt laufen");
    install_flecs_os_api();
    // Nur der Kern von flecs: ohne Pipeline, Systeme, Zeitgeber und REST-Schnittstelle. Systeme
    // plant die Engine selbst auf dem Job-System (ADR 0019).
    impl_->world = ecs_mini();
    impl_->tag = ecs_new(impl_->world);
}

World::~World() {
    ecs_fini(impl_->world);
}

void World::check_structural_change() const {
    verify(!running_systems_,
           "Änderung über die Welt, während Systeme laufen: Systeme ändern nur ihre Komponenten");
}

Entity World::create() {
    check_structural_change();
    return Entity{*this, ecs_new_w_id(impl_->world, impl_->tag)};
}

Entity World::create(std::string_view name) {
    Entity entity = create();
    entity.set(Name{std::string{name}});
    return entity;
}

usize World::entity_count() const {
    return static_cast<usize>(ecs_count_id(impl_->world, impl_->tag));
}

std::vector<u64> World::entity_ids() const {
    std::vector<u64> ids;
    ecs_iter_t it = ecs_each_id(impl_->world, impl_->tag);
    while (ecs_each_next(&it)) {
        ids.insert(ids.end(), it.entities, it.entities + it.count);
    }
    std::ranges::sort(ids);
    return ids;
}

ComponentId World::component(const reflect::TypeInfo& type) {
    if (const auto it = impl_->components.find(&type); it != impl_->components.end()) {
        return it->second;
    }
    verify(types_->find_id(type.id) == &type, "Komponente stammt nicht aus der Registry der Welt");
    verify(type.kind == reflect::Kind::Struct, "Komponenten müssen Structs sein");
    check_structural_change();

    ecs_component_desc_t desc{};
    // Niedrige Kennungen (unter 256) schlägt flecs schneller nach.
    desc.entity = ecs_new_low_id(impl_->world);
    desc.type.size = static_cast<ecs_size_t>(type.size);
    desc.type.alignment = static_cast<ecs_size_t>(type.alignment);
    desc.type.name = type.name.c_str();
    const ecs_entity_t component = ecs_component_init(impl_->world, &desc);
    verify(component != 0, "flecs konnte die Komponente nicht anlegen");

    // Triviale Typen kopiert flecs selbst mit memcpy; die übrigen brauchen ihre Konstruktoren.
    ecs_type_hooks_t hooks{};
    hooks.ctx = const_cast<reflect::TypeInfo*>(&type);
    hooks.ctor = &hook_ctor;
    if (!type.trivial) {
        hooks.dtor = &hook_dtor;
        hooks.copy = &hook_copy;
        hooks.move = &hook_move;
        hooks.copy_ctor = &hook_copy_ctor;
        hooks.move_ctor = &hook_move_ctor;
    }
    ecs_set_hooks_id(impl_->world, component, &hooks);

    impl_->components.emplace(&type, component);
    impl_->types.emplace(component, &type);
    return component;
}

const reflect::TypeInfo* World::component_type(ComponentId component) const noexcept {
    const auto it = impl_->types.find(component);
    return it != impl_->types.end() ? it->second : nullptr;
}

bool World::alive(u64 entity) const noexcept {
    return entity != 0 && ecs_is_alive(impl_->world, entity);
}

void World::destroy(u64 entity) {
    check_structural_change();
    verify(alive(entity), "destroy() auf einer Entity, die nicht lebt");
    ecs_delete(impl_->world, entity);
}

void* World::ensure(u64 entity, ComponentId component) {
    check_structural_change();
    verify(alive(entity), "Komponente an einer Entity, die nicht lebt");
    const reflect::TypeInfo* type = component_type(component);
    verify(type != nullptr, "Unbekannte Komponente");
    return ecs_ensure_id(impl_->world, entity, component, type->size);
}

void World::modified(u64 entity, ComponentId component) {
    check_structural_change();
    ecs_modified_id(impl_->world, entity, component);
}

const void* World::get(u64 entity, ComponentId component) const {
    verify(alive(entity), "Komponente an einer Entity, die nicht lebt");
    return ecs_get_id(impl_->world, entity, component);
}

void* World::get_mut(u64 entity, ComponentId component) {
    verify(alive(entity), "Komponente an einer Entity, die nicht lebt");
    return ecs_get_mut_id(impl_->world, entity, component);
}

bool World::has(u64 entity, ComponentId component) const {
    verify(alive(entity), "Komponente an einer Entity, die nicht lebt");
    return ecs_has_id(impl_->world, entity, component);
}

void World::remove(u64 entity, ComponentId component) {
    check_structural_change();
    verify(alive(entity), "Komponente an einer Entity, die nicht lebt");
    ecs_remove_id(impl_->world, entity, component);
}

void World::set_parent(u64 child, u64 parent) {
    check_structural_change();
    verify(alive(child), "set_parent() auf einer Entity, die nicht lebt");
    if (parent == 0) {
        ecs_remove_pair(impl_->world, child, EcsChildOf, EcsWildcard);
        return;
    }
    verify(alive(parent), "Elternteil lebt nicht");
    for (u64 ancestor = parent; ancestor != 0; ancestor = this->parent(ancestor)) {
        verify(ancestor != child, "set_parent() würde einen Zyklus bilden");
    }
    // ChildOf ist exklusiv: Ein neues Elternteil ersetzt das alte.
    ecs_add_pair(impl_->world, child, EcsChildOf, parent);
}

u64 World::parent(u64 entity) const {
    verify(alive(entity), "parent() auf einer Entity, die nicht lebt");
    return ecs_get_target(impl_->world, entity, EcsChildOf, 0);
}

std::vector<u64> World::children(u64 entity) const {
    verify(alive(entity), "children() auf einer Entity, die nicht lebt");
    std::vector<u64> ids;
    ecs_iter_t it = ecs_children(impl_->world, entity);
    while (ecs_children_next(&it)) {
        ids.insert(ids.end(), it.entities, it.entities + it.count);
    }
    std::ranges::sort(ids);
    return ids;
}

void World::for_each_component(u64 entity, ComponentVisitor visitor) const {
    verify(alive(entity), "for_each_component() auf einer Entity, die nicht lebt");
    const ecs_type_t* type = ecs_get_type(impl_->world, entity);
    if (type == nullptr) {
        return;
    }
    // Die Reihenfolge in flecs folgt den Kennungen der Komponenten, also der Anmeldung.
    for (i32 i = 0; i < type->count; ++i) {
        const ecs_id_t id = type->array[i];
        if (ecs_id_is_pair(id)) {
            continue;
        }
        const reflect::TypeInfo* info = component_type(id);
        if (info == nullptr) {
            continue;
        }
        visitor.visit(visitor.context, *info, id, ecs_get_id(impl_->world, entity, id));
    }
}

// --- Entity ------------------------------------------------------------------------------------

bool Entity::alive() const noexcept {
    return world_ != nullptr && world_->alive(id_);
}

void Entity::destroy() {
    world_->destroy(id_);
}

Entity& Entity::set_parent(Entity parent) {
    verify(!parent || parent.world_ == world_, "Elternteil aus einer anderen Welt");
    world_->set_parent(id_, parent.id_);
    return *this;
}

Entity Entity::parent() const {
    const u64 id = world_->parent(id_);
    return id != 0 ? Entity{*world_, id} : Entity{};
}

std::vector<Entity> Entity::children() const {
    std::vector<Entity> result;
    for (const u64 id : world_->children(id_)) {
        result.emplace_back(*world_, id);
    }
    return result;
}

std::string_view Entity::name() const {
    const Name* name = get<Name>();
    return name != nullptr ? std::string_view{name->value} : std::string_view{};
}

// --- Abfragen ----------------------------------------------------------------------------------

namespace detail {

QueryBase::QueryBase(World& world, std::span<const Term> terms)
    : world_{&world}, terms_{terms.begin(), terms.end()} {
    verify(!terms.empty() && terms.size() <= max_query_terms, "Ungültige Zahl von Komponenten");
    ecs_query_desc_t desc{};
    for (usize i = 0; i < terms.size(); ++i) {
        desc.terms[i].id = terms[i].component;
        desc.terms[i].inout = static_cast<i16>(terms[i].read_only ? EcsIn : EcsInOut);
    }
    // Mit Cache: flecs merkt sich die passenden Tabellen und aktualisiert sie bei Änderungen.
    desc.cache_kind = EcsQueryCacheAuto;
    query_ = ecs_query_init(world.impl_->world, &desc);
    verify(query_ != nullptr, "flecs konnte die Abfrage nicht anlegen");
}

QueryBase::~QueryBase() {
    if (query_ != nullptr) {
        ecs_query_fini(static_cast<ecs_query_t*>(query_));
    }
}

QueryBase::QueryBase(QueryBase&& other) noexcept
    : world_{other.world_}, query_{std::exchange(other.query_, nullptr)},
      terms_{std::move(other.terms_)} {}

QueryBase& QueryBase::operator=(QueryBase&& other) noexcept {
    if (this != &other) {
        if (query_ != nullptr) {
            ecs_query_fini(static_cast<ecs_query_t*>(query_));
        }
        world_ = other.world_;
        query_ = std::exchange(other.query_, nullptr);
        terms_ = std::move(other.terms_);
    }
    return *this;
}

namespace {

Chunk make_chunk(const ecs_iter_t& it, std::span<const Term> terms) {
    Chunk chunk;
    chunk.count = static_cast<u32>(it.count);
    chunk.entities = it.entities;
    for (usize i = 0; i < terms.size(); ++i) {
        chunk.columns[i] = ecs_field_w_size(&it, terms[i].size, static_cast<i8>(i));
    }
    return chunk;
}

} // namespace

void QueryBase::for_each_chunk(ChunkVisitor visitor) const {
    ecs_world_t* world = world_->impl_->world;
    const auto* query = static_cast<const ecs_query_t*>(query_);
    // Aufgeschobene Strukturänderungen: Was visitor anlegt oder entfernt, wirkt erst nach der
    // Schleife. Sonst würden sich die Tabellen unter dem Iterator verschieben.
    ecs_defer_begin(world);
    ecs_iter_t it = ecs_query_iter(world, query);
    while (ecs_query_next(&it)) {
        visitor.visit(visitor.context, make_chunk(it, terms_));
    }
    ecs_defer_end(world);
}

void QueryBase::collect(std::vector<Chunk>& out) const {
    ecs_iter_t it = ecs_query_iter(world_->impl_->world, static_cast<const ecs_query_t*>(query_));
    while (ecs_query_next(&it)) {
        if (it.count > 0) {
            out.push_back(make_chunk(it, terms_));
        }
    }
}

usize QueryBase::count() const {
    return static_cast<usize>(ecs_query_count(static_cast<const ecs_query_t*>(query_)).entities);
}

} // namespace detail

void register_types(reflect::TypeRegistry& types) {
    if (types.find<Name>() == nullptr) {
        types.add<Name>("Name").field("value", &Name::value);
    }
}

} // namespace ngine::world
