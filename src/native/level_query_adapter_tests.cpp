// Offline fault coverage for the live level-query adapter. No game and no engine: the entry points
// are fakes, which lets every failure mode be exercised deliberately, including a real access
// violation raised inside a fake entry point to prove the structured-exception guard catches it
// rather than taking the process down.

#include "level_query_adapter.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void require(const bool value, const char* message) { if (!value) throw std::runtime_error(message); }

constexpr float infinity = std::numeric_limits<float>::infinity();
constexpr float quiet_nan = std::numeric_limits<float>::quiet_NaN();

// Fake objects. Only their addresses matter.
int fake_region_object = 0;
int fake_level_object = 0;
alignas(16) unsigned char fake_camera_object[0x200]{};

// Observations recorded by the fakes so the test can assert what the engine would have been told.
struct Observed
{
    int region_calls;
    int calculate_calls;
    const void* calculate_camera;
    void* calculate_output;
    int level_calls;
    int intersection_calls;
    float origin[3];
    float direction[3];
    float max_distance;
    std::uint32_t surface;
    const void* ignore_entity;
    const void* level;
    bool include_entities;
};
Observed observed{};

float scripted_distance = infinity;
bool region_returns_null = false;
bool level_returns_null = false;
bool region_faults = false;
bool intersection_faults = false;
bool calculate_faults = false;
bool ground_plane = false;
gdtpc::CollisionVec3 scripted_offset{0.0F, 0.0F, 42.0F};
// Bytes the fake writes past the documented 0x14, to prove the out buffer is generously sized.
std::size_t intersection_write_size = 0x14;

void* __fastcall fake_get_region(const void*) noexcept
{
    ++observed.region_calls;
    if (region_faults)
    {
        // A genuine access violation, raised where the engine would raise one.
        volatile int* poison = nullptr;
        return reinterpret_cast<void*>(static_cast<std::intptr_t>(*poison));
    }
    return region_returns_null ? nullptr : &fake_region_object;
}

void* __fastcall fake_get_level(const void*) noexcept
{
    ++observed.level_calls;
    return level_returns_null ? nullptr : &fake_level_object;
}

struct FakeWorldVec3 { void* region; float x, y, z; };
static_assert(sizeof(FakeWorldVec3) == 24);

void* __fastcall fake_calculate_view_position(const void* camera, void* raw_result,
    const void* raw_focus) noexcept
{
    ++observed.calculate_calls;
    observed.calculate_camera = camera;
    observed.calculate_output = raw_result;
    if (calculate_faults)
    {
        volatile int* poison = nullptr;
        *poison = 1;
    }
    const auto focus = static_cast<const FakeWorldVec3*>(raw_focus);
    auto result = static_cast<FakeWorldVec3*>(raw_result);
    *result = {focus->region, focus->x + scripted_offset.x, focus->y + scripted_offset.y,
        focus->z + scripted_offset.z};
    return result;
}

void __fastcall fake_get_intersection(void* level, const void* ray, void* intersection,
    const std::uint32_t surface, void** hit_entity, const float max_distance, const void* ignore_entity,
    const bool include_entities) noexcept
{
    ++observed.intersection_calls;
    observed.level = level;
    observed.max_distance = max_distance;
    observed.surface = surface;
    observed.ignore_entity = ignore_entity;
    observed.include_entities = include_entities;
    std::memcpy(observed.origin, ray, sizeof(observed.origin));
    std::memcpy(observed.direction, static_cast<const unsigned char*>(ray) + 12, sizeof(observed.direction));
    static_cast<void>(hit_entity);
    if (intersection_faults)
    {
        volatile int* poison = nullptr;
        *poison = 1;
    }
    if (ground_plane) {
        const auto* f=static_cast<const float*>(ray);
        const auto hit=f[4]<0 ? -f[1]/f[4] : infinity;
        scripted_distance=hit>=0 && hit<=max_distance ? hit : infinity;
    }
    // Write the hit parameter at offset 0, then scribble further to prove the buffer absorbs it.
    std::memcpy(intersection, &scripted_distance, sizeof(scripted_distance));
    for (std::size_t index = sizeof(float); index < intersection_write_size; ++index)
        static_cast<unsigned char*>(intersection)[index] = 0xAB;
}

gdtpc::LevelQueryEntries complete_entries()
{
    gdtpc::LevelQueryEntries entries{};
    entries.get_region = fake_get_region;
    entries.calculate_view_position = fake_calculate_view_position;
    entries.get_level_ptr = fake_get_level;
    entries.get_intersection = fake_get_intersection;
    return entries;
}

void reset()
{
    observed = {};
    scripted_distance = infinity;
    region_returns_null = level_returns_null = region_faults = intersection_faults = false;
    calculate_faults = false; ground_plane=false;
    scripted_offset = {0.0F, 0.0F, 42.0F};
    intersection_write_size = 0x14;
    const float camera_position[3]{100.0F, 10.0F, 242.0F};
    std::memcpy(fake_camera_object + 0x94, camera_position, sizeof(camera_position));
}

const gdtpc::CollisionRay unit_ray{{100.0F, 10.0F, 200.0F}, {0.0F, 1.0F, 0.0F}};
int fake_player = 0;
}

int main()
{
    try
    {
        // Incomplete entries must refuse rather than call through a null pointer.
        {
            reset();
            gdtpc::LevelQueryEntries partial{};
            partial.get_region = fake_get_region;
            require(!partial.complete(), "partial entries reported complete");
            gdtpc::WindowsLevelQuery query(partial, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            require(!query.raycast(unit_ray, 42.0F, distance), "incomplete entries were not refused");
            require(query.refused() == 1 && observed.region_calls == 0, "incomplete entries still called through");
        }

        // A null camera is refused before anything is resolved.
        {
            reset();
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, nullptr, &fake_player);
            auto distance = 0.0F;
            require(!query.raycast(unit_ray, 42.0F, distance), "a null camera was not refused");
            require(observed.region_calls == 0, "a null camera still resolved a region");
        }

        // Nonfinite geometry must never reach the engine. This is the failure a +0x28 camera
        // position would have produced.
        {
            reset();
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            const gdtpc::CollisionRay poisoned_origin{{quiet_nan, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
            const gdtpc::CollisionRay poisoned_direction{{0.0F, 0.0F, 0.0F}, {0.0F, infinity, 0.0F}};
            require(!query.raycast(poisoned_origin, 42.0F, distance), "a NaN origin was not refused");
            require(!query.raycast(poisoned_direction, 42.0F, distance), "an infinite direction was not refused");
            require(!query.raycast(unit_ray, quiet_nan, distance), "a NaN max distance was not refused");
            require(!query.raycast(unit_ray, 0.0F, distance), "a zero max distance was not refused");
            require(observed.region_calls == 0 && observed.intersection_calls == 0,
                "nonfinite geometry reached the engine");
        }

        // A null region or a null level refuses without attempting the raycast.
        {
            reset(); region_returns_null = true;
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            require(!query.raycast(unit_ray, 42.0F, distance), "a null region was not refused");
            require(observed.region_calls == 1 && observed.intersection_calls == 0, "a null region still raycast");

            reset(); level_returns_null = true;
            gdtpc::WindowsLevelQuery query2(entries, fake_camera_object, &fake_player);
            require(!query2.raycast(unit_ray, 42.0F, distance), "a null level was not refused");
            require(observed.level_calls == 1 && observed.intersection_calls == 0, "a null level still raycast");
        }

        // The happy path: the engine is handed exactly what it is owed, and the hit parameter comes
        // back from offset 0.
        {
            reset(); scripted_distance = 7.25F;
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            require(query.raycast(unit_ray, 42.0F, distance), "a valid query was refused");
            require(distance == 7.25F, "the hit distance was not read from offset 0");
            require(observed.intersection_calls == 1, "the raycast was not issued exactly once");
            require(observed.level == &fake_level_object, "the raycast did not use the resolved level");
            require(observed.max_distance == 42.0F, "the raycast was not bounded by the desired distance");
            require(observed.ignore_entity == &fake_player, "the player was not passed as the ignored entity");
            require(!observed.include_entities, "entity inclusion was requested");
            require(observed.origin[0] == 100.0F && observed.origin[1] == 10.0F && observed.origin[2] == 200.0F,
                "the ray origin was not laid out at offset 0");
            require(observed.direction[1] == 1.0F, "the ray direction was not laid out at offset 12");
            require(query.refused() == 0, "a successful query was counted as refused");
        }

        // The level is re-fetched on every query and never cached, because a level can unload while
        // the camera, engine and player pointers all stay stable.
        {
            reset(); scripted_distance = infinity;
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            for (int attempt = 0; attempt < 4; ++attempt)
                static_cast<void>(query.raycast(unit_ray, 42.0F, distance));
            require(observed.region_calls == 4 && observed.level_calls == 4,
                "the level pointer was cached across queries");
        }

        // A clean miss is +infinity and is reported as a successful query, not a refusal.
        {
            reset(); scripted_distance = infinity;
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            require(query.raycast(unit_ray, 42.0F, distance), "a clean miss was refused");
            require(std::isinf(distance) && distance > 0.0F, "a clean miss did not report +infinity");
            require(query.refused() == 0, "a clean miss was counted as refused");
        }

        // An access violation inside the engine is caught and reported as a refusal, not a crash.
        {
            reset(); region_faults = true;
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, &fake_region_object, &fake_player);
            auto distance = 0.0F;
            require(!query.raycast(unit_ray, 42.0F, distance), "a faulting region accessor was not refused");
            require(query.refused() == 1, "a faulting region accessor was not counted");

            reset(); intersection_faults = true;
            gdtpc::WindowsLevelQuery query2(entries, fake_camera_object, &fake_player);
            require(!query2.raycast(unit_ray, 42.0F, distance), "a faulting raycast was not refused");
            require(query2.refused() == 1, "a faulting raycast was not counted");
        }

        // The out buffer absorbs a writer that scribbles well past the documented 0x14 bytes.
        {
            reset(); scripted_distance = 3.5F; intersection_write_size = gdtpc::intersection_buffer_size;
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            require(query.raycast(unit_ray, 42.0F, distance), "an oversized write refused the query");
            require(distance == 3.5F, "an oversized write corrupted the hit distance");
        }

        // The live path derives the engine-convention ray from CalculateViewPosition(zero), using
        // the live-confirmed WorldCamera position at +0x94. All four entries are called once.
        {
            reset(); scripted_distance = 7.25F;
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            gdtpc::CollisionRay ray{};
            require(query.raycast_from_camera(42.0F, distance, ray), "camera-derived query was refused");
            require(observed.region_calls == 1 && observed.calculate_calls == 1 &&
                observed.level_calls == 1 && observed.intersection_calls == 1,
                "camera-derived query did not call the four-entry chain exactly once");
            require(observed.calculate_camera == fake_camera_object &&
                observed.calculate_output != fake_camera_object,
                "CalculateViewPosition did not receive this in RCX and the result buffer in RDX");
            require(ray.origin == gdtpc::CollisionVec3{100.0F, 10.0F, 200.0F},
                "focus was not camera position minus the calculated offset");
            require(ray.direction == gdtpc::CollisionVec3{0.0F, 0.0F, 1.0F},
                "calculated offset was not normalized into the ray direction");
            require(observed.origin[0] == 100.0F && observed.origin[1] == 10.0F && observed.origin[2] == 200.0F,
                "derived focus was not passed as the ray origin");
        }

        // A shoulder change happens after the current camera transform was calculated. Translate
        // the collision origin by that exact delta so a side switch cannot bypass a nearby wall.
        {
            reset(); scripted_distance = 7.25F;
            const auto entries = complete_entries();
            const gdtpc::CollisionVec3 translation{1.5F, 0.25F, -2.0F};
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player,
                gdtpc::default_physics_surface, translation);
            auto distance = 0.0F;
            gdtpc::CollisionRay ray{};
            require(query.raycast_from_camera(42.0F, distance, ray),
                "translated camera-derived query was refused");
            require(ray.origin == gdtpc::CollisionVec3{101.5F, 10.25F, 198.0F},
                "shoulder translation was not applied to the ray origin");
            require(ray.direction == gdtpc::CollisionVec3{0.0F, 0.0F, 1.0F},
                "shoulder translation changed the camera ray direction");
            require(observed.origin[0] == 101.5F && observed.origin[1] == 10.25F &&
                observed.origin[2] == 198.0F,
                "translated origin was not passed to the engine query");
        }

        // Virtual zoom supplies the displacement the camera position actually reflects (last frame's
        // distance along last frame's direction plus the eye pull). The origin subtracts that instead of
        // this frame's calculated offset, composes with the shoulder translation, and the direction is
        // still this frame's calculated offset.
        {
            reset(); scripted_distance = 7.25F;
            scripted_offset = {0.0F, 0.0F, 85.0F};
            const float camera_position[3]{100.0F, 10.0F, 215.0F};
            std::memcpy(fake_camera_object + 0x94, camera_position, sizeof(camera_position));
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player,
                gdtpc::default_physics_surface, {0.5F, 0.0F, 0.0F});
            query.set_camera_displacement({1.0F, 2.0F, 15.0F});
            auto distance = 0.0F;
            gdtpc::CollisionRay ray{};
            require(query.raycast_from_camera(12.0F, distance, ray), "displaced camera-derived query was refused");
            require(ray.origin == gdtpc::CollisionVec3{99.5F, 8.0F, 200.0F},
                "the recorded displacement was not subtracted from the translated camera position");
            require(ray.direction == gdtpc::CollisionVec3{0.0F, 0.0F, 1.0F},
                "the displacement changed the calculated ray direction");
            require(observed.origin[0] == 99.5F && observed.origin[1] == 8.0F && observed.origin[2] == 200.0F &&
                observed.max_distance == 12.0F, "the displaced origin or visual length was not passed to the engine");

            reset();
            gdtpc::WindowsLevelQuery poisoned(entries, fake_camera_object, &fake_player);
            poisoned.set_camera_displacement({quiet_nan, 0.0F, 0.0F});
            require(!poisoned.raycast_from_camera(12.0F, distance, ray), "a NaN displacement was not refused");
            require(observed.level_calls == 0 && observed.intersection_calls == 0,
                "a NaN displacement reached the level query");
        }

        // A CalculateViewPosition fault and a nonfinite +0x94 position are both refused before
        // GetLevelPtr or GetIntersection can see unsafe geometry.
        {
            reset(); calculate_faults = true;
            const auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, &fake_player);
            auto distance = 0.0F;
            gdtpc::CollisionRay ray{};
            require(!query.raycast_from_camera(42.0F, distance, ray), "calculate fault was not refused");
            require(observed.level_calls == 0 && observed.intersection_calls == 0,
                "calculate fault reached the level query");

            reset();
            const auto nan = quiet_nan;
            std::memcpy(fake_camera_object + 0x94, &nan, sizeof(nan));
            gdtpc::WindowsLevelQuery query2(entries, fake_camera_object, &fake_player);
            require(!query2.raycast_from_camera(42.0F, distance, ray), "NaN +0x94 position was not refused");
            require(observed.level_calls == 0 && observed.intersection_calls == 0,
                "NaN +0x94 position reached the level query");
        }

        // A grazing ray can miss the ground while the bottom of the camera clips it.
        {
            reset(); ground_plane=true;
            scripted_offset={0,-4.2F,41.79F};
            const float position[3]{0,0.3F,41.79F};
            std::memcpy(fake_camera_object+0x94,position,sizeof(position));
            auto entries=complete_entries();
            gdtpc::WindowsLevelQuery center(entries,fake_camera_object,&fake_player);
            float center_distance{}; gdtpc::CollisionRay ray{};
            require(center.raycast_from_camera(42,center_distance,ray) && std::isinf(center_distance),"center ray should miss ground");
            gdtpc::WindowsLevelQuery envelope(entries,fake_camera_object,&fake_player);
            envelope.set_clearance(0.6F); float distance{};
            require(envelope.raycast_from_camera(42,distance,ray),"clearance query refused");
            require(distance>0 && distance<42,"camera envelope did not catch grazing ground");
            require(ray.origin.y+ray.direction.y*distance>0.59F,"camera bottom clearance lost");
            require(observed.intersection_calls>=6,"unexpected envelope ray count");
        }
        {
            reset(); ground_plane = true;
            // Entire horizontal arm starts 0.2 below a one-sided plane.
            scripted_offset = {0.0F, 0.0F, 10.0F};
            const float eye[3]{0.0F, -0.2F, 10.0F};
            std::memcpy(fake_camera_object + 0x94, eye, sizeof(eye));
            auto entries = complete_entries();
            gdtpc::WindowsLevelQuery query(entries, fake_camera_object, nullptr);
            query.set_clearance(0.6F);
            float distance{}; gdtpc::CollisionRay ray{};
            require(query.raycast_from_camera(10.0F, distance, ray), "below-ground query refused");
            require(std::isfinite(distance) && distance < 1.0F, "below-ground horizontal arm escaped clearance guard");
        }
        std::cout << "PASS: level-query adapter entry validation, null camera/region/level refusal, "
                     "nonfinite-geometry refusal before any engine call, correct ray layout and bounds, "
                     "ignored player entity, uncached level re-fetch, miss sentinel passthrough, "
                     "structured-exception containment of a real access violation, and oversized "
                     "intersection-buffer tolerance, affine camera-ray derivation through all four "
                     "validated entries, shoulder-origin translation, virtual-zoom recorded displacement, and unsafe "
                     "camera-geometry refusal.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
