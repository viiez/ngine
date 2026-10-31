module;

#include <bit>
#include <cstdint>

export module ngine.memory:align;

import ngine.core;

export namespace ngine::memory {

[[nodiscard]] constexpr bool is_power_of_two(usize value) noexcept {
    return std::has_single_bit(value);
}

// Rundet value auf das nächste Vielfache von alignment auf (Zweierpotenz).
[[nodiscard]] constexpr usize align_up(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Abstand, um den ein Zeiger verschoben werden muss, damit er ausgerichtet ist.
[[nodiscard]] inline usize alignment_padding(const void* pointer, usize alignment) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return static_cast<usize>(align_up(address, alignment) - address);
}

} // namespace ngine::memory
