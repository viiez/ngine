module ngine.world;

import ngine.core;
import ngine.math;

namespace ngine::world {

void update_transforms(World& world) {
    auto visit = [](void* /*context*/, u32 count, const void* own, void* out, const void* parent) {
        const auto* locals = static_cast<const Transform*>(own);
        auto* globals = static_cast<WorldTransform*>(out);
        const auto* parent_global = static_cast<const WorldTransform*>(parent);
        if (parent_global == nullptr) {
            for (u32 i = 0; i < count; ++i) {
                globals[i].value = locals[i].local();
            }
        } else {
            for (u32 i = 0; i < count; ++i) {
                globals[i].value = parent_global->value * locals[i].local();
            }
        }
    };
    world.for_each_parent_first(world.component<Transform>(), world.component<WorldTransform>(),
                                world.component<WorldTransform>(),
                                HierarchyVisitor{.context = nullptr, .visit = visit});
}

} // namespace ngine::world
