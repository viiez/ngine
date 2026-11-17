module;

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

export module ngine.audio:system;

import ngine.core;
import ngine.math;
import ngine.asset;
import ngine.world;
import :types;
import :spatial;
import :sound;
import :engine;
import :components;

// Audio in der Welt (AUD-02): startet Quellen, rechnet pro Bild Abschwächung, Panning und
// Doppler relativ zum Hörer und schickt das Ergebnis an den Mixer. Läuft im Hauptthread nach
// update_transforms() und liest die Welt nur; die Prüfsumme der Simulation ändert es nicht.
export namespace ngine::audio {

// Einmal-Klang ohne Entity (Schritte, Aufprall), z. B. aus C#.
struct OneShotParams {
    AudioBus bus = AudioBus::Effects;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    f32 min_distance = 1.0f;
    f32 max_distance = 30.0f;
    Rolloff rolloff = Rolloff::Inverse;
    f32 rolloff_factor = 1.0f;
};

struct AudioSystemStats {
    // Quellen in der Welt, davon mit laufender Stimme, und laufende Einmal-Klänge.
    usize sources = 0;
    usize playing = 0;
    usize one_shots = 0;
    // Wartet auf das Laden eines Klangs.
    usize pending = 0;
};

class AudioSystem {
public:
    // database unterscheidet Klänge von Klangsätzen (nullptr: alles sind Klänge, z. B. in Tests
    // mit erzeugten Ressourcen). Engine und Manager müssen das System überleben.
    AudioSystem(AudioEngine& engine, asset::ResourceManager& resources,
                const asset::AssetDatabase* database = nullptr);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    void set_database(const asset::AssetDatabase* database) noexcept {
        database_ = database;
    }

    // Lage des Hörers, wenn keine Entity AudioListener trägt (z. B. die Kamera der App).
    void set_fallback_listener(const math::Affine& transform) noexcept {
        fallback_ = transform;
    }

    // Einmal pro Bild nach update_transforms(); dt ist die Bildzeit (für Doppler).
    void update(world::World& world, f64 dt);

    // Spielt die Quelle der Entity (erneut) ab bzw. stoppt sie.
    void play(world::Entity entity);
    void stop(world::Entity entity, f32 fade = 0.05f);
    [[nodiscard]] bool playing(u64 entity) const;

    // Spielt einen Klang oder Klangsatz einmal, an position oder ohne Ortung. Liefert false, wenn
    // es ihn nicht gibt.
    bool play_at(u64 sound, std::optional<math::DVec3> position, const OneShotParams& params = {});

    // Stoppt alles und vergisst die Quellen (z. B. beim Stoppen des Spielmodus).
    void stop_all();

    [[nodiscard]] const ListenerPose& listener() const noexcept {
        return listener_;
    }
    [[nodiscard]] AudioSystemStats stats() const noexcept;

private:
    struct Choice {
        const Sound* sound = nullptr;
        f32 volume = 1.0f;
        f32 pitch = 1.0f;
        bool pending = false;
        u64 id = 0;
        u32 version = 0;
    };
    struct Source {
        Voice voice;
        u64 sound = 0;
        MotionTracker motion;
        // Gespielter Klang und seine Version im Ressourcen-Manager: Lädt er neu (Hot Reload),
        // beginnt die Quelle mit dem neuen Klang.
        u64 playing_sound = 0;
        u32 version = 0;
        bool started = false;
        bool requested = false;
        u64 seen = 0;
        f32 variation_volume = 1.0f;
        f32 variation_pitch = 1.0f;
    };
    struct OneShot {
        Voice voice;
        u64 sound = 0;
        std::optional<math::DVec3> position;
        OneShotParams params;
        f32 variation_volume = 1.0f;
        f32 variation_pitch = 1.0f;
        bool pending = true;
        u32 waited = 0;
    };

    [[nodiscard]] Choice choose(u64 sound);
    [[nodiscard]] f32 random() noexcept;
    [[nodiscard]] VoiceMix mix_for(const EmitterPose& pose, bool spatial, f32 volume,
                                   f32 pitch) const noexcept;
    void update_one_shots();

    AudioEngine* engine_;
    asset::ResourceManager* resources_;
    const asset::AssetDatabase* database_;
    std::unordered_map<u64, asset::Handle<Sound>> sounds_;
    std::unordered_map<u64, asset::Handle<SoundSet>> sets_;
    std::unordered_map<u64, Source> sources_;
    std::vector<OneShot> one_shots_;
    std::optional<math::Affine> fallback_;
    ListenerPose listener_;
    MotionTracker listener_motion_;
    f32 listener_volume_ = 1.0f;
    f64 dt_ = 0.0;
    u64 frame_ = 0;
    u64 random_state_ = 0x9E3779B97F4A7C15ull;
    usize pending_ = 0;
};

} // namespace ngine::audio
