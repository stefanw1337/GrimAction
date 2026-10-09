#include "level_query_adapter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// Mirrors GAME::Ray exactly: origin then a normalized direction, 24 bytes, no padding.
struct EngineRay
{
    float origin[3];
    float direction[3];
};
static_assert(sizeof(EngineRay) == 24);

struct WorldVec3
{
    void* region;
    float x;
    float y;
    float z;
};
static_assert(sizeof(WorldVec3) == 24);
static_assert(offsetof(WorldVec3, x) == 0x08 && offsetof(WorldVec3, y) == 0x0c &&
    offsetof(WorldVec3, z) == 0x10);

// Every one of these helpers keeps only POD locals so the structured-exception guard is legal and
// so nothing needs unwinding if the engine faults underneath us.

void* guarded_call_pointer(void* (__fastcall* entry)(const void*), const void* argument) noexcept
{
    __try
    {
        return entry(argument);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

bool guarded_intersection(const gdtpc::LevelQueryEntries& entries, void* level, const EngineRay& ray,
    void* out, const std::uint32_t surface, const float max_distance, const void* ignore_entity) noexcept
{
    __try
    {
        void* hit_entity = nullptr;
        entries.get_intersection(level, &ray, out, surface, &hit_entity, max_distance, ignore_entity, false);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool guarded_calculate_view_position(const gdtpc::LevelQueryEntries& entries, const void* camera,
    const WorldVec3& input, WorldVec3& output) noexcept
{
    __try
    {
        entries.calculate_view_position(camera, &output, &input);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool guarded_camera_position(const void* camera, gdtpc::CollisionVec3& position) noexcept
{
    __try
    {
        const auto bytes = static_cast<const unsigned char*>(camera);
        std::memcpy(&position.x, bytes + 0x94, sizeof(float));
        std::memcpy(&position.y, bytes + 0x98, sizeof(float));
        std::memcpy(&position.z, bytes + 0x9c, sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool finite_vec(const gdtpc::CollisionVec3& value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
}

gdtpc::WindowsLevelQuery::WindowsLevelQuery(const LevelQueryEntries& entries, void* camera,
    const void* ignore_entity, const std::uint32_t surface, const CollisionVec3 camera_translation) noexcept
    : entries_{&entries}, camera_{camera}, ignore_entity_{ignore_entity}, surface_{surface},
      camera_translation_{camera_translation}
{
}

bool gdtpc::WindowsLevelQuery::raycast(const CollisionRay& ray, const float max_distance, float& distance) noexcept
{
    distance = 0.0F;
    if (entries_ == nullptr || !entries_->complete() || camera_ == nullptr ||
        !std::isfinite(max_distance) || max_distance <= 0.0F)
    {
        ++refused_;
        return false;
    }

    // Refuse to hand the engine anything nonfinite. A NaN origin in a physics raycast is a crash,
    // not a rejected sample, and no amount of checking the result would catch it.
    const float components[6]{ray.origin.x, ray.origin.y, ray.origin.z,
        ray.direction.x, ray.direction.y, ray.direction.z};
    for (const auto value : components)
    {
        if (!std::isfinite(value))
        {
            ++refused_;
            return false;
        }
    }

    // The level pointer is deliberately re-fetched every query. A level can unload while the
    // camera, engine and player pointers all stay stable, so a cached one could outlive its world.
    const auto region = guarded_call_pointer(entries_->get_region, camera_);
    if (region == nullptr)
    {
        ++refused_;
        return false;
    }
    const auto level = guarded_call_pointer(entries_->get_level_ptr, region);
    if (level == nullptr)
    {
        ++refused_;
        return false;
    }

    EngineRay engine_ray{};
    engine_ray.origin[0] = ray.origin.x;
    engine_ray.origin[1] = ray.origin.y;
    engine_ray.origin[2] = ray.origin.z;
    engine_ray.direction[0] = ray.direction.x;
    engine_ray.direction[1] = ray.direction.y;
    engine_ray.direction[2] = ray.direction.z;

    alignas(16) unsigned char intersection[intersection_buffer_size]{};
    if (!guarded_intersection(*entries_, level, engine_ray, intersection, surface_, max_distance, ignore_entity_))
    {
        ++refused_;
        return false;
    }

    // GAME::Intersection keeps the hit parameter at offset 0. A miss is +infinity, which the model
    // reads as "nothing in the way"; anything else nonfinite is invalid and the model counts it.
    float hit = 0.0F;
    std::memcpy(&hit, intersection, sizeof(hit));
    distance = hit;
    return true;
}

bool gdtpc::WindowsLevelQuery::raycast_from_camera(const float max_distance, float& distance,
    CollisionRay& ray) noexcept
{
    distance = 0.0F;
    ray = {};
    if (entries_ == nullptr || !entries_->complete() || camera_ == nullptr ||
        !std::isfinite(max_distance) || max_distance <= 0.0F)
    {
        ++refused_;
        return false;
    }

    // One query uses the complete validated chain. The region and level are local to this call;
    // neither survives into the next callback.
    const auto region = guarded_call_pointer(entries_->get_region, camera_);
    if (region == nullptr)
    {
        ++refused_;
        return false;
    }

    const WorldVec3 zero{region, 0.0F, 0.0F, 0.0F};
    WorldVec3 offset{};
    CollisionVec3 camera_position{};
    if (!guarded_calculate_view_position(*entries_, camera_, zero, offset) ||
        !guarded_camera_position(camera_, camera_position))
    {
        ++refused_;
        return false;
    }

    const CollisionVec3 toward{offset.x, offset.y, offset.z};
    camera_position.x += camera_translation_.x;
    camera_position.y += camera_translation_.y;
    camera_position.z += camera_translation_.z;
    if (!finite_vec(camera_position) || !finite_vec(toward))
    {
        ++refused_;
        return false;
    }
    const auto length_squared = toward.x * toward.x + toward.y * toward.y + toward.z * toward.z;
    if (!std::isfinite(length_squared) || length_squared < 1.0e-8F)
    {
        ++refused_;
        return false;
    }
    const auto inverse_length = 1.0F / std::sqrt(length_squared);
    ray.direction = {toward.x * inverse_length, toward.y * inverse_length, toward.z * inverse_length};
    const auto displacement = has_camera_displacement_ ? camera_displacement_ : toward;
    if (!finite_vec(displacement))
    {
        ++refused_;
        return false;
    }
    ray.origin = {camera_position.x - displacement.x, camera_position.y - displacement.y,
        camera_position.z - displacement.z};
    if (!finite_vec(ray.origin) || !finite_vec(ray.direction))
    {
        ++refused_;
        return false;
    }

    const auto level = guarded_call_pointer(entries_->get_level_ptr, region);
    if (level == nullptr)
    {
        ++refused_;
        return false;
    }

    EngineRay engine_ray{{ray.origin.x, ray.origin.y, ray.origin.z},
        {ray.direction.x, ray.direction.y, ray.direction.z}};
    alignas(16) unsigned char intersection[intersection_buffer_size]{};
    if (!guarded_intersection(*entries_, level, engine_ray, intersection, surface_, max_distance, ignore_entity_))
    {
        ++refused_;
        return false;
    }
    std::memcpy(&distance, intersection, sizeof(distance));
    // A center ray alone leaves almost no vertical clearance at grazing angles:
    // skin 0.35 along a six-degree ray is only 0.037 above the ground.
    // Sample a small camera envelope, retaining the closest valid obstruction.
    if (!std::isfinite(clearance_) || clearance_<0 || clearance_>2) { ++refused_; return false; }
    if (clearance_>0) {
        const float horizontal=std::sqrt(ray.direction.x*ray.direction.x+ray.direction.z*ray.direction.z);
        const float side_x=horizontal>0.0001F ? ray.direction.z/horizontal : 1.0F;
        const float side_z=horizontal>0.0001F ? -ray.direction.x/horizontal : 0.0F;
        const CollisionVec3 offsets[4]{{0,-clearance_,0},{0,clearance_,0},
            {side_x*clearance_,0,side_z*clearance_},{-side_x*clearance_,0,-side_z*clearance_}};
        for (const auto offset_value : offsets) {
            EngineRay probe=engine_ray;
            probe.origin[0]+=offset_value.x; probe.origin[1]+=offset_value.y; probe.origin[2]+=offset_value.z;
            if (!guarded_intersection(*entries_,level,probe,intersection,surface_,max_distance,ignore_entity_)) {
                ++refused_; return false;
            }
            float hit{}; std::memcpy(&hit,intersection,sizeof(hit));
            if (std::isnan(hit) || hit<0) { ++refused_; return false; }
            if (hit<distance) distance=hit;
        }
    }
    if (clearance_ > 0.0F) {
        // Parallel rays starting below a one-sided terrain surface report misses.
        // Probe downward from the focus height along the candidate arm as well.
        // Stop at the last clear sample, rather than allow the eye below the floor.
        const float candidate = std::min(distance, max_distance);
        if (!std::isfinite(candidate) || candidate < 0.0F) { ++refused_; return false; }
        constexpr int samples = 12;
        for (int i = 1; i <= samples; ++i) {
            const float arm = candidate * static_cast<float>(i) / samples;
            const CollisionVec3 eye{ray.origin.x + ray.direction.x * arm,
                ray.origin.y + ray.direction.y * arm, ray.origin.z + ray.direction.z * arm};
            const float top = std::max(ray.origin.y, eye.y) + clearance_ + 0.25F;
            EngineRay down{{eye.x, top, eye.z}, {0.0F, -1.0F, 0.0F}};
            const float depth = top - eye.y + clearance_;
            if (!guarded_intersection(*entries_, level, down, intersection, surface_, depth, ignore_entity_)) {
                ++refused_; return false;
            }
            float hit{}; std::memcpy(&hit, intersection, sizeof(hit));
            if (std::isnan(hit) || hit < 0.0F) { ++refused_; return false; }
            if (std::isfinite(hit) && top - hit > eye.y - clearance_ + 0.001F) {
                distance = std::min(distance, candidate * static_cast<float>(i - 1) / samples);
                break;
            }
        }
    }
    return true;
}
