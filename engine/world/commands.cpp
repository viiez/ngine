module;

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.world;

import ngine.core;
import ngine.reflect;

namespace ngine::world {

namespace {

// Größe eines Blocks für Werte; größere Werte bekommen einen eigenen Block.
constexpr usize block_size = 16 * 1024;

} // namespace

Commands::~Commands() {
    destroy_values();
}

Commands::Commands(Commands&& other) noexcept
    : world_{other.world_}, records_{std::move(other.records_)},
      created_{std::move(other.created_)}, pending_count_{other.pending_count_},
      blocks_{std::move(other.blocks_)}, block_sizes_{std::move(other.block_sizes_)},
      block_{other.block_}, offset_{other.offset_} {
    other.records_.clear();
    other.pending_count_ = 0;
    other.block_ = 0;
    other.offset_ = 0;
}

Commands& Commands::operator=(Commands&& other) noexcept {
    if (this != &other) {
        destroy_values();
        world_ = other.world_;
        records_ = std::move(other.records_);
        created_ = std::move(other.created_);
        pending_count_ = other.pending_count_;
        blocks_ = std::move(other.blocks_);
        block_sizes_ = std::move(other.block_sizes_);
        block_ = other.block_;
        offset_ = other.offset_;
        other.records_.clear();
        other.pending_count_ = 0;
        other.block_ = 0;
        other.offset_ = 0;
    }
    return *this;
}

Entity Commands::create() {
    const u64 id = pending_bit | pending_count_++;
    records_.push_back(Record{.op = Op::Create, .entity = id});
    return Entity{*world_, id};
}

Entity Commands::create(std::string_view name) {
    const u64 id = pending_bit | pending_count_++;
    records_.push_back(Record{.op = Op::Create, .entity = id, .name = std::string{name}});
    return Entity{*world_, id};
}

Entity Commands::instantiate(const Prefab& prefab, u64 prefab_id, Entity parent) {
    const u64 id = pending_bit | pending_count_++;
    records_.push_back(Record{.op = Op::Instantiate,
                              .entity = id,
                              .other = parent ? parent.id() : 0,
                              .prefab = &prefab,
                              .name = {},
                              .prefab_id = prefab_id});
    return Entity{*world_, id};
}

void Commands::destroy(Entity entity) {
    records_.push_back(Record{.op = Op::Destroy, .entity = entity.id()});
}

void Commands::set_parent(Entity child, Entity parent) {
    records_.push_back(
        Record{.op = Op::SetParent, .entity = child.id(), .other = parent ? parent.id() : 0});
}

void* Commands::push_set(Entity entity, const reflect::TypeInfo& type) {
    void* storage = allocate(type.size, type.alignment);
    records_.push_back(Record{.op = Op::Set, .entity = entity.id(), .type = &type});
    // Erst nach dem Eintrag zuweisen: Wirft push_back, gibt es keinen halb angelegten Wert.
    records_.back().value = storage;
    return storage;
}

void Commands::push_remove(Entity entity, const reflect::TypeInfo& type) {
    records_.push_back(Record{.op = Op::Remove, .entity = entity.id(), .type = &type});
}

void* Commands::allocate(usize size, usize alignment) {
    verify(alignment <= alignof(std::max_align_t),
           "Befehlspuffer: Ausrichtung der Komponente zu groß");
    size = std::max<usize>(size, 1);
    while (true) {
        if (block_ < blocks_.size()) {
            const usize start = (offset_ + alignment - 1) / alignment * alignment;
            if (start + size <= block_sizes_[block_]) {
                offset_ = start + size;
                return blocks_[block_].get() + start;
            }
            // Passt nicht mehr: nächster Block. Ein leerer Block, der zu klein ist, wird ersetzt.
            if (offset_ == 0) {
                blocks_[block_] = std::make_unique<std::byte[]>(size);
                block_sizes_[block_] = size;
                continue;
            }
            ++block_;
            offset_ = 0;
            continue;
        }
        const usize bytes = std::max(block_size, size);
        blocks_.push_back(std::make_unique<std::byte[]>(bytes));
        block_sizes_.push_back(bytes);
    }
}

u64 Commands::resolve(u64 id) const noexcept {
    if (!is_pending(id)) {
        return id;
    }
    const u64 index = id & ~pending_bit;
    return index < created_.size() ? created_[index] : 0;
}

void Commands::apply() {
    verify(!world_->running_systems(), "Commands::apply() während Systeme laufen");
    created_.assign(pending_count_, 0);
    World& world = *world_;
    for (Record& record : records_) {
        switch (record.op) {
        case Op::Create: {
            const Entity entity = record.name.empty() ? world.create() : world.create(record.name);
            created_[record.entity & ~pending_bit] = entity.id();
            break;
        }
        case Op::Instantiate: {
            const u64 parent = resolve(record.other);
            auto instance = world::instantiate(world, *record.prefab, record.prefab_id, nullptr,
                                               parent != 0 && world.alive(parent) ? parent : 0);
            if (instance) {
                created_[record.entity & ~pending_bit] = instance->id();
            } else {
                log::warn("Befehlspuffer: Vorlage lässt sich nicht anlegen: {}", instance.error());
            }
            break;
        }
        case Op::Destroy: {
            const u64 entity = resolve(record.entity);
            if (entity != 0 && world.alive(entity)) {
                world.destroy(entity);
            }
            break;
        }
        case Op::Set: {
            const u64 entity = resolve(record.entity);
            if (entity != 0 && world.alive(entity)) {
                const ComponentId component = world.component(*record.type);
                record.type->ops.move_assign(world.ensure(entity, component), record.value);
                world.modified(entity, component);
            }
            break;
        }
        case Op::Remove: {
            const u64 entity = resolve(record.entity);
            if (entity != 0 && world.alive(entity)) {
                world.remove(entity, world.component(*record.type));
            }
            break;
        }
        case Op::SetParent: {
            const u64 child = resolve(record.entity);
            const u64 parent = resolve(record.other);
            if (child != 0 && world.alive(child) && (parent == 0 || world.alive(parent))) {
                world.set_parent(child, parent);
            }
            break;
        }
        }
    }
    clear();
}

void Commands::destroy_values() noexcept {
    for (Record& record : records_) {
        if (record.value != nullptr) {
            record.type->ops.destroy(record.value);
            record.value = nullptr;
        }
    }
}

void Commands::clear() noexcept {
    destroy_values();
    records_.clear();
    created_.clear();
    pending_count_ = 0;
    block_ = 0;
    offset_ = 0;
}

} // namespace ngine::world
