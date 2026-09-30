#pragma once
#include <cstdint>
namespace gdtpc {
inline bool skybox_fallback_allowed(bool active, bool third_person, bool main_scene,
    bool current_region, bool authored_sky, std::uint64_t now, std::uint64_t tick) noexcept {
    return active && third_person && main_scene && current_region && !authored_sky &&
        tick != 0 && now >= tick && now - tick <= 250;
}
}
