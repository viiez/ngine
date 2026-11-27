module ngine.animation;

import ngine.core;
import ngine.reflect;

namespace ngine::animation {

void register_types(reflect::TypeRegistry& types) {
    if (types.find<PoseMode>() == nullptr) {
        types.add<PoseMode>("PoseMode")
            .value("Visible", PoseMode::Visible)
            .value("Always", PoseMode::Always)
            .value("Never", PoseMode::Never);
    }
    if (types.find<Animator>() == nullptr) {
        types.add<Animator>("Animator")
            .replicated()
            .field("graph", &Animator::graph)
            .hint(reflect::Hint::Asset)
            .field("speed", &Animator::speed)
            .range(0.0, 10.0, 0.01)
            .field("active", &Animator::active)
            .field("pose_mode", &Animator::pose_mode);
    }
    if (types.find<AnimatorParameters>() == nullptr) {
        types.add<AnimatorParameters>("AnimatorParameters")
            .transient()
            .replicated()
            .field("p0", &AnimatorParameters::p0)
            .field("p1", &AnimatorParameters::p1)
            .field("p2", &AnimatorParameters::p2)
            .field("p3", &AnimatorParameters::p3)
            .field("triggers", &AnimatorParameters::triggers);
    }
}

} // namespace ngine::animation
