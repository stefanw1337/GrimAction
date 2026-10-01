#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
namespace gdtpc {
inline float skybox_radius_scale(float far_plane, float mesh_radius, float preferred_radius) noexcept {
    if (!std::isfinite(far_plane) || far_plane <= 1 || !std::isfinite(mesh_radius) || mesh_radius<=0 ||
        !std::isfinite(preferred_radius) || preferred_radius<=0) return 0;
    return std::min(preferred_radius,far_plane*0.8F)/mesh_radius;
}

inline bool northshire_sky_record(const char* text, std::uint64_t length) noexcept {
    constexpr char expected[] = "records/northshire/environment/day_sky.dbr";
    if (text == nullptr || length != sizeof(expected)-1) return false;
    for (std::uint64_t i=0; i<length; ++i) {
        char c=text[i];
        if (c >= 'A' && c <= 'Z') c=static_cast<char>(c-'A'+'a');
        if (c == '\\') c='/';
        if (c != expected[i]) return false;
    }
    return true;
}

inline bool skybox_fallback_allowed(bool active, bool third_person, bool main_scene,
    bool current_region, bool authored_sky, std::uint64_t now, std::uint64_t tick) noexcept {
    return active && third_person && main_scene && current_region && !authored_sky &&
        tick != 0 && now >= tick && now - tick <= 250;
}
}
