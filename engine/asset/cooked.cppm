module;

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module ngine.asset:cooked;

import ngine.core;
import :image;
import :model;
import :database;

// Gekochte Assets (ASSET-03, ADR 0036): Laufzeitformate, die sich ohne Dekodieren und Rechnen
// laden lassen. Ein Modell wird zu einer Binärdatei mit Eckpunkten und Indizes so, wie der Renderer
// sie hochlädt, und seinen Texturen als KTX2 mit Basis Universal. Gekochte Dateien liegen in einem
// Cache-Ordner; jede kennt die Quelldateien, aus denen sie entstand, und gilt nur, solange diese
// unverändert sind. Kochen selbst (Basis-Kodierung, MikkTSpace) steckt in ngine.cook, das nur
// Editor, Werkzeug und Tests linken; hier liegt, was auch Spiel und Server zum Lesen brauchen.
export namespace ngine::asset {

// Format einer Textur, wie sie auf die GPU geht. BC7 nur, wo die GPU es kann; sonst RGBA8.
enum class TextureFormat : u8 { RGBA8, BC7 };

// Textur mit allen Mip-Stufen hintereinander (Stufe 0 zuerst, Zeilen bzw. Blockzeilen ohne
// Abstand), fertig zum Hochladen.
struct TextureData {
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = false;
    u32 width = 0;
    u32 height = 0;
    u32 levels = 1;
    std::vector<std::byte> data;
};

// Bytes aller Stufen einer Textur in diesem Format.
[[nodiscard]] u64 texture_size(TextureFormat format, u32 width, u32 height, u32 levels) noexcept;

// RGBA8 aus einem Bild samt Mip-Stufen auf der CPU (mip_chain()); ohne mipmaps nur Stufe 0.
[[nodiscard]] TextureData texture_from_image(const Image& image, bool srgb, bool mipmaps);

// Liest eine KTX2-Datei (auch mit Basis Universal und zstd) aus dem Speicher. Basis-Daten werden in
// target umgewandelt; unkomprimierte RGBA8-Daten bleiben RGBA8. Adapter für KTX-Software.
[[nodiscard]] Result<TextureData> decode_ktx2(std::span<const std::byte> data,
                                              TextureFormat target);

// Eine Datei, aus der ein gekochtes Asset entstand, mit Größe, Änderungszeit und Prüfsumme.
struct SourceStamp {
    // Absolut und normalisiert.
    std::string path;
    u64 size = 0;
    i64 modified = 0;
    u64 hash = 0;

    friend bool operator==(const SourceStamp&, const SourceStamp&) = default;
};

// FNV-1a über den Inhalt; schnell genug für Assets, kein Schutz gegen Absicht.
[[nodiscard]] u64 content_hash(std::span<const std::byte> data) noexcept;

// Stempel einer Datei, wie sie jetzt auf der Platte liegt.
[[nodiscard]] Result<SourceStamp> stamp_file(std::string_view path);

// Ob alle Dateien noch so sind wie gestempelt. Stimmen Größe und Änderungszeit, gilt die Datei ohne
// Lesen als gleich; sonst entscheidet die Prüfsumme (z. B. nach git checkout). refreshed sagt, ob
// sich dabei nur Änderungszeiten geändert haben; stamps enthält dann die neuen.
[[nodiscard]] bool stamps_current(std::vector<SourceStamp>& stamps, bool* refreshed = nullptr);

// Eine Textur eines gekochten Modells: Bild und Farbraum wie in Model::images bzw. dem Material,
// das sie benutzt, die KTX2-Datei und dieselbe Textur schon als BC7 umgewandelt. Desktop-GPUs
// können BC7 alle; sie laden die fertigen Blöcke ohne Umwandeln. Nur ohne BC7 wird beim Laden aus
// der KTX2-Datei nach RGBA8 umgewandelt. bc7.data ist leer, wenn es keine BC7-Fassung gibt.
struct CookedTexture {
    u32 image = 0;
    bool srgb = false;
    std::vector<std::byte> ktx2;
    TextureData bc7;
};

// Ein gekochtes Modell: Model mit Eckpunkten und Indizes, aber Bildern ohne Pixel; die Bilder
// stehen als KTX2 in textures, je Bild und Farbraum einmal.
struct CookedModel {
    Model model;
    std::vector<CookedTexture> textures;
    std::vector<SourceStamp> sources;
};

// Version des Formats; ändert sich, wenn sich das Format oder das Kochen ändert. Ältere Dateien im
// Cache gelten dann als veraltet.
// 2: Meshlets je Teilnetz (R3.5).
inline constexpr u32 cooked_model_version = 2;

[[nodiscard]] std::vector<std::byte> write_cooked_model(const CookedModel& model);
// Liest ein gekochtes Modell; kaputte Daten und andere Versionen liefern einen Fehler.
[[nodiscard]] Result<CookedModel> read_cooked_model(std::span<const std::byte> data);

// Eine gekochte Einzeltextur (R4.3, z. B. die Texturen des Terrains): KTX2 und BC7 wie bei
// Modellen, dazu der Stempel der Bilddatei. texture.image ist 0.
struct CookedImage {
    CookedTexture texture;
    std::vector<SourceStamp> sources;
};

inline constexpr u32 cooked_image_version = 1;

[[nodiscard]] std::vector<std::byte> write_cooked_image(const CookedImage& image);
[[nodiscard]] Result<CookedImage> read_cooked_image(std::span<const std::byte> data);

// Kocht eine Bilddatei (absolut, normalisiert) im Farbraum srgb mit Mip-Stufen. Kommt von
// ngine.cook.
using ImageCooker = std::function<Result<CookedImage>(std::string_view source, bool srgb)>;

// Kocht ein Modell aus seiner Quelldatei (absolut, normalisiert). Kommt von ngine.cook; der
// Ressourcen-Manager ruft es im Hintergrund auf.
using ModelCooker = std::function<Result<CookedModel>(std::string_view source)>;

// Ordner mit gekochten Assets, eine Datei pro Kennung (<Kennung hex>.nmodel, Texturen .ntex). Lesen
// und Schreiben verschiedener Kennungen darf parallel laufen.
class AssetCache {
public:
    AssetCache() = default;
    explicit AssetCache(std::string directory);

    [[nodiscard]] const std::string& directory() const noexcept {
        return directory_;
    }
    [[nodiscard]] bool enabled() const noexcept {
        return !directory_.empty();
    }
    [[nodiscard]] std::string model_path(AssetId id) const;

    // Gekochtes Modell, wenn es eins gibt und seine Quellen unverändert sind; sonst nichts. Ein
    // Fehler heißt: Datei da, aber nicht lesbar.
    [[nodiscard]] Result<std::optional<CookedModel>> load_model(AssetId id) const;
    // Nur prüfen, ob ein aktuelles gekochtes Modell da ist.
    [[nodiscard]] bool model_current(AssetId id) const;
    // Schreibt das Modell (über eine temporäre Datei, damit ein paralleles Lesen nie eine halbe
    // Datei sieht) und legt den Ordner bei Bedarf an. Dieselbe Kennung nicht parallel schreiben.
    [[nodiscard]] Result<void> store_model(AssetId id, const CookedModel& model) const;

    // Einzeltexturen (R4.3), eine Datei je Kennung und Farbraum (<Kennung hex>.srgb.ntex bzw.
    // .linear.ntex); sonst wie die Modelle.
    [[nodiscard]] std::string texture_path(AssetId id, bool srgb) const;
    [[nodiscard]] Result<std::optional<CookedImage>> load_texture(AssetId id, bool srgb) const;
    [[nodiscard]] Result<void> store_texture(AssetId id, bool srgb, const CookedImage& image) const;

private:
    std::string directory_;
};

// Cache-Ordner zu einem Asset-Ordner: „.cache“ darin. Versteckt, damit Datenbank und Beobachter
// ihn übergehen; steht in .gitignore.
[[nodiscard]] std::string default_cache_dir(std::string_view asset_root);

} // namespace ngine::asset
