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

// Stufen der vollen Mip-Kette: jede Stufe halb so groß (abgerundet, mindestens 1) bis 1x1.
[[nodiscard]] u32 mip_count(u32 width, u32 height) noexcept;

// Alle Mip-Stufen des Bildes hintereinander, Stufe 0 zuerst, jede Zeile ohne Abstand (TD-17).
// Jede Stufe ist das 2x2-Mittel der vorigen; mit srgb wird in linearen Werten gemittelt, Alpha
// immer linear. levels 0 meint die volle Kette. Liefert nichts bei einem leeren Bild.
[[nodiscard]] std::vector<std::byte> mip_chain(const Image& image, bool srgb, u32 levels = 0);

} // namespace ngine::asset
