module;

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Asset-Pipeline (ASSET-03, ADR 0036): kocht Quelldateien in die Laufzeitformate aus ngine.asset.
// glTF-Modelle werden zu gekochten Modellen mit Tangenten nach MikkTSpace (TD-21), ihre Bilder zu
// KTX2 mit Basis Universal. Nur Editor, Werkzeug (ngine_cook) und Tests linken dieses Modul; Spiel
// und Server lesen nur, was im Cache liegt. KTX-Software steckt in ktx_encode.cpp, MikkTSpace in
// tangents_mikktspace.cpp, meshoptimizer in meshlets_meshoptimizer.cpp.
export module ngine.cook;

import ngine.core;
import ngine.asset;
import ngine.jobs;

export namespace ngine::cook {

template <typename T>
using Result = std::expected<T, std::string>;

// Tangenten nach MikkTSpace, dem Standard, mit dem Werkzeuge Normalenkarten backen. Eckpunkte,
// an denen Dreiecke verschiedene Tangenten brauchen (UV-Nähte, Spiegelungen), werden geteilt;
// Indizes zeigen danach auf die neuen Eckpunkte. Normalen und Texturkoordinaten müssen stimmen.
void generate_tangents(asset::Primitive& primitive);

// Meshlets mit meshoptimizer (R3.5, ADR 0043): ordnet die Dreiecke für den Cache der Eckpunkte
// und teilt sie in Meshlets im Format von ngine.meshlet; indices ist danach umsortiert.
void build_meshlets(asset::Primitive& primitive);

// Wie Bilder zu KTX2 werden.
struct TextureSettings {
    // Basis Universal: UASTC (hohe Qualität, wird BC7 fast ohne Verlust) oder ETC1S (kleiner,
    // sichtbar verlustbehaftet). Ohne basis bleibt RGBA8 unkomprimiert (für Tests).
    bool basis = true;
    bool uastc = true;
    // UASTC: 0 schnellste bis 4 beste Kodierung.
    u32 uastc_level = 1;
    // ETC1S: 1 bis 255.
    u32 etc1s_quality = 128;
    // zstd über UASTC und RGBA8; 0 schaltet es ab.
    u32 zstd_level = 18;
    // Threads der Kodierung; 0 heißt: alle Kerne.
    u32 threads = 0;
};

// Kodiert ein Bild samt Mip-Stufen (asset::mip_chain(), sRGB richtig gemittelt) als KTX2.
[[nodiscard]] Result<std::vector<std::byte>> encode_ktx2(const asset::Image& image, bool srgb,
                                                         bool mipmaps,
                                                         const TextureSettings& settings = {});

struct CookSettings {
    TextureSettings textures;
};

// Kocht ein glTF-Modell: Eckpunkte und Indizes, Tangenten nach MikkTSpace für Teilnetze ohne
// Tangenten in der Datei, jedes benutzte Bild als KTX2 je Farbraum, Stempel aller Quelldateien.
[[nodiscard]] Result<asset::CookedModel> cook_model(std::string_view source,
                                                    const CookSettings& settings = {});

// cook_model() als Funktion für den Ressourcen-Manager bzw. register_loaders().
[[nodiscard]] asset::ModelCooker model_cooker(CookSettings settings = {});

struct CookReport {
    usize cooked = 0;
    // Waren schon aktuell.
    usize current = 0;
    usize failed = 0;
    std::vector<std::string> errors;
    f64 seconds = 0.0;
};

// Kocht alle Modelle der Datenbank, deren gekochte Datei fehlt oder veraltet ist (mit force alle),
// nacheinander; die Kodierung der Bilder nutzt dabei alle Kerne.
[[nodiscard]] CookReport cook_all(const asset::AssetDatabase& database,
                                  const asset::AssetCache& cache, const CookSettings& settings = {},
                                  bool force = false);

// Stand eines Kochens im Hintergrund.
struct CookProgress {
    // Modelle insgesamt und davon fertig (gekocht, aktuell oder fehlgeschlagen).
    usize total = 0;
    usize finished = 0;
    usize failed = 0;
};

// Kocht wie cook_all(), aber im Hintergrund über das Job-System (TD-34): ein Job, der die Modelle
// nacheinander kocht, die Bilder mit höchstens der Hälfte der Kerne, damit Editor und Treiber
// genug Rechenzeit behalten (Erkenntnis aus R3.3). Die Liste der Modelle entsteht beim Start; die
// Datenbank darf sich danach ändern, cache muss das Kochen überleben. Der Destruktor bricht ab und
// wartet auf das Modell, das gerade kocht.
class BackgroundCook {
public:
    BackgroundCook(jobs::JobSystem& jobs, const asset::AssetDatabase& database,
                   const asset::AssetCache& cache, CookSettings settings = {}, bool force = false);
    ~BackgroundCook();
    BackgroundCook(const BackgroundCook&) = delete;
    BackgroundCook& operator=(const BackgroundCook&) = delete;

    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] CookProgress progress() const noexcept;
    // Nach dem Modell, das gerade kocht, ist Schluss.
    void cancel() noexcept;
    // Ergebnis; vollständig erst nach done().
    [[nodiscard]] CookReport report() const;

private:
    // Zustand mit Sperre in cooker.cpp, damit die Schnittstelle kein <mutex> braucht.
    struct State;
    std::shared_ptr<State> state_;
    jobs::JobSystem* jobs_;
};

} // namespace ngine::cook
