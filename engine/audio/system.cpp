module;

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

module ngine.audio;

import ngine.core;
import ngine.math;
import ngine.asset;
import ngine.world;

namespace ngine::audio {

namespace {

// So viele Bilder wartet ein Einmal-Klang höchstens auf das Laden, dann verfällt er (ein Schritt,
// der eine Sekunde zu spät kommt, klingt falsch).
constexpr u32 max_one_shot_wait = 30;

} // namespace

AudioSystem::AudioSystem(AudioEngine& engine, asset::ResourceManager& resources,
                         const asset::AssetDatabase* database)
    : engine_{&engine}, resources_{&resources}, database_{database} {}

AudioSystem::~AudioSystem() {
    stop_all();
}

f32 AudioSystem::random() noexcept {
    // xorshift64*: reicht für Streuung und ist überall gleich.
    random_state_ ^= random_state_ >> 12;
    random_state_ ^= random_state_ << 25;
    random_state_ ^= random_state_ >> 27;
    const u64 value = random_state_ * 2685821657736338717ull;
    return static_cast<f32>(value >> 40) / static_cast<f32>(1u << 24);
}

AudioSystem::Choice AudioSystem::choose(u64 sound) {
    Choice choice;
    if (sound == 0) {
        return choice;
    }
    u64 id = sound;
    const asset::AssetInfo* info = database_ != nullptr ? database_->find(sound) : nullptr;
    if (info != nullptr && info->type == asset::AssetType::SoundSet) {
        auto [it, added] = sets_.try_emplace(sound);
        if (added) {
            it->second = resources_->load<SoundSet>(sound);
        }
        const SoundSet* set = it->second.get();
        if (set == nullptr) {
            choice.pending = it->second.state() == asset::ResourceState::Loading;
            return choice;
        }
        if (set->sounds.empty()) {
            return choice;
        }
        // Alle Klänge des Satzes vorladen, damit die nächste Wahl schon bereit ist.
        for (const u64 member : set->sounds) {
            if (!sounds_.contains(member)) {
                sounds_.emplace(member, resources_->load<Sound>(member));
            }
        }
        const auto index =
            std::min(static_cast<usize>(random() * static_cast<f32>(set->sounds.size())),
                     set->sounds.size() - 1);
        id = set->sounds[index];
        choice.volume = 1.0f - std::clamp(set->volume_variation, 0.0f, 1.0f) * random();
        choice.pitch =
            1.0f + std::clamp(set->pitch_variation, 0.0f, 1.0f) * (2.0f * random() - 1.0f);
    }
    auto [it, added] = sounds_.try_emplace(id);
    if (added || !it->second) {
        it->second = resources_->load<Sound>(id);
    }
    choice.sound = it->second.get();
    choice.id = id;
    choice.version = it->second.version();
    if (choice.sound == nullptr) {
        choice.pending = it->second.state() == asset::ResourceState::Loading;
    }
    return choice;
}

VoiceMix AudioSystem::mix_for(const EmitterPose& pose, bool spatial, f32 volume,
                              f32 pitch) const noexcept {
    const f32 gain = volume * listener_volume_;
    if (!spatial) {
        return VoiceMix{.left = gain, .right = gain, .pitch = pitch};
    }
    const SpatialResult result = spatialize(listener_, pose);
    return VoiceMix{.left = result.left * gain,
                    .right = result.right * gain,
                    .pitch = std::clamp(pitch * result.doppler_pitch, min_pitch, max_pitch)};
}

void AudioSystem::update(world::World& world, f64 dt) {
    ++frame_;
    dt_ = dt;
    pending_ = 0;
    engine_->update();

    // Hörer: Entity mit AudioListener (kleinste Kennung), sonst die Lage der App.
    std::optional<math::Affine> listener_transform;
    u64 listener_entity = 0;
    world.query<const AudioListener, const world::WorldTransform>().each(
        [&](world::Entity entity, const AudioListener& listener,
            const world::WorldTransform& transform) {
            if (listener_entity == 0 || entity.id() < listener_entity) {
                listener_entity = entity.id();
                listener_transform = transform.value;
                listener_volume_ = std::max(listener.volume, 0.0f);
            }
        });
    if (!listener_transform) {
        listener_transform = fallback_;
        listener_volume_ = 1.0f;
    }
    if (listener_transform) {
        const math::Vec3 velocity = listener_motion_.update(listener_transform->translation, dt);
        listener_ = listener_from(*listener_transform, velocity);
    }

    // Quellen ohne Lage (z. B. Wind ohne Transform) spielen am Hörer; ohne Ortung ist die Lage
    // ohnehin egal.
    world.query<const AudioSource>().each([&](world::Entity entity, const AudioSource& source) {
        Source& state = sources_[entity.id()];
        state.seen = frame_;
        const auto* transform = entity.get<world::WorldTransform>();
        const math::DVec3 position =
            transform != nullptr ? transform->position() : listener_.position;
        const math::Vec3 velocity =
            transform != nullptr ? state.motion.update(position, dt) : listener_.velocity;

        if (state.sound != source.sound) {
            // Anderer Klang: neu beginnen, wie beim ersten Erscheinen.
            engine_->stop(state.voice);
            state.voice = {};
            state.sound = source.sound;
            state.started = false;
        }
        if (state.voice.valid() && !engine_->playing(state.voice)) {
            state.voice = {};
        }
        if (state.voice.valid() && state.playing_sound != 0) {
            const auto loaded = sounds_.find(state.playing_sound);
            if (loaded != sounds_.end() && loaded->second.get() != nullptr &&
                loaded->second.version() != state.version) {
                // Hot Reload: mit dem neuen Klang neu beginnen.
                state.requested = true;
            }
        }

        const EmitterPose pose{.position = position,
                               .velocity = velocity,
                               .min_distance = source.min_distance,
                               .max_distance = source.max_distance,
                               .rolloff = source.rolloff,
                               .rolloff_factor = source.rolloff_factor,
                               .doppler = source.doppler};
        const bool start = state.requested || (!state.started && source.play_on_start);
        if (start && source.sound != 0) {
            const Choice choice = choose(source.sound);
            if (choice.pending) {
                // Noch nicht geladen: im nächsten Bild wieder versuchen.
                state.requested = true;
                ++pending_;
                return;
            }
            state.requested = false;
            state.started = true;
            if (choice.sound == nullptr) {
                return;
            }
            engine_->stop(state.voice);
            state.playing_sound = choice.id;
            state.version = choice.version;
            state.variation_volume = choice.volume;
            state.variation_pitch = choice.pitch;
            state.voice = engine_->play(
                *choice.sound, PlayParams{.bus = source.bus,
                                          .loop = source.loop,
                                          .spatial = source.spatial,
                                          .mix = mix_for(pose, source.spatial,
                                                         source.volume * state.variation_volume,
                                                         source.pitch * state.variation_pitch)});
            return;
        }
        if (state.voice.valid()) {
            engine_->set(state.voice,
                         mix_for(pose, source.spatial, source.volume * state.variation_volume,
                                 source.pitch * state.variation_pitch));
        }
    });

    // Quellen, die verschwunden sind (Entity zerstört oder Komponente entfernt).
    std::erase_if(sources_, [&](auto& entry) {
        if (entry.second.seen == frame_) {
            return false;
        }
        engine_->stop(entry.second.voice);
        return true;
    });

    update_one_shots();
}

void AudioSystem::update_one_shots() {
    for (OneShot& shot : one_shots_) {
        const EmitterPose pose{.position = shot.position.value_or(listener_.position),
                               .min_distance = shot.params.min_distance,
                               .max_distance = shot.params.max_distance,
                               .rolloff = shot.params.rolloff,
                               .rolloff_factor = shot.params.rolloff_factor,
                               .doppler = 0.0f};
        const bool spatial = shot.position.has_value();
        if (shot.pending) {
            const Choice choice = choose(shot.sound);
            if (choice.pending && ++shot.waited < max_one_shot_wait) {
                ++pending_;
                continue;
            }
            shot.pending = false;
            if (choice.sound == nullptr) {
                continue;
            }
            shot.variation_volume = choice.volume;
            shot.variation_pitch = choice.pitch;
            shot.voice = engine_->play(
                *choice.sound,
                PlayParams{.bus = shot.params.bus,
                           .spatial = spatial,
                           .mix = mix_for(pose, spatial, shot.params.volume * choice.volume,
                                          shot.params.pitch * choice.pitch)});
            continue;
        }
        engine_->set(shot.voice, mix_for(pose, spatial, shot.params.volume * shot.variation_volume,
                                         shot.params.pitch * shot.variation_pitch));
    }
    std::erase_if(one_shots_, [&](const OneShot& shot) {
        return !shot.pending && !engine_->playing(shot.voice);
    });
}

void AudioSystem::play(world::Entity entity) {
    if (entity) {
        sources_[entity.id()].requested = true;
    }
}

void AudioSystem::stop(world::Entity entity, f32 fade) {
    const auto it = sources_.find(entity.id());
    if (it != sources_.end()) {
        engine_->stop(it->second.voice, fade);
        it->second.voice = {};
        it->second.requested = false;
    }
}

bool AudioSystem::playing(u64 entity) const {
    const auto it = sources_.find(entity);
    return it != sources_.end() && engine_->playing(it->second.voice);
}

bool AudioSystem::play_at(u64 sound, std::optional<math::DVec3> position,
                          const OneShotParams& params) {
    if (sound == 0) {
        return false;
    }
    one_shots_.push_back(OneShot{.sound = sound, .position = position, .params = params});
    // Sofort versuchen, damit ein geladener Klang im selben Bild beginnt.
    update_one_shots();
    return true;
}

void AudioSystem::stop_all() {
    for (auto& [entity, source] : sources_) {
        engine_->stop(source.voice);
    }
    for (const OneShot& shot : one_shots_) {
        engine_->stop(shot.voice);
    }
    sources_.clear();
    one_shots_.clear();
}

AudioSystemStats AudioSystem::stats() const noexcept {
    AudioSystemStats stats{
        .sources = sources_.size(), .one_shots = one_shots_.size(), .pending = pending_};
    for (const auto& [entity, source] : sources_) {
        if (engine_->playing(source.voice)) {
            ++stats.playing;
        }
    }
    return stats;
}

} // namespace ngine::audio
