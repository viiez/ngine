// Makros für Profiling-Zonen. Module können keine Makros exportieren, deshalb liegen sie in
// diesem Header (ADR 0016). Er enthält nur Makros und darf im globalen Modulfragment stehen;
// wer ihn nutzt, braucht zusätzlich `import ngine.profile;`.
//
//   void update() {
//       NGINE_PROFILE_ZONE("update");
//       ...
//   }
#pragma once

#define NGINE_PROFILE_CONCAT_IMPL(a, b) a##b
#define NGINE_PROFILE_CONCAT(a, b) NGINE_PROFILE_CONCAT_IMPL(a, b)

#if NGINE_WITH_TRACY

// Zone mit Namen und Farbe (0xRRGGBB) bis zum Ende des Blocks.
#define NGINE_PROFILE_ZONE_COLOR(zone_name, zone_color)                                            \
    static const ::ngine::profile::SourceLocation NGINE_PROFILE_CONCAT(                            \
        ngine_profile_location_, __LINE__){zone_name, __func__, __FILE__,                          \
                                           static_cast<::ngine::u32>(__LINE__), zone_color};       \
    const ::ngine::profile::Zone NGINE_PROFILE_CONCAT(ngine_profile_zone_, __LINE__) {             \
        NGINE_PROFILE_CONCAT(ngine_profile_location_, __LINE__)                                    \
    }

#else

#define NGINE_PROFILE_ZONE_COLOR(zone_name, zone_color) static_cast<void>(0)

#endif

// Zone mit Namen bis zum Ende des Blocks.
#define NGINE_PROFILE_ZONE(zone_name) NGINE_PROFILE_ZONE_COLOR(zone_name, 0)
