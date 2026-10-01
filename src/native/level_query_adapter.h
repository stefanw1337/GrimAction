#pragma once

// The live half of camera collision: issues one GAME::Level::GetIntersection against the camera's
// current region and reports the hit distance to the offline spring-arm model.
//
// This is the first code in this project that calls a game function from inside a hooked game
// function. The re-entrancy policy recorded in IMPLEMENTATION_PLAN.md governs it in full:
//  - the caller must invoke this only after the original UpdateFromInputImpl has returned;
//  - the level pointer is NEVER cached, it is re-fetched through both accessors on every query;
//  - every call is wrapped in a narrow structured-exception guard;
//  - a refused query is a fault for the model to count, never a silent zero.
//
// Thread affinity was live-confirmed: the camera callback owner is the game's own primary thread,
// which is the thread that mutates level geometry, so a torn read is not a hazard here.

#include "camera_collision_model.h"

#include <cstddef>
#include <cstdint>

namespace gdtpc
{
// Entry points resolved and validated once during initialization, so nothing is ever resolved on
// the camera thread. On x64 every calling convention collapses to the same ABI; the annotations are
// documentation. Member functions take `this` in the first integer register.
struct LevelQueryEntries
{
    void* (__fastcall* get_region)(const void* world_camera){};
    // The inspected MSVC x64 body receives this in RCX, its 24-byte hidden result buffer in RDX,
    // and the explicit WorldVec3 input in R8: this, result, input.
    void* (__fastcall* calculate_view_position)(const void* world_camera, void* result,
        const void* focus){};
    void* (__fastcall* get_level_ptr)(const void* region){};
    void (__fastcall* get_intersection)(void* level, const void* ray, void* intersection,
        std::uint32_t surface, void** hit_entity, float max_distance, const void* ignore_entity,
        bool include_entities){};

    [[nodiscard]] bool complete() const noexcept
    {
        return get_region != nullptr && calculate_view_position != nullptr &&
            get_level_ptr != nullptr && get_intersection != nullptr;
    }
};

// Conservative default until the enum is recovered: ask for the widest surface set rather than
// guess a specific one, and let the skin and minimum distance keep the result sane.
constexpr std::uint32_t default_physics_surface = 0;

// The engine writes at least 0x14 bytes of GAME::Intersection on the inspected path. Nothing proves
// that is the whole structure, so the out buffer is deliberately oversized and zeroed.
constexpr std::size_t intersection_buffer_size = 128;

class WindowsLevelQuery final : public LevelQuery, public CameraLevelQuery
{
public:
    WindowsLevelQuery(const LevelQueryEntries& entries, void* camera, const void* ignore_entity,
        std::uint32_t surface = default_physics_surface,
        CollisionVec3 camera_translation = {}) noexcept;

    // Returns false when the query could not be performed at all, which the model counts as a
    // fault. Returns true with +infinity for a clean miss, matching the engine's own sentinel.
    [[nodiscard]] bool raycast(const CollisionRay& ray, float max_distance, float& distance) noexcept override;
    [[nodiscard]] bool raycast_from_camera(float max_distance, float& distance,
        CollisionRay& ray) noexcept override;

    void set_clearance(float radius) noexcept { clearance_ = radius; }

    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

    // Virtual zoom: the camera position +0x94 was computed by the previous frame's update from that frame's
    // distance, yaw, pitch and eye pull. With the engine held ~85 units out, subtracting THIS frame's
    // calculated offset instead misplaces the focus by the distance times the rotation since then (live
    // telemetry: p90 0.68 units against 0.11 with the recorded displacement). When set, the origin is
    // position + translation - displacement; the direction still comes from CalculateViewPosition.
    void set_camera_displacement(const CollisionVec3& displacement) noexcept
    {
        camera_displacement_ = displacement;
        has_camera_displacement_ = true;
    }

private:
    const LevelQueryEntries* entries_;
    void* camera_;
    const void* ignore_entity_;
    std::uint32_t surface_;
    CollisionVec3 camera_translation_{};
    CollisionVec3 camera_displacement_{};
    bool has_camera_displacement_{};
    std::uint64_t refused_{};
    float clearance_{};
};
}
