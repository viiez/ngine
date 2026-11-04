module;

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

export module ngine.asset:image;

import ngine.core;

export namespace ngine::asset {

template <typename T>
using Result = std::expected<T, std::string>;

// Dekodiertes Bild, immer 8 Bit RGBA, Zeilen von oben nach unten ohne Abstand dazwischen. Ob die
// Werte sRGB oder linear sind, entscheidet der Verwender (Grundfarbe sRGB, Normalen linear).
struct Image {
    std::string name;
    u32 width = 0;
    u32 height = 0;
    std::vector<std::byte> pixels;
};

// Dekodiert PNG, JPEG und die anderen Formate von stb_image aus dem Speicher.
[[nodiscard]] Result<Image> decode_image(std::span<const std::byte> data);

// Lädt ein Bild von der Platte; der Pfad ist UTF-8.
[[nodiscard]] Result<Image> load_image(std::string_view path);

} // namespace ngine::asset
