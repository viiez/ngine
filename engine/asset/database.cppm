module;

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module ngine.asset:database;

import ngine.core;
import :image;

// Stabile Kennungen für Quelldateien (ASSET-05, ADR 0033). Jede Datei bekommt eine zufällige
// 64-Bit-Kennung, die in einer .meta-Datei daneben steht (kiste.gltf → kiste.gltf.meta). Die
// Kennung folgt der Datei, wenn man beide zusammen verschiebt oder umbenennt; Szenen verweisen
// nur über sie auf Assets, nie über Pfade.
export namespace ngine::asset {

// Kennung eines Assets; 0 heißt: keins.
using AssetId = u64;

// Verweis auf einen Teil eines Assets, z. B. das dritte Mesh einer glTF-Datei. index zählt in
// der Reihenfolge der Quelldatei; wer dort umsortiert, verschiebt die Verweise.
struct AssetRef {
    AssetId asset = 0;
    u32 index = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return asset != 0;
    }
    friend constexpr bool operator==(const AssetRef&, const AssetRef&) = default;
};

// Feste Kennung für Assets, die ein Programm selbst erzeugt (Meshes und Materialien der
// Standardszene): FNV-1a-Hash von „generated:<name>“. Gleicher Name, gleiche Kennung über
// Programmstarts hinweg, deshalb lassen sich Szenen damit speichern.
[[nodiscard]] constexpr AssetId generated_id(std::string_view name) noexcept {
    u64 hash = 14695981039346656037ull;
    for (const char c : std::string_view{"generated:"}) {
        hash ^= static_cast<u8>(c);
        hash *= 1099511628211ull;
    }
    for (const char c : name) {
        hash ^= static_cast<u8>(c);
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

// Neue zufällige Kennung (nie 0).
[[nodiscard]] AssetId random_id();

enum class AssetType : u8 { Unknown, Model, Texture, Shader, Scene, Prefab, Input };

// Art nach der Endung: .gltf/.glb Modell, .png/.jpg/.jpeg/.tga/.bmp Textur, .slang Shader,
// .json und .nscene Szene, .prefab Prefab (R2.5), .input Belegung der Eingabe (R4.1). Dateien
// unbekannter Art bekommen keine Kennung.
[[nodiscard]] AssetType type_of(std::string_view path);
[[nodiscard]] std::string_view to_string(AssetType type);

inline constexpr std::string_view meta_extension = ".meta";
inline constexpr std::string_view meta_format = "ngine.meta";
inline constexpr i64 meta_version = 1;

// Inhalt einer .meta-Datei als Text und zurück.
[[nodiscard]] std::string write_meta(AssetId id);
[[nodiscard]] Result<AssetId> parse_meta(std::string_view text);

struct AssetInfo {
    AssetId id = 0;
    AssetType type = AssetType::Unknown;
    // Relativ zur Wurzel, mit „/“ als Trenner (für Anzeige und Sortierung).
    std::string path;
    // Absolut und normalisiert (filesystem::normalize()), zum Laden und für den Beobachter.
    std::string file;
};

// Was refresh() gefunden hat.
struct DatabaseChanges {
    usize added = 0;
    usize removed = 0;
    // Davon neu angelegte .meta-Dateien.
    usize created_meta = 0;
};

// Alle Assets unter einem Ordner (Asset-Wurzel). Fehlt einer Datei die .meta-Datei, legt die
// Datenbank sie mit einer neuen Kennung an; doppelte Kennungen (kopierte .meta-Dateien) bekommen
// eine neue. .meta-Dateien ohne Datei bleiben liegen und werden ignoriert, damit ein kurz
// verschobenes Asset seine Kennung behält. Nur im Hauptthread benutzen.
class AssetDatabase {
public:
    AssetDatabase() = default;

    // Durchsucht root samt Unterordnern.
    [[nodiscard]] static Result<AssetDatabase> open(std::string_view root);
    // Durchsucht die Wurzel neu (nach Änderungen auf der Platte).
    Result<DatabaseChanges> refresh();

    [[nodiscard]] const AssetInfo* find(AssetId id) const;
    // path relativ zur Wurzel oder absolut.
    [[nodiscard]] const AssetInfo* find_path(std::string_view path) const;
    // Alle Assets, sortiert nach Pfad.
    [[nodiscard]] std::span<const AssetInfo> assets() const noexcept {
        return assets_;
    }
    [[nodiscard]] const std::string& root() const noexcept {
        return root_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return assets_.empty();
    }

private:
    void rebuild_index();

    std::string root_;
    std::vector<AssetInfo> assets_;
    std::unordered_map<AssetId, usize> by_id_;
    std::unordered_map<std::string, usize> by_file_;
};

} // namespace ngine::asset
