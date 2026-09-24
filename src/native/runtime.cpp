#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>

#define GDTPC_RUNTIME_EXPORTS
#include "runtime_api.h"
#include "callback_evidence.h"
#include "detour_hook.h"
#include "logging_drain.h"
#include "runtime_config.h"
#include "telemetry_queue.h"
#if defined(GDTPC_GATE1_PROFILE_WRITES)
#include "camera_write_adapter.h"
#include "profile_switch_model.h"
#endif
#if defined(GDTPC_CAMERA_COLLISION)
#include "level_query_adapter.h"
#include "ui_probe_model.h"
#include "mouse_look_model.h"
#include "virtual_zoom_model.h"
#include "controller_event_model.h"
#include "cursor_visibility_model.h"
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace
{
constexpr std::uint32_t abi_version = 2;
// A failed activation must not hang the injector's remote thread forever. A bounded join keeps
// worker ownership of its own resources and reports a pending shutdown instead of blocking.
constexpr DWORD initialization_stop_timeout_ms = 5000;
std::atomic<GdTpcPhase> phase{GdTpcPhase::inert};
std::atomic<std::uint32_t> hooks_installed{0};
std::mutex validation_mutex;
// last_error has its own lock so a status query never waits behind activation or a writer join,
// and so no exported boundary takes the lifecycle mutex just to read text.
std::mutex error_mutex;
std::array<wchar_t, 512> last_error{};
gdtpc::RuntimeConfig active_config{}; // validated settings retained for the life of the run
std::atomic<std::uint32_t> config_loaded{0};

using UpdateFromInput = void(__fastcall*)(void* camera);
UpdateFromInput update_from_input_original{};
gdtpc::DetourHook update_from_input_hook;
#if defined(GDTPC_CAMERA_COLLISION)
using SteamControllerUpdate = void(__fastcall*)(void* device, int elapsed_ms);
SteamControllerUpdate steam_controller_update_original{};
gdtpc::DetourHook steam_controller_update_hook;
std::atomic<DWORD> cursor_owner_thread{0}, cursor_apply_thread{0};
std::atomic<std::uint32_t> cursor_visibility_status{0};
#endif
HANDLE logging_thread{};
HANDLE logging_stop_event{};
std::atomic<std::uint32_t> writer_health{0}; // 0=inactive, 1=healthy, 2=failed, 3=stop pending, 4=complete
std::atomic<std::uint64_t> callback_count{0};
void* const* game_engine_slot{};
std::atomic<std::uint64_t> telemetry_published{0};
std::atomic<std::uint64_t> telemetry_dropped{0};
std::atomic<std::uint64_t> session_generation{0};
std::atomic<std::uintptr_t> session_camera{0};
std::atomic<std::uintptr_t> session_engine{0};
std::atomic<std::uintptr_t> session_player{0};
gdtpc::CallbackEvidence callback_evidence{};
// Observation-only camera-collision research. Captured once during initialization so the camera
// callback never enumerates threads or touches windows. Compared against the callback owner thread
// to decide whether a level query could ever be issued safely from the hook.
std::atomic<std::uint32_t> game_main_thread_id{0};
std::atomic<std::uint32_t> game_window_thread_id{0};
std::atomic<std::uint32_t> game_state_writes_enabled{0};
std::atomic<std::uint32_t> restore_state{0};
std::atomic<std::uint64_t> control_write_count{0};
std::atomic<std::uint64_t> restore_write_count{0};
std::atomic<std::uint32_t> camera_mode{0};
std::atomic<std::uint64_t> abandoned_excursions{0};
std::atomic<std::uint32_t> collision_state{static_cast<std::uint32_t>(gdtpc::CollisionState::ineligible)};
std::atomic<std::uint64_t> collision_query_count{0};
std::atomic<std::uint64_t> collision_hit_count{0};
std::atomic<std::uint64_t> collision_fault_count{0};
std::atomic<std::uint64_t> collision_write_count{0};
std::atomic<float> collision_arm{0.0F};
// The player's own zoom as the collision model understands it. A wrong value here is invisible in
// the arm alone, and mistaking our own writes for the player's choice was the second live defect.
std::atomic<float> collision_desired{0.0F};
std::atomic<std::uint32_t> collision_enabled_status{0};
// Toggle observation. The second live session lost F8 and the trace could not say why, because it
// recorded neither what the profile switch was told nor what it decided.
std::atomic<std::uint32_t> toggle_down_status{0};
std::atomic<std::uint64_t> toggle_edge_count{0};
std::atomic<std::uint32_t> profile_transition_status{0};
// Why the last collision zoom write ended as it did, so a refused write is diagnosable from the
// trace instead of only showing up as a fault count.
std::atomic<std::uint32_t> collision_write_result{0};
std::atomic<std::uint64_t> zoom_step_clicks{0};
std::atomic<std::uint32_t> shoulder_side_status{0};
std::atomic<std::uint64_t> shoulder_edge_count{0};
std::atomic<std::uint64_t> shoulder_write_count{0};
std::atomic<std::uint64_t> virtual_zoom_write_count{0}; // engine zoom, target and eye-offset writes


#if defined(GDTPC_GATE1_PROFILE_WRITES)
using GetEngineObject = void*(__fastcall*)(void* engine);
using SetCameraZoom = void(__fastcall*)(void* camera, float distance);
GetEngineObject get_engine_camera{};
GetEngineObject get_main_player{};
SetCameraZoom set_camera_zoom{};
std::optional<gdtpc::ProfileSwitchModel> profile_switch;
std::atomic<std::uint32_t> control_stop_requested{0};
HANDLE control_stop_ack_event{};
std::atomic<std::uint32_t> recoverable_fault_request{0}; // 0=idle, 1=arm, 2=execute
HANDLE recoverable_fault_ack_event{};
std::uint64_t control_generation{};
std::uintptr_t control_camera{};
std::uintptr_t control_engine{};
std::uintptr_t control_player{};
#if defined(GDTPC_CAMERA_COLLISION)
gdtpc::LevelQueryEntries collision_entries{};
void* const* core_engine_slot{};
GetEngineObject get_input_device{};
std::optional<gdtpc::CameraCollisionModel> collision_model;
std::optional<gdtpc::ZoomStepModel> zoom_step_model;
std::optional<gdtpc::ShoulderOffsetModel> shoulder_offset_model;
std::uint64_t collision_generation{};
std::uint64_t zoom_step_generation{};
std::uint64_t shoulder_generation{};
std::uintptr_t shoulder_camera{};
// True while the target field is parked at the arm, so it is handed back exactly once.
bool collision_holding_target{};
std::uint64_t collision_last_tick_ms{};
#endif
#endif

#if defined(GDTPC_CAMERA_COLLISION)
// Read-only menu-detection probe. One preallocated record buffer is handed between exactly two
// threads by a three-state atomic: the camera callback fills it (0 -> 1 -> 2) and the telemetry writer,
// which exclusively owns the probe file, writes it out (2 -> 0). Neither ever frees the buffer, so a
// capture racing the writer's exit simply leaves one unwritten record behind.
constexpr std::size_t ui_probe_buffer_bytes = 4U << 20;
std::uint8_t ui_probe_buffer[ui_probe_buffer_bytes];
std::size_t ui_probe_length{};
std::atomic<std::uint32_t> ui_probe_state{0}; // 0=idle, 1=capturing, 2=ready to write
std::wstring ui_probe_path;                   // set before the writer starts; read only by the writer
bool ui_probe_open_down{};
bool ui_probe_closed_down{};
std::atomic<std::uint64_t> ui_probe_write_failures{0};
// Mouse look (IMPLEMENTATION_PLAN.md section 7). Model and cursor bookkeeping are camera-thread only;
// the atomics exist for telemetry.
std::optional<gdtpc::MouseLookModel> mouse_look_model;
std::uint64_t mouse_look_generation{};
bool mouse_look_warp_failed{};
std::atomic<std::uint32_t> mouse_look_state_status{0};
std::atomic<float> mouse_look_aim_y{0.0F};
std::atomic<float> mouse_look_yaw_delta{0.0F};
std::atomic<std::uint64_t> mouse_look_warps{0};
std::atomic<std::uint64_t> mouse_look_escapes{0};
std::atomic<std::uint64_t> mouse_look_yaw_writes{0};
// Vertical look: a per-frame overlay on the engine-derived pitch (WorldCamera+0x10). UpdatePitch rewrites
// the native value inside UpdateFromInputImpl every frame, so there is nothing persistent to restore;
// ownership only distinguishes our last write from a fresh native value within the same frame.
bool mouse_look_pitch_apply{};
float mouse_look_pitch_offset{};
gdtpc::PitchOverlay mouse_look_pitch_overlay;
std::uint64_t mouse_look_last_tick{};
std::atomic<float> mouse_look_pitch_status{0.0F};
std::atomic<std::uint64_t> mouse_look_pitch_writes{0};
// Virtual zoom (IMPLEMENTATION_PLAN.md section 8). Camera-thread only; the atomics exist for telemetry.
// The engine zoom is held far so targeting reaches far; scroll drives the visual distance V, collision
// shortens it to the arm A, and the rendered eye is pulled in by D - A through WorldCamera+0x60.
std::optional<gdtpc::VirtualZoomModel> virtual_zoom_model;
gdtpc::EyeOffsetOverlay virtual_zoom_eye;
std::uint64_t virtual_zoom_generation{};
std::uintptr_t virtual_zoom_camera{};
std::uint64_t virtual_zoom_last_tick{};
bool virtual_zoom_frame_holding{};   // the engine is held at E for this callback
bool virtual_zoom_fell_back{};       // latched off; the accepted non-virtual path owns zoom for the process
float virtual_zoom_arm{};            // A: last visual arm from collision
bool virtual_zoom_arm_valid{};
// Everything the camera position +0x94 holds beyond the focus, recorded at the end of the previous callback:
// D * eye_direction(yaw, pitch) + pull. WorldCamera::Update computes +0x94 after our callback from exactly
// those fields, so next frame's collision origin is +0x94 minus this (see WindowsLevelQuery).
gdtpc::CollisionVec3 virtual_zoom_previous_eye{};
bool virtual_zoom_previous_eye_valid{};
std::atomic<std::uint32_t> virtual_zoom_state_status{0};
std::atomic<float> virtual_zoom_visual_status{0.0F};
std::atomic<float> virtual_zoom_arm_status{0.0F};
std::atomic<float> virtual_zoom_pull_status{0.0F};
std::atomic<float> virtual_zoom_engine_status{0.0F};
std::atomic<std::uint64_t> virtual_zoom_clicks{0};
std::atomic<std::uint64_t> virtual_zoom_faults{0};
std::atomic<std::uint64_t> camera_shake_suppressions{0};
// Third-person far-plane cap: third_person_far_plane_percent of native WorldCamera+0x18 while mouse look applies;
// restored exactly whenever it does not, and before the logical-stop acknowledgment.
gdtpc::FarPlaneOverlay far_plane_overlay;
std::atomic<std::uint32_t> far_plane_percent_status{0};
// NPC conversation signal. Resolved during activation from the validated addresses above; camera-thread use only.
using ObjectManagerGet = void*(__fastcall*)();
using ObjectManagerFind = void*(__fastcall*)(void* manager, std::uint32_t id);
ObjectManagerGet object_manager_get{};
ObjectManagerFind object_manager_find{};
std::uintptr_t controller_player_vtable{};
std::uintptr_t talk_to_npc_vtable{};
std::uintptr_t game_module_base{};
std::uintptr_t game_module_size{};
std::uint32_t npc_talk_fault_count{};
std::atomic<std::uint32_t> npc_talk_status{0};
std::atomic<std::uint32_t> controller_state_rva_status{0xFFFFFFFFU};
// Dot cursor while mouse look captures. Handle created once at activation and kept for the process (never destroyed while the
// window might still hold it). Camera-thread only; the callback runs on the window thread that handles WM_SETCURSOR.
HCURSOR dot_cursor_handle{};
std::uintptr_t win_window_vtable{};
std::uintptr_t dot_cursor_window{};
gdtpc::CursorHandleOverlay dot_cursor_overlay;
std::atomic<std::uint32_t> dot_cursor_status{0};
// Panel flag bytes behind the menu rule: bit 0 inventory 0x95d2, 1 quest/NPC 0x991a, 2 skills 0x9f9a,
// 3 map 0xa2da, 4 Escape confirmed by both 0x17a9 and 0x9c5a. The first Escape byte alone is also
// latched by the rift button, so treating it as a panel drops mouse look after that and other Alt clicks.
std::atomic<std::uint32_t> panel_open_flags_status{0xFFFFFFFFU};
// Read-only Steam Input event probe. SteamControllerDevice::Update is called first and its native 16-byte event vector is
// observed afterwards; the vector is never changed and every event continues through the game's original path.
std::atomic<std::uint64_t> controller_event_updates{0};
std::atomic<std::uint64_t> controller_event_epoch{0}; // odd while the event hook publishes a new snapshot
std::atomic<std::uint32_t> controller_event_count_status{0};
std::atomic<std::int32_t> controller_analog_action_status{-1};
std::atomic<float> controller_analog_x_status{0.0F};
std::atomic<float> controller_analog_y_status{0.0F};
std::atomic<std::uint64_t> controller_event_faults{0};
std::uint64_t controller_event_consumed_updates{}; // camera-thread only; never apply one Steam sample twice
// Right-stick pitch uses only the live-proven camera action id 36. Native yaw, movement and events remain untouched.
std::atomic<std::int32_t> right_stick_y_status{0};
std::atomic<float> stick_pitch_status{0.0F};
#endif

struct TelemetrySnapshot
{
    std::uint64_t sequence{}; // assigned by TelemetryQueue::publish
    std::uint64_t tick_ms{};
    std::uint64_t callbacks{};
    std::uint64_t generation{};
    std::uintptr_t camera{};
    std::uintptr_t player{};
    std::uintptr_t game_engine{};
    std::uintptr_t ui{};
    float world_yaw{};
    float world_pitch{};
    float world_fov{};
    float requested_yaw{};
    float zoom_blend{};
    float zoom_target_blend{};
    float zoom_a{};
    float zoom_b{};
    float facing_x{};
    float facing_y{};
    float facing_z{};
    float facing_heading{};
    std::uint32_t facing_valid{};
    std::uint32_t sample_valid{};
    std::uint32_t dialog_active{};
    std::uint32_t action_set{};
    std::uint32_t input_mode{};
    std::uint32_t foreground{};
    gdtpc::CallbackEvidenceSnapshot callback_evidence{};
    std::uint32_t main_thread_id{};
    std::uint32_t window_thread_id{};
    float camera_local_x{};   // Camera-level position candidate, +0x28
    float camera_local_y{};
    float camera_local_z{};
    float camera_world_x{};   // WorldCamera-level position candidate, +0x94
    float camera_world_y{};
    float camera_world_z{};
    float camera_offset_x{};  // WorldCamera::GetCameraOffset(), +0x60
    float camera_offset_y{};
    float camera_offset_z{};
    float target_offset_x{};  // GameCamera::GetTarget() composition, +0x55c
    float target_offset_y{};
    float target_offset_z{};
    std::uint32_t camera_mode{};
    std::uint32_t restore_state{};
    std::uint64_t abandoned_excursions{};
    std::uint32_t writes_enabled{};
    std::uint64_t control_writes{};
    std::uint64_t restore_writes{};
    float collision_arm{};
    float collision_desired{};
    std::uint32_t collision_state{};
    std::uint64_t collision_queries{};
    std::uint64_t collision_hits{};
    std::uint64_t collision_faults{};
    std::uint64_t collision_writes{};
    std::uint32_t toggle_down{};
    std::uint64_t toggle_edges{};
    std::uint32_t profile_transition{};
    std::uint32_t collision_write_result{};
    std::uint64_t zoom_step_clicks{};
    std::uint32_t shoulder_side{};
    std::uint64_t shoulder_edges{};
    std::uint64_t shoulder_writes{};
    // Menu-detection candidates from the 2026-09-13 probe, observation only. 0xFFFFFFFF / all-ones
    // mean "could not be read this frame" rather than a value.
    std::uint32_t menu_flag{0xFFFFFFFFU};      // byte at *(ui+0x1c20)+0x84
    std::uint64_t menu_ui9918{~0ULL};          // qword at ui+0x9918
    std::uint64_t menu_e1858{~0ULL};           // qword at GameEngine+0x1858 (code pointer; hover suspect)
    std::uint32_t mouse_look_state{};
    float aim_y{};
    float yaw_delta{};
    std::uint64_t cursor_warps{};
    std::uint64_t cursor_escapes{};  // captured frames whose cursor had left the client area before the warp
    std::uint64_t mouse_yaw_writes{};
    std::uint32_t combat_state{0xFFFFFFFFU}; // u32 Player+0x690 (CombatManager+0x160); meaning unknown
    std::uint32_t combat_aux{0xFFFFFFFFU};   // u32 Player+0x694
    std::uint32_t panel_candidates{0xFFFFFFFFU}; // bitmask, see read_menu_candidates
    float pitch_offset{};            // degrees applied on top of the engine-derived pitch
    std::uint64_t pitch_writes{};
    float visual_distance{};           // V
    float visual_arm{};                // A
    float eye_pull{};                  // P = D - A actually written (0 when released)
    float engine_distance{};           // D, +0x08 when the pull was computed
    std::uint32_t virtual_zoom_state{}; // gdtpc::VirtualZoomState
    std::uint64_t virtual_zoom_clicks{};
    std::uint64_t virtual_zoom_faults{};
    std::uint64_t camera_shake_suppressions{}; // active native shake branches suppressed while P > 0
    float far_plane{};                 // WorldCamera+0x18, the camera far-plane setting (SetCameraFarPlane)
    float render_far_plane{};          // WorldCamera+0xB0 = embedded Camera(+0x6C) far, used by GetFrustum(viewport)
    std::uint32_t far_plane_percent{}; // cap applied this frame, 0 when not applied
    std::uint32_t view_distance_locked{0xFFFFFFFFU}; // byte GameEngine+0x1B40 (IsViewDistanceLocked)
    std::uint32_t npc_talk{};                        // gdtpc::NpcTalkSignal
    std::uint32_t controller_state_rva{0xFFFFFFFFU}; // player controller top state vtable, Game.dll RVA (0 none)
    std::uint32_t cursor_visibility{}, cursor_owner{}, cursor_thread{};
    std::uint32_t dot_cursor{};                       // 0 not applied, 1 dot shown, 2 window not found / write refused
    std::uint32_t panel_open_flags{0xFFFFFFFFU};      // menu-rule flag bytes (see panel_open_flags_status)
    std::uint64_t controller_event_updates{};         // completed native SteamControllerDevice::Update calls observed
    std::uint32_t controller_event_count{};           // native event-vector length from the latest update
    std::int32_t controller_analog_action{-1};        // strongest analog action id, -1 when none
    float controller_analog_x{};                      // untouched native analog X
    float controller_analog_y{};                      // untouched native analog Y
    std::uint64_t controller_event_faults{};          // guarded vector-read failures
    std::int32_t right_stick_y{};                     // right-stick Y fed to vertical look (0: no source wired yet)
    float stick_pitch_delta{};                        // pitch-offset degrees the stick asked for this frame
};

constexpr std::size_t telemetry_capacity = 256;
gdtpc::TelemetryQueue<TelemetrySnapshot, telemetry_capacity> telemetry_queue{};
#if defined(GDTPC_CAMERA_COLLISION)
void sample_controller_event_status(TelemetrySnapshot& sample) noexcept;
#endif

struct AccessPoint
{
    const char* name;
    const std::uint8_t* prefix;
    std::size_t prefix_size;
    std::uint32_t rva;
};

constexpr std::array<std::uint8_t, 8> get_camera_prefix{0x48, 0x8d, 0x81, 0xb0, 0x13, 0x00, 0x00, 0xc3};
constexpr std::array<std::uint8_t, 15> update_from_input_prefix{0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0xd9, 0xff, 0x15, 0xf9, 0x8c, 0x34, 0x00};
constexpr std::array<std::uint8_t, 15> game_camera_update_prefix{0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x0f, 0x29, 0x70, 0xe8};
constexpr std::array<std::uint8_t, 16> game_camera_shake_prefix{0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x57, 0x48, 0x81, 0xec, 0x70, 0x04};
constexpr std::array<std::uint8_t, 8> get_camera_player_prefix{0x48, 0x8b, 0x81, 0x18, 0x01, 0x00, 0x00, 0xc3};
constexpr std::array<std::uint8_t, 7> get_input_mode_prefix{0x8b, 0x81, 0x18, 0x77, 0x03, 0x00, 0xc3};
constexpr std::uint8_t get_main_player_prefix[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x89, 0xe0, 0x40, 0x00, 0x00};
constexpr std::array<std::uint8_t, 17> adjust_yaw_prefix{0xf3, 0x0f, 0x58, 0x89, 0x9c, 0x05, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x89, 0x9c, 0x05, 0x00, 0x00, 0xc3};
constexpr std::uint8_t set_game_yaw_prefix[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8b, 0xd9};
constexpr std::uint8_t set_zoom_prefix[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0xf3, 0x0f, 0x10, 0x91, 0x94, 0x05, 0x00, 0x00, 0x48, 0x8b, 0xd9};
constexpr std::uint8_t set_game_fov_prefix[]{0xf3, 0x0f, 0x59, 0x0d, 0xbc, 0x5a, 0x49, 0x00, 0x48, 0x81, 0xc1, 0xb0, 0x13, 0x00, 0x00, 0x48, 0xff, 0x25, 0xba, 0x30, 0x31, 0x00};
constexpr std::uint8_t get_controller_direction_prefix[]{0xf2, 0x0f, 0x10, 0x81, 0x58, 0x04, 0x00, 0x00, 0xf2, 0x0f, 0x11, 0x02, 0x8b, 0x81, 0x60, 0x04, 0x00, 0x00, 0x89, 0x42, 0x08};
constexpr std::uint8_t set_controller_direction_prefix[]{0xf2, 0x0f, 0x10, 0x02, 0xf2, 0x0f, 0x11, 0x81, 0x58, 0x04, 0x00, 0x00, 0x8b, 0x42, 0x08, 0x89, 0x81, 0x60, 0x04, 0x00, 0x00};
constexpr std::uint8_t get_game_controller_prefix[]{0x48, 0x8d, 0x81, 0xf0, 0x76, 0x03, 0x00, 0xc3};
constexpr std::uint8_t get_action_set_prefix[]{0x8b, 0x41, 0x08, 0xc3};
constexpr std::array<std::uint8_t, 8> get_coords_prefix{0x48, 0x8d, 0x81, 0xd8, 0x00, 0x00, 0x00, 0xc3};
constexpr std::array<std::uint8_t, 6> get_yaw_prefix{0xf3, 0x0f, 0x10, 0x41, 0x0c, 0xc3};
constexpr std::uint8_t set_yaw_prefix[]{0xf3, 0x0f, 0x11, 0x49, 0x0c, 0xc3};
constexpr std::array<std::uint8_t, 6> get_pitch_prefix{0xf3, 0x0f, 0x10, 0x41, 0x10, 0xc3};
constexpr std::array<std::uint8_t, 6> set_pitch_prefix{0xf3, 0x0f, 0x11, 0x49, 0x10, 0xc3};
constexpr std::array<std::uint8_t, 6> get_fov_prefix{0xf3, 0x0f, 0x10, 0x41, 0x14, 0xc3};
constexpr std::array<std::uint8_t, 6> set_fov_prefix{0xf3, 0x0f, 0x11, 0x49, 0x14, 0xc3};

constexpr std::uint8_t get_world_camera_region_prefix[]{0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x01, 0x48, 0x8d, 0x54, 0x24, 0x20};
constexpr std::uint8_t calculate_view_position_prefix[]{0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x60};
constexpr std::uint8_t get_region_level_ptr_prefix[]{0x48, 0x8b, 0x41, 0x68, 0xc3};
constexpr std::uint8_t get_level_intersection_prefix[]{0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18};
constexpr std::uint8_t object_manager_get_prefix[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff};
constexpr std::uint8_t get_input_device_prefix[]{0x48, 0x8b, 0x81, 0x00, 0x02, 0x00, 0x00, 0xc3};
constexpr std::uint8_t steam_controller_update_prefix[]{0x48, 0x8b, 0xc4, 0x55, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1};
// Unexported ObjectManager id lookup (Game.dll RVA 0x19D20), the routine CursorHandler::GetPlayerCtrl and the game's own
// controller code use: (manager, id) -> object or null, under the manager's critical section. No export exists, so it is
// validated by exact RVA and prefix on the hash-verified module, like the exported access points.
constexpr std::uint8_t object_manager_find_prefix[]{0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xda, 0x48, 0x8b, 0xf1};
constexpr std::uint32_t object_manager_find_rva = 105760; // 0x19D20

constexpr std::array<std::uint8_t, 32> executable_hash{0x53, 0x85, 0xc7, 0xa8, 0xb5, 0x97, 0xdf, 0x6d, 0x8c, 0xad, 0x64, 0x66, 0x0c, 0x4a, 0xa4, 0xf4, 0x1c, 0x27, 0xf1, 0x22, 0x45, 0x85, 0xff, 0x86, 0x03, 0x20, 0x3c, 0xb7, 0x81, 0x52, 0xe4, 0x0e};
constexpr std::array<std::uint8_t, 32> game_hash{0x07, 0x77, 0x5a, 0x29, 0x70, 0x50, 0xe8, 0x4a, 0x84, 0x6a, 0xf1, 0x18, 0x27, 0x31, 0x70, 0x06, 0x14, 0xfb, 0x8b, 0x7b, 0xb4, 0x1c, 0xca, 0x46, 0xb3, 0x7f, 0xd2, 0x4c, 0x90, 0x52, 0x93, 0x87};
constexpr std::array<std::uint8_t, 32> engine_hash{0x9f, 0x20, 0x42, 0xe1, 0xfb, 0xa6, 0xc9, 0x26, 0x1c, 0x5e, 0x9f, 0x1e, 0x6d, 0x60, 0x75, 0x1a, 0x49, 0xbe, 0x66, 0x9f, 0xdc, 0x68, 0x7f, 0x87, 0xd8, 0xa0, 0x82, 0xc7, 0x98, 0xfd, 0x1c, 0xa0};

constexpr std::array game_access_points{
    AccessPoint{"?GetCamera@GameEngine@GAME@@QEAAPEAVGameCamera@2@XZ", get_camera_prefix.data(), get_camera_prefix.size(), 2904096},
    AccessPoint{"?UpdateFromInputImpl@GameCamera@GAME@@MEAAXXZ", update_from_input_prefix.data(), update_from_input_prefix.size(), 2797744},
    AccessPoint{"?Update@GameCamera@GAME@@UEAAXXZ", game_camera_update_prefix.data(), game_camera_update_prefix.size(), 2802656},
    AccessPoint{"?Shake@GameCamera@GAME@@QEAAXAEBVViewport@2@HMAEBVWorldVec3@2@_N@Z", game_camera_shake_prefix.data(), game_camera_shake_prefix.size(), 2798400},
    AccessPoint{"?GetPlayer@GameCamera@GAME@@QEAAPEAVPlayer@2@XZ", get_camera_player_prefix.data(), get_camera_player_prefix.size(), 2797680},
    AccessPoint{"?GetInputMode@GameEngine@GAME@@QEBA?AW4InputMode@2@XZ", get_input_mode_prefix.data(), get_input_mode_prefix.size(), 2988448},
    AccessPoint{"?GetMainPlayer@GameEngine@GAME@@QEBAPEAVPlayer@2@XZ", get_main_player_prefix, std::size(get_main_player_prefix), 2980496},
    AccessPoint{"?AdjustYaw@GameCamera@GAME@@QEAAXM@Z", adjust_yaw_prefix.data(), adjust_yaw_prefix.size(), 2802480},
    AccessPoint{"?SetCameraYaw@GameCamera@GAME@@UEAAXM@Z", set_game_yaw_prefix, std::size(set_game_yaw_prefix), 2803296},
    AccessPoint{"?SetZoom@GameCamera@GAME@@QEAAXM@Z", set_zoom_prefix, std::size(set_zoom_prefix), 2803344},
    AccessPoint{"?SetCameraFOV@GameEngine@GAME@@QEAAXM@Z", set_game_fov_prefix, std::size(set_game_fov_prefix), 3020256},
    AccessPoint{"?GetControllerDirection@ControllerPlayer@GAME@@QEAAXAEAVVec3@2@@Z", get_controller_direction_prefix, std::size(get_controller_direction_prefix), 1362304},
    AccessPoint{"?SetControllerDirection@ControllerPlayer@GAME@@QEAAXAEBVVec3@2@@Z", set_controller_direction_prefix, std::size(set_controller_direction_prefix), 1362272},
    AccessPoint{"?GetGameController@GameEngine@GAME@@QEAAAEAVGameController@2@XZ", get_game_controller_prefix, std::size(get_game_controller_prefix), 2988432},
    AccessPoint{"?GetActionSet@GameController@GAME@@QEBA?AW4ActionSet@12@XZ", get_action_set_prefix, std::size(get_action_set_prefix), 151488}};
constexpr std::array engine_access_points{
    AccessPoint{"?GetCoordsUnparented@Entity@GAME@@QEBAAEBVWorldCoords@2@XZ", get_coords_prefix.data(), get_coords_prefix.size(), 48096},
    AccessPoint{"?GetCameraYaw@WorldCamera@GAME@@QEBAMXZ", get_yaw_prefix.data(), get_yaw_prefix.size(), 464416},
    AccessPoint{"?SetCameraYaw@WorldCamera@GAME@@UEAAXM@Z", set_yaw_prefix, std::size(set_yaw_prefix), 533808},
    AccessPoint{"?GetCameraPitch@WorldCamera@GAME@@QEBAMXZ", get_pitch_prefix.data(), get_pitch_prefix.size(), 464448},
    AccessPoint{"?SetCameraPitch@WorldCamera@GAME@@QEAAXM@Z", set_pitch_prefix.data(), set_pitch_prefix.size(), 2267120},
    AccessPoint{"?GetCameraFOV@WorldCamera@GAME@@QEBAMXZ", get_fov_prefix.data(), get_fov_prefix.size(), 464480},
    AccessPoint{"?SetCameraFOV@WorldCamera@GAME@@QEAAXM@Z", set_fov_prefix.data(), set_fov_prefix.size(), 2267136},
    // Camera-collision chain. Validated like every other access point so an unknown build fails
    // closed rather than calling an address that means something else entirely.
    AccessPoint{"?GetRegion@WorldCamera@GAME@@QEBAPEAVRegion@2@XZ", get_world_camera_region_prefix, std::size(get_world_camera_region_prefix), 2263712},
    AccessPoint{"?CalculateViewPosition@WorldCamera@GAME@@MEBA?AVWorldVec3@2@AEBV32@@Z", calculate_view_position_prefix, std::size(calculate_view_position_prefix), 2265552},
    AccessPoint{"?GetLevelPtr@Region@GAME@@QEBAPEAVLevel@2@XZ", get_region_level_ptr_prefix, std::size(get_region_level_ptr_prefix), 1654688},
    AccessPoint{"?GetIntersection@Level@GAME@@QEBAXAEBVRay@2@AEAVIntersection@2@W4PhysicsSurface@2@PEAPEAVEntity@2@MPEBV62@_N@Z", get_level_intersection_prefix, std::size(get_level_intersection_prefix), 1244464},
    AccessPoint{"?Get@?$Singleton@VObjectManager@GAME@@@GAME@@SAPEAVObjectManager@2@XZ", object_manager_get_prefix, std::size(object_manager_get_prefix), 114752},
    AccessPoint{"?GetInputDevice@Engine@GAME@@QEAAPEAVInputDevice@2@XZ", get_input_device_prefix, std::size(get_input_device_prefix), 489552},
    AccessPoint{"?Update@SteamControllerDevice@GAME@@QEAAXH@Z", steam_controller_update_prefix, std::size(steam_controller_update_prefix), 1820016}};

// Exported vtables (data, not code): identity checks for the player controller and its talk-to-NPC state.
struct DataExport
{
    const char* name;
    std::uint32_t rva;
};
constexpr std::array engine_data_exports{
    DataExport{"??_7WinWindow@GAME@@6B@", 3404960}, // 0x33F4A0
    DataExport{"?gEngine@GAME@@3PEAVEngine@1@EA", 4330536}}; // 0x421428
constexpr std::array game_data_exports{
    DataExport{"??_7ControllerPlayer@GAME@@6B@", 7009280},
    DataExport{"??_7ControllerPlayerStateTalkToNpc@GAME@@6B@", 7417624}};

void set_last_error(const wchar_t* message) noexcept
{
    if (message == nullptr) message = L"Unknown failure.";
    try
    {
        std::scoped_lock lock(error_mutex);
        wcsncpy_s(last_error.data(), last_error.size(), message, _TRUNCATE);
    }
    catch (...) {} // error text is diagnostic only and must never escape a noexcept boundary
}

void clear_last_error() noexcept
{
    try
    {
        std::scoped_lock lock(error_mutex);
        last_error.fill(L'\0');
    }
    catch (...) {}
}

void reject(const GdTpcPhase rejected_phase, const wchar_t* message) noexcept
{
    set_last_error(message);
    phase.store(rejected_phase, std::memory_order_release);
}

bool validate_access_points(const HMODULE module, const AccessPoint* points, const std::size_t count) noexcept
{
    const auto base = reinterpret_cast<const std::uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto address = reinterpret_cast<const std::uint8_t*>(GetProcAddress(module, points[index].name));
        if (address == nullptr || address != base + points[index].rva ||
            points[index].rva + points[index].prefix_size > nt->OptionalHeader.SizeOfImage ||
            std::memcmp(address, points[index].prefix, points[index].prefix_size) != 0)
            return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) return false;
    }
    return true;
}

bool validate_internal_code(const HMODULE module, const std::uint32_t rva, const std::uint8_t* prefix,
    const std::size_t prefix_size) noexcept
{
    const auto base = reinterpret_cast<const std::uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || rva + prefix_size > nt->OptionalHeader.SizeOfImage) return false;
    const auto address = base + rva;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) return false;
    return std::memcmp(address, prefix, prefix_size) == 0;
}

bool validate_data_exports(const HMODULE module, const DataExport* exports, const std::size_t count) noexcept
{
    const auto base = reinterpret_cast<const std::uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto address = reinterpret_cast<const std::uint8_t*>(GetProcAddress(module, exports[index].name));
        if (address == nullptr || address != base + exports[index].rva ||
            exports[index].rva + sizeof(void*) > nt->OptionalHeader.SizeOfImage) return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    }
    return true;
}

bool validate_engine_slot(const HMODULE game) noexcept
{
    const auto address = GetProcAddress(game, "?gGameEngine@GAME@@3PEAVGameEngine@1@EA");
    if (address == nullptr) return false;
    const auto base = reinterpret_cast<const std::uint8_t*>(game);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto byte = reinterpret_cast<const std::uint8_t*>(address);
    if (byte < base || byte + sizeof(void*) > base + nt->OptionalHeader.SizeOfImage) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto readable = memory.Protect & 0xff;
    return readable == PAGE_READONLY || readable == PAGE_READWRITE || readable == PAGE_WRITECOPY ||
        readable == PAGE_EXECUTE_READ || readable == PAGE_EXECUTE_READWRITE || readable == PAGE_EXECUTE_WRITECOPY;
}

bool is_grim_dawn_host() noexcept
{
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return false;
    const auto separator = std::wcsrchr(path.data(), L'\\');
    const auto filename = separator == nullptr ? path.data() : separator + 1;
    return _wcsicmp(filename, L"Grim Dawn.exe") == 0;
}

std::wstring join_path(std::wstring root, const wchar_t* relative)
{
    if (!root.empty() && root.back() != L'\\' && root.back() != L'/')
        root.push_back(L'\\');
    root.append(relative);
    return root;
}

bool hash_file(const std::wstring& path, std::array<std::uint8_t, 32>& digest)
{
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD received = 0;
    bool success = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &received, 0) >= 0;
    std::vector<std::uint8_t> object(success ? object_size : 0);
    success = success && BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0;
    std::array<std::uint8_t, 65536> buffer{};
    while (success)
    {
        DWORD count = 0;
        if (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) == FALSE)
        {
            success = false;
            break;
        }
        if (count == 0)
            break;
        success = BCryptHashData(hash, buffer.data(), count, 0) >= 0;
    }
    success = success && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return success;
}

bool validate_hash(const std::wstring& path, const std::array<std::uint8_t, 32>& expected)
{
    std::array<std::uint8_t, 32> actual{};
    return hash_file(path, actual) && std::equal(actual.begin(), actual.end(), expected.begin());
}

#if defined(GDTPC_GATE1_PROFILE_WRITES)
class WindowsCameraMemoryAccess final : public gdtpc::CameraMemoryAccess
{
public:
    explicit WindowsCameraMemoryAccess(const int fail_write = -1) noexcept : fail_write_{fail_write} {}
    [[nodiscard]] bool preflight(const void* camera, const std::size_t offset, const bool writable) noexcept override
    {
        if (camera == nullptr) return false;
        const auto address = static_cast<const std::byte*>(camera) + offset;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        const auto end = start + sizeof(float);
        const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (end < start || end > region_end) return false;
        const auto protection = memory.Protect & 0xff;
        if (writable)
            return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        return protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
    }

    [[nodiscard]] bool read(const void* camera, const std::size_t offset, void* destination,
        const std::size_t size) noexcept override
    {
        if (camera == nullptr || destination == nullptr || size != sizeof(float)) return false;
        __try { std::memcpy(destination, static_cast<const std::byte*>(camera) + offset, size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    [[nodiscard]] bool write(void* camera, const std::size_t offset, const void* source,
        const std::size_t size) noexcept override
    {
        if (camera == nullptr || source == nullptr || size != sizeof(float)) return false;
        if (write_count_++ == fail_write_) return false;
        __try { std::memcpy(static_cast<std::byte*>(camera) + offset, source, size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    [[nodiscard]] bool synchronize_zoom(void* camera, const float distance) noexcept override
    {
        if (camera == nullptr || set_camera_zoom == nullptr || !std::isfinite(distance)) return false;
        __try { set_camera_zoom(camera, distance); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
private:
    int fail_write_{};
    int write_count_{};
};

struct ObservedControlIdentity
{
    void* camera{};
    void* engine{};
    void* player{};
    bool valid{};
};

ObservedControlIdentity observe_control_identity(void* callback_camera) noexcept
{
    ObservedControlIdentity observed{};
    __try
    {
        if (callback_camera == nullptr || game_engine_slot == nullptr || get_engine_camera == nullptr || get_main_player == nullptr)
            __leave;
        observed.engine = *game_engine_slot;
        if (observed.engine == nullptr) __leave;
        observed.camera = get_engine_camera(observed.engine);
        observed.player = *reinterpret_cast<void**>(static_cast<std::byte*>(callback_camera) + 0x118);
        const auto main_player = get_main_player(observed.engine);
        observed.valid = observed.camera == callback_camera && observed.player != nullptr && observed.player == main_player;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { observed.valid = false; }
    return observed;
}

bool guarded_read_mouse_input_mode(void* engine, std::uint32_t& mode) noexcept
{
    mode = 0xFFFFFFFFU;
    if (engine == nullptr) return false;
    __try
    {
        mode = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(engine) + 0x37718);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { mode = 0xFFFFFFFFU; return false; }
}

std::uint64_t update_control_generation(const ObservedControlIdentity& identity) noexcept
{
    // A loading frame with no validated player is absence of an identity, not a new identity. Rift
    // travel can briefly produce exactly that shape while retaining the same camera/player objects;
    // advancing here made the returning objects look like a replacement and permanently faulted a
    // still-resident third-person profile. The first subsequent valid, different identity still
    // advances the generation normally.
    if (!identity.valid) return control_generation;
    const auto camera = reinterpret_cast<std::uintptr_t>(identity.camera);
    const auto engine = reinterpret_cast<std::uintptr_t>(identity.engine);
    const auto player = reinterpret_cast<std::uintptr_t>(identity.player);
    if (camera != control_camera || engine != control_engine || player != control_player)
    {
        control_camera = camera;
        control_engine = engine;
        control_player = player;
        ++control_generation;
    }
    return control_generation;
}

#if defined(GDTPC_CAMERA_COLLISION)
void undo_override_before_toggle(void* camera) noexcept;
void remove_shoulder_overlay(void* camera) noexcept;
void force_release_mouse_look() noexcept;
void release_dot_cursor() noexcept;
void stop_virtual_zoom(void* camera) noexcept;
#endif

void run_profile_control(void* camera, const ObservedControlIdentity& before) noexcept
{
    if (!profile_switch.has_value()) return;
    const auto after = observe_control_identity(camera);
    const auto generation = update_control_generation(after);
    const auto stable = before.valid && after.valid && before.camera == after.camera && before.engine == after.engine &&
        before.player == after.player;
    gdtpc::ProfileFrame frame{};
    // A player-less world-exit frame is not a valid control session, but an engine-confirmed
    // callback camera is still the only safe object on which to determine whether our invariant
    // profile remains resident. Never substitute an unconfirmed callback address here.
    if (after.engine != nullptr && after.camera == camera) frame.camera = after.camera;
    if (stable)
    {
        frame.session = {reinterpret_cast<std::uintptr_t>(after.camera), reinterpret_cast<std::uintptr_t>(after.engine),
            reinterpret_cast<std::uintptr_t>(after.player), generation};
    }
    DWORD foreground_process = 0;
    static_cast<void>(GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process));
    frame.foreground = foreground_process == GetCurrentProcessId();
    const auto physical_toggle_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    // Recorded before any gating so the trace shows every press, including ones the switch refuses.
    const auto toggle_edge =
        physical_toggle_down && toggle_down_status.exchange(1, std::memory_order_relaxed) == 0;
    if (toggle_edge) toggle_edge_count.fetch_add(1, std::memory_order_relaxed);
    else if (!physical_toggle_down) toggle_down_status.store(0, std::memory_order_relaxed);

    auto fault_stage = recoverable_fault_request.load(std::memory_order_acquire);
    const auto fault_eligible = stable && frame.foreground && !physical_toggle_down &&
        camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::native);
    if (fault_stage == 1 && fault_eligible)
    {
        frame.toggle_down = false; // deliberately arm the normal release-edge path first
        recoverable_fault_request.store(2, std::memory_order_release);
    }
    else frame.toggle_down = fault_stage == 0 ? physical_toggle_down : fault_stage == 2 && fault_eligible;
    frame.stop_requested = control_stop_requested.load(std::memory_order_acquire) != 0;

#if defined(GDTPC_CAMERA_COLLISION)
    // Leaving third person must remember the player's OWN zoom, not the distance collision happens to
    // be holding the camera at. ProfileSwitchModel captures the camera's current blend exactly as it
    // finds it, so the override is undone before it looks. Only on a frame where the toggle key is
    // down, so it costs nothing the rest of the time.
    // Exactly once per press, on the rising edge, which is also the frame the profile switch acts on.
    // Doing it on every frame the key is held made the camera alternate between the player's zoom and
    // the arm for the duration of the keypress, a visible snap on entering third person at a wall.
    if (toggle_edge) undo_override_before_toggle(camera);
    if (toggle_edge || frame.stop_requested || fault_stage != 0) remove_shoulder_overlay(camera);
    // The cursor is handed back on the camera thread before the stop is acknowledged, so a logical stop
    // can never leave mouse look holding the cursor.
    if (frame.stop_requested || fault_stage != 0)
    {
        force_release_mouse_look();
        // Likewise the eye offset base is restored before the acknowledgment. The engine zoom needs no
        // hand-back here: the profile switch restores the native zoom bytes itself.
        stop_virtual_zoom(camera);
    }
#endif

    WindowsCameraMemoryAccess access(fault_stage == 2 && fault_eligible ? 4 : -1);
    const auto result = profile_switch->step(access, frame);
    profile_transition_status.store(static_cast<std::uint32_t>(result.transition), std::memory_order_relaxed);
    camera_mode.store(static_cast<std::uint32_t>(result.mode), std::memory_order_release);
    restore_state.store(static_cast<std::uint32_t>(result.restore_state), std::memory_order_release);
    control_write_count.store(result.control_write_count + collision_write_count.load(std::memory_order_relaxed) +
        shoulder_write_count.load(std::memory_order_relaxed) + virtual_zoom_write_count.load(std::memory_order_relaxed),
        std::memory_order_release);
    restore_write_count.store(result.restore_write_count, std::memory_order_release);
    abandoned_excursions.store(profile_switch->abandoned_excursion_count(), std::memory_order_release);
    if (!result.control_enabled) game_state_writes_enabled.store(0, std::memory_order_release);
    if (fault_stage == 2 && fault_eligible && result.transition == gdtpc::ProfileTransition::rejected_fault)
    {
        recoverable_fault_request.store(0, std::memory_order_release);
        if (recoverable_fault_ack_event != nullptr) SetEvent(recoverable_fault_ack_event);
    }
    else if (fault_stage == 2 && result.transition == gdtpc::ProfileTransition::none)
        recoverable_fault_request.store(1, std::memory_order_release); // identity changed after arming
    if (frame.stop_requested && (!result.dirty || result.restore_state == gdtpc::ProfileRestoreState::impossible))
    {
        game_state_writes_enabled.store(0, std::memory_order_release);
        if (control_stop_ack_event != nullptr) SetEvent(control_stop_ack_event);
    }
}

#if defined(GDTPC_CAMERA_COLLISION)
bool guarded_read_camera_distance(const void* camera, float& distance) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        std::memcpy(&distance, static_cast<const std::byte*>(camera) + 0x08, sizeof(distance));
        return std::isfinite(distance) && distance > 0.0F;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The player's own chosen zoom, taken from the engine's zoom TARGET rather than from the camera's
// live distance.
//
// The engine stores zoom as a blend between the camera's two distance endpoints and continuously
// animates the current blend at +0x580 toward the target blend at +0x584. The player writes the
// target when they scroll, in discrete steps, and nothing in this runtime ever writes it; the native
// zoom setter writes the current blend only. So the target is an exact, uncontaminated statement of
// what the player wants, while the live distance is a moving value that collision itself overwrites.
// Reading the live distance and inferring intent from it is what ratcheted the player's zoom onto the
// character across two live sessions.
bool guarded_read_player_zoom(const void* camera, float& distance) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        const auto* bytes = static_cast<const std::byte*>(camera);
        float target_blend = 0.0F, endpoint_a = 0.0F, endpoint_b = 0.0F;
        std::memcpy(&target_blend, bytes + 0x584, sizeof(target_blend));
        std::memcpy(&endpoint_a, bytes + 0x590, sizeof(endpoint_a));
        std::memcpy(&endpoint_b, bytes + 0x594, sizeof(endpoint_b));
        if (!std::isfinite(target_blend) || target_blend < 0.0F || target_blend > 1.0F ||
            !std::isfinite(endpoint_a) || !std::isfinite(endpoint_b) || endpoint_a > endpoint_b ||
            endpoint_a <= 0.0F)
            return false;
        distance = endpoint_a + (endpoint_b - endpoint_a) * target_blend;
        return std::isfinite(distance) && distance > 0.0F;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Substitutes a finer zoom step for the engine's own.
//
// The engine moves its zoom target by a fixed tenth of the blend per scroll click, which on the
// profile's endpoints is a fixed number of units: far too coarse close in, far too fine far out. This
// reads the target the engine just wrote, asks the model whether that was one of its clicks, and if so
// replaces it with a proportional step. The target field is the player's own intent, so it is written
// ONLY when the model says a click was detected, never otherwise.
//
// This must run before the collision decision in the same callback, because the collision path reads
// the target to learn the player's zoom and writes it back after the native setter tramples it.
bool guarded_apply_zoom_step(void* camera) noexcept
{
    if (camera == nullptr || !zoom_step_model.has_value()) return false;
    __try
    {
        auto* bytes = static_cast<std::byte*>(camera);
        auto* const target_field = reinterpret_cast<float*>(bytes + 0x584);
        const auto engine_target = *target_field;
        const auto endpoint_a = *reinterpret_cast<const float*>(bytes + 0x590);
        const auto endpoint_b = *reinterpret_cast<const float*>(bytes + 0x594);
        auto substituted = 0.0F;
        if (!zoom_step_model->step(engine_target, endpoint_a, endpoint_b, substituted)) return false;
        if (!std::isfinite(substituted) || substituted < 0.0F || substituted > 1.0F) return false;
        *target_field = substituted;
        zoom_step_clicks.store(zoom_step_model->click_count(), std::memory_order_relaxed);
        return *target_field == substituted;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_read_zoom_endpoints(const void* camera, float& endpoint_a, float& endpoint_b) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        const auto* bytes = static_cast<const std::byte*>(camera);
        std::memcpy(&endpoint_a, bytes + 0x590, sizeof(endpoint_a));
        std::memcpy(&endpoint_b, bytes + 0x594, sizeof(endpoint_b));
        return std::isfinite(endpoint_a) && std::isfinite(endpoint_b) && endpoint_a > 0.0F &&
            endpoint_b > endpoint_a;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_read_target_blend(const void* camera, float& blend) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        std::memcpy(&blend, static_cast<const std::byte*>(camera) + 0x584, sizeof(blend));
        return std::isfinite(blend) && blend >= 0.0F && blend <= 1.0F;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_write_target_blend(void* camera, const float blend) noexcept
{
    if (camera == nullptr || !std::isfinite(blend) || blend < 0.0F || blend > 1.0F) return false;
    __try
    {
        auto* const field = reinterpret_cast<float*>(static_cast<std::byte*>(camera) + 0x584);
        *field = blend;
        return *field == blend;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_read_target_offset(const void* camera, gdtpc::CollisionVec3& value) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        std::memcpy(&value, static_cast<const std::byte*>(camera) + 0x55c, sizeof(value));
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_write_target_offset(void* camera, const gdtpc::CollisionVec3& value) noexcept
{
    if (camera == nullptr || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
        return false;
    // Constructor and ClampTargetOffset prove horizontal +/-40 and vertical +/-15 extents. Refuse
    // rather than bypass those engine bounds if a native offset and our overlay would exceed them.
    if (std::abs(value.x) > 40.0F || std::abs(value.y) > 15.0F || std::abs(value.z) > 40.0F) return false;
    const auto* const address = static_cast<const std::byte*>(camera) + 0x55c;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto protection = memory.Protect & 0xff;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + sizeof(value);
    const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    if (end < start || end > region_end) return false;
    __try
    {
        // +0x55c, the GetTarget() offset validated above. +0x60 is the eye offset that orbits the view
        // and is prohibited; an earlier candidate validated this field but wrote that one.
        auto* const field = static_cast<std::byte*>(camera) + 0x55c;
        std::memcpy(field, &value, sizeof(value));
        gdtpc::CollisionVec3 readback{};
        std::memcpy(&readback, field, sizeof(readback));
        return readback == value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool apply_shoulder_decision(void* camera, const gdtpc::ShoulderOffsetDecision& decision) noexcept
{
    shoulder_side_status.store(static_cast<std::uint32_t>(decision.side), std::memory_order_relaxed);
    if (shoulder_offset_model.has_value())
        shoulder_edge_count.store(shoulder_offset_model->edge_count(), std::memory_order_relaxed);
    if (!decision.write) return true;
    shoulder_write_count.fetch_add(1, std::memory_order_relaxed);
    if (guarded_write_target_offset(camera, decision.value)) return true;
    if (shoulder_offset_model.has_value()) shoulder_offset_model->reset();
    shoulder_side_status.store(static_cast<std::uint32_t>(gdtpc::ShoulderSide::center), std::memory_order_relaxed);
    return false;
}

void remove_shoulder_overlay(void* camera) noexcept
{
    if (!shoulder_offset_model.has_value()) return;
    gdtpc::CollisionVec3 current{};
    if (!guarded_read_target_offset(camera, current))
    {
        shoulder_offset_model->reset();
        shoulder_side_status.store(0, std::memory_order_relaxed);
        return;
    }
    static_cast<void>(apply_shoulder_decision(camera, shoulder_offset_model->relinquish(current)));
}

struct ShoulderFrameResult
{
    gdtpc::CollisionVec3 translation{};
    bool force_query{};
};

ShoulderFrameResult run_shoulder_control(void* camera, const ObservedControlIdentity& before) noexcept
{
    ShoulderFrameResult result{};
    if (!shoulder_offset_model.has_value()) return result;
    const auto after = observe_control_identity(camera);
    const auto stable = before.valid && after.valid && before.camera == after.camera &&
        before.engine == after.engine && before.player == after.player;
    if (shoulder_generation != control_generation)
    {
        if (shoulder_camera == reinterpret_cast<std::uintptr_t>(camera)) remove_shoulder_overlay(camera);
        shoulder_offset_model->reset();
        shoulder_generation = control_generation;
        shoulder_camera = reinterpret_cast<std::uintptr_t>(camera);
    }
    DWORD foreground_process = 0;
    static_cast<void>(GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process));
    const auto eligible = active_config.shoulder_offset_enabled && stable &&
        foreground_process == GetCurrentProcessId() &&
        phase.load(std::memory_order_acquire) == GdTpcPhase::logging_active &&
        game_state_writes_enabled.load(std::memory_order_acquire) != 0 &&
        camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person);
    gdtpc::CollisionVec3 current{};
    float yaw = 0.0F;
    if (!guarded_read_target_offset(camera, current)) return result;
    __try { std::memcpy(&yaw, static_cast<const std::byte*>(camera) + 0x0c, sizeof(yaw)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return result; }
    const auto key_down = active_config.shoulder_offset_enabled && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const auto decision = shoulder_offset_model->step(current, yaw, active_config.shoulder_offset_units,
        active_config.shoulder_height_units, eligible, key_down);
    result.force_query = decision.force_query;
    if (apply_shoulder_decision(camera, decision)) result.translation = decision.translation;
    return result;
}

float read_target_blend_or(const void* camera, const float fallback) noexcept
{
    auto blend = 0.0F;
    return guarded_read_target_blend(camera, blend) ? blend : fallback;
}

// Hands the engine's zoom target back to the player after the held method parked it at the arm.
// Without this the engine has nothing to animate toward and the camera stays pinned in after the
// obstruction has gone. Called on release, on ineligibility, and on a mid-decision bail.
void release_held_target(void* camera) noexcept
{
    if (!collision_holding_target) return;
    collision_holding_target = false;
    if (!zoom_step_model.has_value() || !zoom_step_model->has_player_blend()) return;

    // If the target no longer holds what we last put there, the engine has moved it, which means the
    // player scrolled since the last callback. That click is theirs: writing over it here would
    // swallow it exactly as the native setter used to.
    float current = 0.0F;
    if (guarded_read_target_blend(camera, current) &&
        std::abs(current - zoom_step_model->field_blend()) > 1.0e-4F)
        return;

    const auto player_blend = zoom_step_model->player_blend();
    if (guarded_write_target_blend(camera, player_blend)) zoom_step_model->note_field_write(player_blend);
}

// Parks the engine's zoom target on the arm, so it has nothing to animate toward and the camera stays
// exactly where collision put it.
//
// This runs on EVERY frame the arm is shortened, not only on frames that write the camera position.
// Position writes are suppressed while an unchanged arm holds an unchanged obstruction, which is most
// frames at a wall; tying the hold to them left the field handed back on two thirds of held frames and
// the shimmer came straight back.
void hold_target_at_arm(void* camera, const float arm) noexcept
{
    if (!zoom_step_model.has_value() || !zoom_step_model->has_player_blend()) return;
    float endpoint_a = 0.0F, endpoint_b = 0.0F;
    if (!guarded_read_zoom_endpoints(camera, endpoint_a, endpoint_b)) return;
    const auto blend = (arm - endpoint_a) / (endpoint_b - endpoint_a);
    if (!std::isfinite(blend) || blend < 0.0F || blend > 1.0F) return;
    if (!guarded_write_target_blend(camera, blend)) return;
    zoom_step_model->note_field_write(blend);
    collision_holding_target = true;
}

// Recorded when a structured exception escapes the collision write, so the trace distinguishes a
// refused write from a faulting one. Deliberately outside the CollisionZoomResult range.
constexpr std::uint32_t collision_write_faulted = 0xFFFFFFFFU;

// The SEH frame is its own function because an object with a destructor, which the memory access is,
// cannot live in a function containing __try.
bool guarded_collision_zoom_call(gdtpc::CameraMemoryAccess& access, void* camera, const float distance,
    gdtpc::CollisionZoomResult& result) noexcept
{
    __try
    {
        result = gdtpc::write_collision_zoom(access, camera, distance);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_restore_player_zoom_call(gdtpc::CameraMemoryAccess& access, void* camera,
    const float distance, const float player_blend, gdtpc::CollisionZoomResult& result) noexcept
{
    __try
    {
        result = gdtpc::restore_player_zoom(access, camera, distance, player_blend);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_read_float(const void* camera, const std::size_t offset, float& value) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        std::memcpy(&value, static_cast<const std::byte*>(camera) + offset, sizeof(value));
        return std::isfinite(value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_read_eye_offset(const void* camera, gdtpc::CollisionVec3& value) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        std::memcpy(&value, static_cast<const std::byte*>(camera) + 0x60, sizeof(value));
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// WorldCamera+0x60 is added to the calculated eye after our callback; prohibited for shoulder framing, used
// here only to move the eye along its own arm.
bool guarded_write_eye_offset(void* camera, const gdtpc::CollisionVec3& value) noexcept
{
    if (camera == nullptr || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
    __try
    {
        auto* const field = static_cast<std::byte*>(camera) + 0x60;
        std::memcpy(field, &value, sizeof(value));
        gdtpc::CollisionVec3 readback{};
        std::memcpy(&readback, field, sizeof(readback));
        return readback == value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// GameCamera::Update calls WorldCamera::Update and then, when +0x138 is positive, overwrites
// WorldCamera+0x60 with its generated shake vector. That final write replaces the virtual-zoom pull
// after our UpdateFromInputImpl hook. Suppress only the pending native shake branch while a nonzero
// pull is required; native mode and the no-pull end of virtual zoom remain untouched.
bool guarded_suppress_pending_camera_shake(void* camera, bool& suppressed) noexcept
{
    suppressed = false;
    if (camera == nullptr) return false;
    __try
    {
        auto* const remaining = reinterpret_cast<std::int32_t*>(static_cast<std::byte*>(camera) + 0x138);
        if (!gdtpc::camera_shake_active(*remaining)) return true;
        *remaining = 0;
        if (*remaining != 0) return false;
        suppressed = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The six profile fields GameCamera::UpdatePitch reads, as the engine currently holds them.
bool guarded_read_pitch_profile(const void* camera, gdtpc::PitchProfileFields& profile) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        const auto* bytes = static_cast<const std::byte*>(camera);
        std::memcpy(&profile.distance_default, bytes + 0x10c, sizeof(float));
        std::memcpy(&profile.pitch_default, bytes + 0x110, sizeof(float));
        std::memcpy(&profile.distance_minimum, bytes + 0x570, sizeof(float));
        std::memcpy(&profile.distance_maximum, bytes + 0x574, sizeof(float));
        std::memcpy(&profile.pitch_minimum, bytes + 0x578, sizeof(float));
        std::memcpy(&profile.pitch_maximum, bytes + 0x57c, sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool virtual_zoom_owns_zoom() noexcept
{
    return active_config.virtual_zoom_enabled && virtual_zoom_model.has_value() && !virtual_zoom_fell_back;
}

// Restores the eye offset base if the field still holds our pull, and forgets what +0x94 reflected.
void release_virtual_zoom_view(void* camera) noexcept
{
    virtual_zoom_previous_eye_valid = false;
    virtual_zoom_pull_status.store(0.0F, std::memory_order_relaxed);
    virtual_zoom_arm_status.store(0.0F, std::memory_order_relaxed);
    virtual_zoom_engine_status.store(0.0F, std::memory_order_relaxed);
    if (!virtual_zoom_eye.owned()) return;
    gdtpc::CollisionVec3 current{}, restore{};
    if (!guarded_read_eye_offset(camera, current))
    {
        virtual_zoom_eye.forget(); // unreadable: nothing provably ours to restore
        return;
    }
    if (!virtual_zoom_eye.relinquish(current, restore)) return;
    virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
    if (!guarded_write_eye_offset(camera, restore) && virtual_zoom_model.has_value()) virtual_zoom_model->record_fault();
}

// Puts the engine at the visual distance, target included, through the same validated routine collision
// uses to hand the player's zoom back, then stops holding. The profile switch then remembers V, not E.
void hand_back_engine_zoom(void* camera) noexcept
{
    if (!virtual_zoom_model.has_value()) return;
    float endpoint_a = 0.0F, endpoint_b = 0.0F, blend = 0.0F;
    if (guarded_read_zoom_endpoints(camera, endpoint_a, endpoint_b) &&
        virtual_zoom_model->visual_blend(endpoint_a, endpoint_b, blend))
    {
        const auto distance = endpoint_a + (endpoint_b - endpoint_a) * blend;
        WindowsCameraMemoryAccess access(-1);
        auto result = gdtpc::CollisionZoomResult::invalid_argument;
        if (!guarded_restore_player_zoom_call(access, camera, distance, blend, result) ||
            result != gdtpc::CollisionZoomResult::success)
            virtual_zoom_model->record_fault();
        virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
    }
    virtual_zoom_model->relinquish();
}

bool guarded_write_far_plane(void* camera, const float value) noexcept
{
    if (camera == nullptr || !std::isfinite(value)) return false;
    __try
    {
        auto* const field = reinterpret_cast<float*>(static_cast<std::byte*>(camera) + 0x18);
        *field = value;
        return *field == value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Restores the native far plane if the field still holds our cap.
void release_far_plane(void* camera) noexcept
{
    if (!far_plane_overlay.owned()) return;
    auto current = 0.0F, restore = 0.0F;
    if (!guarded_read_float(camera, 0x18, current)) { far_plane_overlay.forget(); return; }
    if (far_plane_overlay.relinquish(current, restore)) static_cast<void>(guarded_write_far_plane(camera, restore));
}

// After the virtual-zoom view: the cap is re-derived every callback from the native value (tolerant ownership) while
// mouse look applies in third person, and released whenever it does not. At large virtual zooms it preserves the capped
// sector's default-camera scenery depth beyond the target, rather than letting that depth collapse as the eye moves out.
void run_far_plane_cap(void* camera) noexcept
{
    const auto fraction = gdtpc::far_plane_fraction(active_config.third_person_far_plane_percent);
    const auto applies = fraction > 0.0F && mouse_look_pitch_apply &&
        game_state_writes_enabled.load(std::memory_order_acquire) != 0 &&
        control_stop_requested.load(std::memory_order_acquire) == 0 &&
        camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person);
    far_plane_percent_status.store(applies ? active_config.third_person_far_plane_percent : 0U, std::memory_order_relaxed);
    const auto arm = virtual_zoom_frame_holding && virtual_zoom_arm_valid ? virtual_zoom_arm : 0.0F;
    auto current = 0.0F, value = 0.0F;
    if (!applies || fraction <= 0.0F || !guarded_read_float(camera, 0x18, current) ||
        !far_plane_overlay.step(current, fraction, arm, active_config.third_person_camera.distance.initial, value))
    {
        release_far_plane(camera);
        return;
    }
    virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
    if (!guarded_write_far_plane(camera, value)) far_plane_overlay.forget();
}

// Logical stop and recoverable fault: restore the eye offset and stop holding. No engine write; the profile
// switch restores the native zoom bytes on the same callback.
void stop_virtual_zoom(void* camera) noexcept
{
    if (!virtual_zoom_model.has_value()) return;
    virtual_zoom_model->relinquish();
    virtual_zoom_frame_holding = false;
    virtual_zoom_arm_valid = false;
    release_virtual_zoom_view(camera);
    virtual_zoom_state_status.store(static_cast<std::uint32_t>(gdtpc::VirtualZoomState::released), std::memory_order_relaxed);
    release_far_plane(camera);
}

// Moves the camera for collision through the shared, offline-tested adapter routine rather than
// calling the native setter here. That routine captures the player's zoom target, refreshes derived
// state with the native setter, and gives the target back, because the setter owns both blend fields
// and overwrites them. Doing this by hand in the runtime is what let the player's scroll be reset.
bool guarded_set_collision_zoom(void* camera, const float distance) noexcept
{
    if (camera == nullptr || set_camera_zoom == nullptr) return false;

    // The zoom target is deliberately left at the arm, which is what stops the engine animating away
    // from it. That is only safe because ZoomStepModel holds the player's own zoom; without it there
    // would be nothing to hand the camera back to, so the write is refused rather than risking the
    // player's zoom being lost.
    if (!zoom_step_model.has_value() || !zoom_step_model->has_player_blend()) return false;

    WindowsCameraMemoryAccess access(-1);
    auto result = gdtpc::CollisionZoomResult::invalid_argument;
    if (!guarded_collision_zoom_call(access, camera, distance, result))
    {
        collision_write_result.store(collision_write_faulted, std::memory_order_relaxed);
        return false;
    }
    collision_write_result.store(static_cast<std::uint32_t>(result), std::memory_order_relaxed);
    if (result != gdtpc::CollisionZoomResult::success) return false;
    collision_holding_target = true;

    // Tell the stepping model what is in the field now, so the next frame does not mistake our own
    // value for the player scrolling.
    if (zoom_step_model.has_value())
    {
        float blend = 0.0F;
        if (guarded_read_target_blend(camera, blend)) zoom_step_model->note_field_write(blend);
    }
    return true;
}

// Puts the camera back at the player's own zoom and drops the override, so anything that reads the
// camera afterwards sees the player's choice rather than an arm pulled in by geometry.
void undo_override_before_toggle(void* camera) noexcept
{
    // Virtual zoom: the engine is held far, which is not the player's zoom. Put the engine at V and drop the
    // eye pull before the profile switch looks, so it remembers V and nothing flashes out to E.
    if (virtual_zoom_owns_zoom())
    {
        if (virtual_zoom_model->holding() &&
            camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person) &&
            game_state_writes_enabled.load(std::memory_order_acquire) != 0)
            hand_back_engine_zoom(camera);
        else virtual_zoom_model->relinquish();
        virtual_zoom_frame_holding = false;
        virtual_zoom_arm_valid = false;
        release_virtual_zoom_view(camera);
        if (collision_model.has_value()) collision_model->abandon_override();
        return;
    }
    if (!collision_model.has_value() || !zoom_step_model.has_value() ||
        !zoom_step_model->has_player_blend())
        return;
    if (camera_mode.load(std::memory_order_acquire) !=
        static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person))
        return;
    if (game_state_writes_enabled.load(std::memory_order_acquire) == 0) return;

    float endpoint_a = 0.0F, endpoint_b = 0.0F;
    if (!guarded_read_zoom_endpoints(camera, endpoint_a, endpoint_b)) return;
    const auto player_blend = zoom_step_model->player_blend();
    const auto distance = endpoint_a + (endpoint_b - endpoint_a) * player_blend;

    WindowsCameraMemoryAccess access(-1);
    auto result = gdtpc::CollisionZoomResult::invalid_argument;
    if (guarded_restore_player_zoom_call(access, camera, distance, player_blend, result) &&
        result == gdtpc::CollisionZoomResult::success)
    {
        zoom_step_model->note_field_write(player_blend);
        collision_holding_target = false;
        collision_model->abandon_override();
    }
}

// Cached once: the counter frequency is fixed for the life of the process.
std::uint64_t performance_frequency() noexcept
{
    static const std::uint64_t value = [] {
        LARGE_INTEGER frequency{};
        return QueryPerformanceFrequency(&frequency) ? static_cast<std::uint64_t>(frequency.QuadPart)
                                                     : std::uint64_t{0};
    }();
    return value;
}

void publish_virtual_zoom_status(const gdtpc::VirtualZoomState state) noexcept
{
    virtual_zoom_state_status.store(static_cast<std::uint32_t>(state), std::memory_order_relaxed);
    if (!virtual_zoom_model.has_value()) return;
    virtual_zoom_visual_status.store(virtual_zoom_model->visual_distance(), std::memory_order_relaxed);
    virtual_zoom_clicks.store(virtual_zoom_model->click_count(), std::memory_order_relaxed);
    virtual_zoom_faults.store(virtual_zoom_model->fault_count(), std::memory_order_relaxed);
}

// Zoom intent, after mouse look and shoulder and before collision. Holds the engine at E while mouse look
// applies (captured, Alt or menu in third person), turns the engine's scroll clicks into V, and hands the
// engine back to V when holding ends for any reason other than a stop.
void run_virtual_zoom_intent(void* camera, const ObservedControlIdentity& before) noexcept
{
    virtual_zoom_frame_holding = false;
    if (!virtual_zoom_owns_zoom())
    {
        if (virtual_zoom_model.has_value())
            publish_virtual_zoom_status(virtual_zoom_fell_back ? gdtpc::VirtualZoomState::latched_off
                                                               : gdtpc::VirtualZoomState::disabled);
        return;
    }
    const auto after = observe_control_identity(camera);
    const auto stable = before.valid && after.valid && before.camera == after.camera &&
        before.engine == after.engine && before.player == after.player;
    if (virtual_zoom_generation != control_generation)
    {
        // A new session: nothing held belongs to it. The eye offset is only restored on the same camera
        // object, and only if it still holds our pull.
        if (virtual_zoom_camera == reinterpret_cast<std::uintptr_t>(camera)) release_virtual_zoom_view(camera);
        else { virtual_zoom_eye.forget(); virtual_zoom_previous_eye_valid = false; }
        virtual_zoom_model->reset_session();
        virtual_zoom_arm_valid = false;
        virtual_zoom_generation = control_generation;
        virtual_zoom_camera = reinterpret_cast<std::uintptr_t>(camera);
    }

    const auto third_person_writable = stable &&
        phase.load(std::memory_order_acquire) == GdTpcPhase::logging_active &&
        game_state_writes_enabled.load(std::memory_order_acquire) != 0 &&
        control_stop_requested.load(std::memory_order_acquire) == 0 &&
        recoverable_fault_request.load(std::memory_order_acquire) == 0 &&
        camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person);
    float target = 0.0F, endpoint_a = 0.0F, endpoint_b = 0.0F;
    const auto readable = guarded_read_zoom_endpoints(camera, endpoint_a, endpoint_b) &&
        guarded_read_float(camera, 0x584, target);
    // mouse_look_pitch_apply is this callback's decision: captured, Alt or menu, which already requires
    // third person, a stable identity, foreground, readable menus and enabled writes.
    const auto applies = mouse_look_pitch_apply && third_person_writable && readable;
    const auto decision = virtual_zoom_model->step(applies, target, endpoint_a, endpoint_b);

    if (decision.release)
    {
        // Release (alt-tab, load, latch): put the engine at V so the view does not jump out to E. Only on a
        // camera that is still provably ours; otherwise the profile switch owns what happens next.
        if (third_person_writable)
        {
            WindowsCameraMemoryAccess access(-1);
            auto result = gdtpc::CollisionZoomResult::invalid_argument;
            const auto distance = endpoint_a + (endpoint_b - endpoint_a) * decision.release_blend;
            virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
            if (!guarded_restore_player_zoom_call(access, camera, distance, decision.release_blend, result) ||
                result != gdtpc::CollisionZoomResult::success)
                virtual_zoom_model->record_fault();
        }
    }
    if (decision.acquire)
    {
        // Through the validated setter path: SetZoom moves current and target blend together, so the engine
        // is at E this very callback and never animates there (an animating D would push the eye out).
        WindowsCameraMemoryAccess access(-1);
        auto result = gdtpc::CollisionZoomResult::invalid_argument;
        virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
        const auto called = guarded_collision_zoom_call(access, camera, active_config.virtual_zoom_engine_distance, result);
        float observed = 0.0F;
        const auto verified = called && result == gdtpc::CollisionZoomResult::success &&
            guarded_read_float(camera, 0x584, observed);
        virtual_zoom_model->record_acquire_result(verified, observed);
        if (verified && virtual_zoom_model->holding()) virtual_zoom_arm_valid = false; // collision sets A this frame
    }
    else if (decision.write_target)
    {
        virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
        virtual_zoom_model->record_target_write_result(guarded_write_target_blend(camera, decision.target_blend));
    }

    if (virtual_zoom_model->latched() && !virtual_zoom_model->holding())
    {
        // The accepted non-virtual path takes over for the rest of the process. Its stepping model must
        // adopt the engine's zoom afresh rather than trust anything from before virtual zoom held it.
        virtual_zoom_fell_back = true;
        if (zoom_step_model.has_value()) zoom_step_model->reset();
        release_virtual_zoom_view(camera);
        publish_virtual_zoom_status(gdtpc::VirtualZoomState::latched_off);
        return;
    }

    LARGE_INTEGER counter{};
    static_cast<void>(QueryPerformanceCounter(&counter));
    const auto tick = static_cast<std::uint64_t>(counter.QuadPart);
    const auto frequency = performance_frequency();
    const auto delta_seconds = frequency > 0 && virtual_zoom_last_tick != 0 && tick > virtual_zoom_last_tick
        ? static_cast<float>(static_cast<double>(tick - virtual_zoom_last_tick) / static_cast<double>(frequency)) : 0.0F;
    virtual_zoom_last_tick = tick;
    static_cast<void>(virtual_zoom_model->advance(delta_seconds));

    virtual_zoom_frame_holding = virtual_zoom_model->holding();
    if (!virtual_zoom_frame_holding)
    {
        virtual_zoom_arm_valid = false;
        release_virtual_zoom_view(camera);
    }
    publish_virtual_zoom_status(virtual_zoom_frame_holding ? gdtpc::VirtualZoomState::holding : decision.state);
}

// After collision and the pitch re-application: pull the eye from D to A along this frame's final yaw and
// pitch, and record what +0x94 will reflect for next frame's collision origin.
void apply_virtual_zoom_view(void* camera) noexcept
{
    if (!virtual_zoom_owns_zoom()) return;
    if (!virtual_zoom_frame_holding || !virtual_zoom_arm_valid)
    {
        release_virtual_zoom_view(camera);
        return;
    }
    float engine = 0.0F, yaw = 0.0F, pitch = 0.0F;
    gdtpc::CollisionVec3 current{}, pull{}, value{};
    if (!guarded_read_float(camera, 0x08, engine) || !guarded_read_float(camera, 0x0c, yaw) ||
        !guarded_read_float(camera, 0x10, pitch) || !guarded_read_eye_offset(camera, current) ||
        !gdtpc::eye_pull(engine, virtual_zoom_arm, yaw, pitch, pull))
    {
        // A stale pull left in the field would misplace both the view and next frame's collision origin.
        release_virtual_zoom_view(camera);
        return;
    }
    bool shake_suppressed = false;
    if (engine - virtual_zoom_arm > gdtpc::EyeOffsetOverlay::tolerance)
    {
        if (!guarded_suppress_pending_camera_shake(camera, shake_suppressed))
        {
            virtual_zoom_model->record_fault();
            release_virtual_zoom_view(camera);
            return;
        }
        if (shake_suppressed)
        {
            camera_shake_suppressions.fetch_add(1, std::memory_order_relaxed);
            virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const auto eye_composed = shake_suppressed && virtual_zoom_eye.owned()
        ? virtual_zoom_eye.recompose(pull, value) : virtual_zoom_eye.step(current, pull, value);
    if (!eye_composed)
    {
        release_virtual_zoom_view(camera);
        return;
    }
    virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
    if (!guarded_write_eye_offset(camera, value))
    {
        virtual_zoom_eye.forget();
        virtual_zoom_previous_eye_valid = false;
        virtual_zoom_model->record_fault();
        virtual_zoom_pull_status.store(0.0F, std::memory_order_relaxed);
        return;
    }
    const auto direction = gdtpc::eye_direction(yaw, pitch);
    virtual_zoom_previous_eye = {engine * direction.x + pull.x, engine * direction.y + pull.y,
        engine * direction.z + pull.z};
    virtual_zoom_previous_eye_valid = true;
    virtual_zoom_engine_status.store(engine, std::memory_order_relaxed);
    virtual_zoom_arm_status.store(virtual_zoom_arm, std::memory_order_relaxed);
    virtual_zoom_pull_status.store(engine - virtual_zoom_arm, std::memory_order_relaxed);
}

void publish_collision_status(const gdtpc::CollisionDecision& decision) noexcept
{
    if (!collision_model.has_value()) return;
    collision_arm.store(decision.arm, std::memory_order_relaxed);
    collision_desired.store(decision.desired, std::memory_order_relaxed);
    collision_state.store(static_cast<std::uint32_t>(collision_model->state()), std::memory_order_release);
    collision_query_count.store(collision_model->query_count(), std::memory_order_relaxed);
    collision_hit_count.store(collision_model->hit_count(), std::memory_order_relaxed);
    collision_fault_count.store(collision_model->fault_count(), std::memory_order_relaxed);
}

void run_collision_control(void* camera, const ObservedControlIdentity& before,
    const gdtpc::CollisionVec3& camera_translation, const bool force_query) noexcept
{
    if (!collision_model.has_value()) return;
    const auto after = observe_control_identity(camera);
    const auto stable = before.valid && after.valid && before.camera == after.camera &&
        before.engine == after.engine && before.player == after.player;
    const auto generation = control_generation;

    // The timestep comes from the performance counter, NOT GetTickCount64, whose real resolution is
    // about 15.6 ms. At 60 frames per second that counter reports 0 ms on most frames and 15 or 16 on
    // the rest, so the arm advanced in jumps of one full 16 ms step every other frame instead of moving
    // a little each frame. The engine's own zoom animation used to smooth that away; holding the target
    // removes the smoothing and the stutter becomes visible while the arm extends.
    LARGE_INTEGER counter{};
    static_cast<void>(QueryPerformanceCounter(&counter));
    const auto tick = static_cast<std::uint64_t>(counter.QuadPart);
    if (generation != collision_generation)
    {
        collision_model->reset_session();
        collision_generation = generation;
        collision_last_tick_ms = tick;
    }
    const auto elapsed_ticks = tick >= collision_last_tick_ms ? tick - collision_last_tick_ms : 0;
    collision_last_tick_ms = tick;
    const auto frequency = performance_frequency();
    const auto delta_seconds = frequency > 0
        ? static_cast<float>(static_cast<double>(elapsed_ticks) / static_cast<double>(frequency))
        : 0.0F;

    DWORD foreground_process = 0;
    static_cast<void>(GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process));
    const auto foreground = foreground_process == GetCurrentProcessId();

    // Virtual zoom owns scroll and the engine zoom: the stepping model must not reinterpret its held target,
    // and collision shortens the visual distance instead of writing the engine.
    const auto virtual_mode = virtual_zoom_owns_zoom();

    // Finer zoom stepping first, so the player's zoom is settled before collision reads it.
    if (!virtual_mode && active_config.zoom_step_enabled && stable && foreground &&
        phase.load(std::memory_order_acquire) == GdTpcPhase::logging_active &&
        game_state_writes_enabled.load(std::memory_order_acquire) != 0 &&
        camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person))
    {
        if (generation != zoom_step_generation)
        {
            zoom_step_model->reset();
            zoom_step_generation = generation;
        }
        static_cast<void>(guarded_apply_zoom_step(camera));
    }
    // Deliberately NOT reset here. Losing eligibility, by alt-tabbing or leaving third person, is not
    // a reason to forget the player's zoom, and forgetting it while the target field was held at the
    // arm made the arm itself be re-learned as their choice: their 9.94 became the wall's 5.39.
    // Only a new control generation forgets, which is handled above.

    float observed_distance = 0.0F;
    float player_distance = 0.0F;
    // Both are needed and they are not the same thing: the player's zoom governs how far the arm may
    // extend, while the observed distance says whether the camera is still where we put it.
    auto readable_distance = !virtual_mode && stable && guarded_read_camera_distance(camera, observed_distance) &&
        guarded_read_player_zoom(camera, player_distance);
    if (virtual_mode)
    {
        // The desired arm is the (glided) visual distance, so the ray is only as long as what is seen: the
        // ~85-unit engine arm snapped into hills live. No observed distance, because nothing is written.
        observed_distance = std::numeric_limits<float>::quiet_NaN();
        player_distance = virtual_zoom_model->smoothed_distance();
        readable_distance = stable && virtual_zoom_frame_holding && std::isfinite(player_distance) && player_distance > 0.0F;
    }
    // While the target field may be held at the arm, the field is no longer a statement of the
    // player's zoom. The stepping model is, so it is preferred whenever it has one.
    else if (readable_distance && zoom_step_model.has_value() && zoom_step_model->has_player_blend())
    {
        float endpoint_a = 0.0F, endpoint_b = 0.0F;
        if (guarded_read_zoom_endpoints(camera, endpoint_a, endpoint_b))
            player_distance = endpoint_a + (endpoint_b - endpoint_a) * zoom_step_model->player_blend();
        else readable_distance = false;
    }
    const auto eligible = active_config.collision_enabled && readable_distance && foreground &&
        phase.load(std::memory_order_acquire) == GdTpcPhase::logging_active &&
        game_state_writes_enabled.load(std::memory_order_acquire) != 0 &&
        camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person);

    if (force_query) collision_model->force_query_next();
    gdtpc::WindowsLevelQuery query(collision_entries, camera, after.player,
        gdtpc::default_physics_surface, camera_translation);
    // +0x94 still carries last frame's far engine distance and eye pull. Without a recorded displacement
    // (first held frame) there was no pull last frame and the calculated offset is the right subtraction.
    if (virtual_mode && virtual_zoom_previous_eye_valid) query.set_camera_displacement(virtual_zoom_previous_eye);
    const auto decision = collision_model->decide_camera(query, player_distance, observed_distance,
        delta_seconds, eligible);
    publish_collision_status(decision);

    if (virtual_mode)
    {
        // The arm is only ever used, never written: the eye pull renders it after the pitch overlay.
        virtual_zoom_arm_valid = virtual_zoom_frame_holding && std::isfinite(decision.arm) && decision.arm > 0.0F;
        if (virtual_zoom_arm_valid) virtual_zoom_arm = decision.arm;
        return;
    }

    // Release is decided by the model's own state, NEVER by the absence of a write. Writes are also
    // suppressed while an unchanged arm holds an unchanged obstruction, which is most frames at a
    // wall. Conflating the two handed the target back one frame after every hold, so the held method
    // never actually took effect and measured almost the same as the one it was meant to replace.
    //
    // The field was already handed back at the top of the callback, so this only matters if a write
    // happened earlier in this same pass; it is kept because a silently held field is the failure
    // that costs the player their zoom.
    if (decision.state != gdtpc::CollisionState::shortened) release_held_target(camera);
    else hold_target_at_arm(camera, decision.arm);
    if (!decision.write) return;

    // Revalidate identity and foreground after the engine query and immediately before the one
    // allowed collision write. A transition during the query discards the decision.
    const auto write_identity = observe_control_identity(camera);
    DWORD write_foreground_process = 0;
    static_cast<void>(GetWindowThreadProcessId(GetForegroundWindow(), &write_foreground_process));
    if (!write_identity.valid || write_identity.camera != after.camera || write_identity.engine != after.engine ||
        write_identity.player != after.player || write_foreground_process != GetCurrentProcessId() ||
        game_state_writes_enabled.load(std::memory_order_acquire) == 0 ||
        camera_mode.load(std::memory_order_acquire) != static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person))
    {
        // Relinquish, but do not forget the player's zoom: the camera is still sitting at a distance
        // we wrote, so a reset here would re-learn our own value as the player's choice next frame.
        collision_model->abandon_override();
        release_held_target(camera);
        publish_collision_status({});
        return;
    }

    // Count the native setter attempt even if it faults: it may have changed engine-owned derived
    // state before raising, and telemetry must not pretend no write was attempted.
    collision_write_count.fetch_add(1, std::memory_order_relaxed);
    control_write_count.fetch_add(1, std::memory_order_release);
    const auto wrote = guarded_set_collision_zoom(camera, decision.arm);
    collision_model->record_write_result(wrote);
    publish_collision_status(decision);
}
#endif

void reset_profile_control_before_activation() noexcept
{
    game_state_writes_enabled.store(0, std::memory_order_release);
    control_stop_requested.store(0, std::memory_order_release);
    recoverable_fault_request.store(0, std::memory_order_release);
    profile_switch.reset();
#if defined(GDTPC_CAMERA_COLLISION)
    collision_model.reset();
    zoom_step_model.reset();
    shoulder_offset_model.reset();
    virtual_zoom_model.reset();
    collision_entries = {};
    collision_generation = 0;
    zoom_step_generation = 0;
    shoulder_generation = 0;
    shoulder_camera = 0;
    collision_last_tick_ms = 0;
    collision_enabled_status.store(0, std::memory_order_release);
    shoulder_side_status.store(0, std::memory_order_relaxed);
    shoulder_edge_count.store(0, std::memory_order_relaxed);
    shoulder_write_count.store(0, std::memory_order_relaxed);
#endif
    if (control_stop_ack_event != nullptr)
    {
        CloseHandle(control_stop_ack_event);
        control_stop_ack_event = nullptr;
    }
    if (recoverable_fault_ack_event != nullptr)
    {
        CloseHandle(recoverable_fault_ack_event);
        recoverable_fault_ack_event = nullptr;
    }
}
#endif

// Identifies the process's own primary and GUI threads once, before any hook is active. The primary
// thread is the earliest-created thread in this process; the GUI thread is the one owning a visible
// top-level window. Both are read-only queries and neither runs on the camera callback.
BOOL CALLBACK record_window_thread(HWND window, LPARAM) noexcept
{
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;
    game_window_thread_id.store(static_cast<std::uint32_t>(thread), std::memory_order_release);
    return FALSE;
}

void capture_host_thread_identity() noexcept
{
    EnumWindows(record_window_thread, 0);

    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process_id = GetCurrentProcessId();
    ULONGLONG earliest = 0;
    DWORD earliest_thread = 0;
    if (Thread32First(snapshot, &entry) != FALSE)
    {
        do
        {
            if (entry.th32OwnerProcessID != process_id) continue;
            const auto thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr) continue;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user) != FALSE)
            {
                ULARGE_INTEGER value{};
                value.LowPart = created.dwLowDateTime;
                value.HighPart = created.dwHighDateTime;
                if (earliest_thread == 0 || value.QuadPart < earliest)
                {
                    earliest = value.QuadPart;
                    earliest_thread = entry.th32ThreadID;
                }
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    game_main_thread_id.store(static_cast<std::uint32_t>(earliest_thread), std::memory_order_release);
}

// Observation only, isolated from the camera sample so a stale UI pointer during a zone change leaves
// the rest of the row valid. Unreadable candidates keep their all-ones sentinels.
void read_menu_candidates(TelemetrySnapshot& sample) noexcept
{
    __try
    {
        const auto engine = reinterpret_cast<const std::uint8_t*>(sample.game_engine);
        sample.menu_e1858 = *reinterpret_cast<const std::uint64_t*>(engine + 0x1858);
        const auto ui_object = reinterpret_cast<const std::uint8_t*>(sample.ui);
        if (ui_object == nullptr) __leave;
        sample.menu_ui9918 = *reinterpret_cast<const std::uint64_t*>(ui_object + 0x9918);
        const auto window_state = *reinterpret_cast<const std::uint8_t* const*>(ui_object + 0x1c20);
        if (window_state != nullptr) sample.menu_flag = window_state[0x84];
        // Per-panel candidates from the 2026-09-13 probe: each qword was constant across all 27 closed
        // records and changed only for specific panels. Bit set = differs from that closed value.
        // The live mouse-look session showed menu_flag is cursor-over-panel, not panel-open.
        struct Candidate { std::uint32_t offset; std::uint64_t closed; };
        static constexpr Candidate candidates[] = {
            {0x770, 0}, {0x95d0, 0}, {0x3218, 0}, {0x1fb8, 0}, {0x3a80, 1}, {0x3b48, 0}, {0x9918, 0},
            {0xb7a0, 0}, {0xc110, 0}, {0xa2d8, 0}, {0x7378, 0}, {0x17a8, 0}, {0x9c58, 1}, {0x9f98, 0}, {0xaa8, 0}};
        std::uint32_t mask = 0;
        for (std::uint32_t bit = 0; bit < std::size(candidates); ++bit)
            if (*reinterpret_cast<const std::uint64_t*>(ui_object + candidates[bit].offset) != candidates[bit].closed)
                mask |= 1U << bit;
        sample.panel_candidates = mask;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try
    {
        const auto player = reinterpret_cast<const std::uint8_t*>(sample.player);
        if (player == nullptr) __leave;
        sample.combat_state = *reinterpret_cast<const std::uint32_t*>(player + 0x690);
        sample.combat_aux = *reinterpret_cast<const std::uint32_t*>(player + 0x694);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void capture_camera_state(void* camera_pointer, const gdtpc::CallbackEvidenceSnapshot& evidence) noexcept
{
    const auto callbacks = callback_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!telemetry_queue.try_acquire_producer())
    {
        telemetry_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    TelemetrySnapshot sample{};
    sample.tick_ms = GetTickCount64();
    sample.callbacks = callbacks;
    sample.camera = reinterpret_cast<std::uintptr_t>(camera_pointer);
    sample.callback_evidence = evidence;
    sample.camera_mode = camera_mode.load(std::memory_order_acquire);
    sample.restore_state = restore_state.load(std::memory_order_acquire);
    sample.writes_enabled = game_state_writes_enabled.load(std::memory_order_acquire);
    sample.control_writes = control_write_count.load(std::memory_order_acquire);
    sample.restore_writes = restore_write_count.load(std::memory_order_acquire);
    sample.collision_arm = collision_arm.load(std::memory_order_relaxed);
    sample.collision_desired = collision_desired.load(std::memory_order_relaxed);
    sample.collision_state = collision_state.load(std::memory_order_acquire);
    sample.collision_queries = collision_query_count.load(std::memory_order_relaxed);
    sample.collision_hits = collision_hit_count.load(std::memory_order_relaxed);
    sample.collision_faults = collision_fault_count.load(std::memory_order_relaxed);
    sample.collision_writes = collision_write_count.load(std::memory_order_relaxed);
    sample.toggle_down = toggle_down_status.load(std::memory_order_relaxed);
    sample.toggle_edges = toggle_edge_count.load(std::memory_order_relaxed);
    sample.profile_transition = profile_transition_status.load(std::memory_order_relaxed);
    sample.collision_write_result = collision_write_result.load(std::memory_order_relaxed);
    sample.zoom_step_clicks = zoom_step_clicks.load(std::memory_order_relaxed);
    sample.shoulder_side = shoulder_side_status.load(std::memory_order_relaxed);
    sample.shoulder_edges = shoulder_edge_count.load(std::memory_order_relaxed);
    sample.shoulder_writes = shoulder_write_count.load(std::memory_order_relaxed);
#if defined(GDTPC_CAMERA_COLLISION)
    sample.mouse_look_state = mouse_look_state_status.load(std::memory_order_relaxed);
    sample.aim_y = mouse_look_aim_y.load(std::memory_order_relaxed);
    sample.yaw_delta = mouse_look_yaw_delta.load(std::memory_order_relaxed);
    sample.cursor_warps = mouse_look_warps.load(std::memory_order_relaxed);
    sample.cursor_escapes = mouse_look_escapes.load(std::memory_order_relaxed);
    sample.mouse_yaw_writes = mouse_look_yaw_writes.load(std::memory_order_relaxed);
    sample.pitch_offset = mouse_look_pitch_status.load(std::memory_order_relaxed);
    sample.pitch_writes = mouse_look_pitch_writes.load(std::memory_order_relaxed);
    sample.visual_distance = virtual_zoom_visual_status.load(std::memory_order_relaxed);
    sample.visual_arm = virtual_zoom_arm_status.load(std::memory_order_relaxed);
    sample.eye_pull = virtual_zoom_pull_status.load(std::memory_order_relaxed);
    sample.engine_distance = virtual_zoom_engine_status.load(std::memory_order_relaxed);
    sample.virtual_zoom_state = virtual_zoom_state_status.load(std::memory_order_relaxed);
    sample.virtual_zoom_clicks = virtual_zoom_clicks.load(std::memory_order_relaxed);
    sample.virtual_zoom_faults = virtual_zoom_faults.load(std::memory_order_relaxed);
    sample.camera_shake_suppressions = camera_shake_suppressions.load(std::memory_order_relaxed);
    sample.far_plane_percent = far_plane_percent_status.load(std::memory_order_relaxed);
    sample.npc_talk = npc_talk_status.load(std::memory_order_relaxed);
    sample.controller_state_rva = controller_state_rva_status.load(std::memory_order_relaxed);
    sample.dot_cursor = dot_cursor_status.load(std::memory_order_relaxed);
    sample.cursor_visibility = cursor_visibility_status.load(std::memory_order_relaxed);
    sample.cursor_owner = cursor_owner_thread.load(std::memory_order_relaxed);
    sample.cursor_thread = cursor_apply_thread.load(std::memory_order_relaxed);
    sample.panel_open_flags = panel_open_flags_status.load(std::memory_order_relaxed);
    sample_controller_event_status(sample);
    sample.right_stick_y = right_stick_y_status.load(std::memory_order_relaxed);
    sample.stick_pitch_delta = stick_pitch_status.load(std::memory_order_relaxed);
#endif
    sample.abandoned_excursions = abandoned_excursions.load(std::memory_order_acquire);
    DWORD foreground_process = 0;
    static_cast<void>(GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process));
    sample.foreground = foreground_process == GetCurrentProcessId() ? 1U : 0U;
    sample.main_thread_id = game_main_thread_id.load(std::memory_order_relaxed);
    sample.window_thread_id = game_window_thread_id.load(std::memory_order_relaxed);

    __try
    {
        if (camera_pointer == nullptr) __leave;
        const auto camera = static_cast<const std::uint8_t*>(camera_pointer);
        sample.world_yaw = *reinterpret_cast<const float*>(camera + 0x0c);
        sample.world_pitch = *reinterpret_cast<const float*>(camera + 0x10);
        sample.world_fov = *reinterpret_cast<const float*>(camera + 0x14);
        sample.requested_yaw = *reinterpret_cast<const float*>(camera + 0x59c);
        sample.zoom_blend = *reinterpret_cast<const float*>(camera + 0x580);
        sample.zoom_target_blend = *reinterpret_cast<const float*>(camera + 0x584);
        sample.zoom_a = *reinterpret_cast<const float*>(camera + 0x590);
        sample.zoom_b = *reinterpret_cast<const float*>(camera + 0x594);
        // Both candidate camera-position sources, read only. Camera::GetRayThroughImagePoint builds
        // its ray origin from +0x28; WorldCamera::GetRayThroughImagePoint uses +0x94. Which one this
        // GameCamera exposes is an empirical question this build exists to answer. Nonfinite values
        // are reported as-is rather than rejecting the sample, because being wrong is the finding.
        sample.camera_local_x = *reinterpret_cast<const float*>(camera + 0x28);
        sample.camera_local_y = *reinterpret_cast<const float*>(camera + 0x2c);
        sample.camera_local_z = *reinterpret_cast<const float*>(camera + 0x30);
        sample.camera_world_x = *reinterpret_cast<const float*>(camera + 0x94);
        sample.camera_world_y = *reinterpret_cast<const float*>(camera + 0x98);
        sample.camera_world_z = *reinterpret_cast<const float*>(camera + 0x9c);
        // Retain the rejected first candidate's raw eye offset for diagnosis. Live, writing +0x60
        // moved +0x94 while keeping the original target, producing orbit/yaw-like framing.
        sample.camera_offset_x = *reinterpret_cast<const float*>(camera + 0x60);
        sample.camera_offset_y = *reinterpret_cast<const float*>(camera + 0x64);
        sample.camera_offset_z = *reinterpret_cast<const float*>(camera + 0x68);
        // GameCamera::GetTarget adds this vector to the tracked target before WorldCamera derives the
        // eye position. This is the owned field used by the corrected shoulder candidate.
        sample.target_offset_x = *reinterpret_cast<const float*>(camera + 0x55c);
        sample.target_offset_y = *reinterpret_cast<const float*>(camera + 0x560);
        sample.target_offset_z = *reinterpret_cast<const float*>(camera + 0x564);
        sample.far_plane = *reinterpret_cast<const float*>(camera + 0x18);
        sample.render_far_plane = *reinterpret_cast<const float*>(camera + 0xb0);
        if (!std::isfinite(sample.world_yaw) || !std::isfinite(sample.world_pitch) || !std::isfinite(sample.world_fov) ||
            !std::isfinite(sample.requested_yaw) ||
            !std::isfinite(sample.zoom_blend) || !std::isfinite(sample.zoom_target_blend) ||
            !std::isfinite(sample.zoom_a) || !std::isfinite(sample.zoom_b)) __leave;

        if (game_engine_slot != nullptr)
        {
            const auto engine = static_cast<const std::uint8_t*>(*game_engine_slot);
            sample.game_engine = reinterpret_cast<std::uintptr_t>(engine);
            if (engine != nullptr)
            {
                sample.ui = reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void* const*>(engine + 0x19b0));
                const auto dialog_begin = *reinterpret_cast<void* const*>(engine + 0x38);
                const auto dialog_end = *reinterpret_cast<void* const*>(engine + 0x40);
                sample.dialog_active = dialog_begin != dialog_end ? 1U : 0U;
                sample.action_set = *reinterpret_cast<const std::uint32_t*>(engine + 0x376f8);
                sample.input_mode = *reinterpret_cast<const std::uint32_t*>(engine + 0x37718);
                sample.view_distance_locked = *reinterpret_cast<const std::uint8_t*>(engine + 0x1b40);
            }
        }

        const auto player = *reinterpret_cast<void* const*>(camera + 0x118);
        sample.player = reinterpret_cast<std::uintptr_t>(player);
        if (player != nullptr)
        {
            const auto entity = static_cast<const std::uint8_t*>(player);
            sample.facing_x = *reinterpret_cast<const float*>(entity + 0x108);
            sample.facing_y = *reinterpret_cast<const float*>(entity + 0x10c);
            sample.facing_z = *reinterpret_cast<const float*>(entity + 0x110);
            const auto length = sample.facing_x * sample.facing_x + sample.facing_z * sample.facing_z;
            if (std::isfinite(sample.facing_x) && std::isfinite(sample.facing_y) && std::isfinite(sample.facing_z) &&
                length >= 0.0001f && length <= 4.0f)
            {
                sample.facing_heading = std::atan2(sample.facing_z, sample.facing_x);
                sample.facing_valid = 1;
            }
        }
        sample.sample_valid = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        sample.sample_valid = 0;
        sample.facing_valid = 0;
    }
    if (sample.game_engine != 0) read_menu_candidates(sample);

    const auto old_camera = session_camera.load(std::memory_order_relaxed);
    const auto old_engine = session_engine.load(std::memory_order_relaxed);
    const auto old_player = session_player.load(std::memory_order_relaxed);
    if (sample.sample_valid != 0 && (old_camera != sample.camera || old_engine != sample.game_engine || old_player != sample.player))
    {
        session_camera.store(sample.camera, std::memory_order_relaxed);
        session_engine.store(sample.game_engine, std::memory_order_relaxed);
        session_player.store(sample.player, std::memory_order_relaxed);
        sample.generation = session_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    else sample.generation = session_generation.load(std::memory_order_relaxed);

    if (telemetry_queue.publish(sample)) telemetry_published.fetch_add(1, std::memory_order_relaxed);
    else telemetry_dropped.fetch_add(1, std::memory_order_relaxed);
    telemetry_queue.release_producer();
}

#if defined(GDTPC_CAMERA_COLLISION)
// Panel-open rule from the structured 2026-09-13 session (IMPLEMENTATION_PLAN.md record "Structured panel
// session"). Each qword is per panel and was consistent across two sessions:
//   0x95d0 inventory/character (also vendor, stash)   0x9918 quest log / NPC dialog
//   0x9f98 skills   0xa2d8 map   0xc110 vendor   0x17a8/0x9c58 Escape menu
// The marked 2026-09-15 session added 0xafd8 Factions and 0x8b28 Loot Filter. As with the older
// panel fields, the validated flag is byte +2 of each aligned qword.
// NOT used: *(ui+0x1c20)+0x84 is cursor-over-panel, and 0x7378 fires in bursts during combat.
// `readable` is false at the main menu and during loads, which mouse look treats as not eligible.
void guarded_read_menu_signal(bool& menu, bool& readable, std::uint32_t& flags) noexcept
{
    flags = 0xFFFFFFFFU;
    menu = false;
    readable = false;
    if (game_engine_slot == nullptr) return;
    __try
    {
        const auto engine = static_cast<const std::uint8_t*>(*game_engine_slot);
        if (engine == nullptr) return;
        const auto ui = *reinterpret_cast<const std::uint8_t* const*>(engine + 0x19b0);
        if (ui == nullptr) return;
        // Flag BYTES, not whole qwords (2026-09-13): open panels read exactly 0x10000 / 0x100, i.e. one byte == 1, and the
        // surrounding bytes can hold uninitialised padding (0x7f0000000000 after a UI rebuild; 0x3503-style masks from the
        // first frame of some launches), which stuck the old non-zero rule on "menu". Vendor 0xc110 was an integer, not a
        // flag, and vendors also raise the inventory flag, so it is not used.
        flags = gdtpc::classify_panel_open_flags(
            ui[0x95d2], ui[0x991a], ui[0x9f9a], ui[0xa2da], ui[0x17a9], ui[0x9c5a],
            ui[0xafda], ui[0x8b2a]);
        menu = flags != 0;
        readable = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { menu = false; readable = false; flags = 0xFFFFFFFFU; }
}

// Adds rotation to requested yaw and verifies it. The engine does not wrap this field at +/-pi (3.8 was
// observed natively), so neither does this; a value beyond the bound is refused rather than wrapped,
// because wrapping could make the interpolated camera spin the long way round.
bool guarded_add_requested_yaw(void* camera, const float delta) noexcept
{
    if (camera == nullptr || !std::isfinite(delta)) return false;
    __try
    {
        auto* const field = reinterpret_cast<float*>(static_cast<std::byte*>(camera) + 0x59c);
        const auto next = *field + delta;
        if (!std::isfinite(next) || std::abs(next) > 1000.0F) return false;
        *field = next;
        return *field == next;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Pulls the rendered yaw (+0x0C) a further fraction toward requested yaw (+0x59C). The engine's own follow
// in GameCamera::Update still runs; this only shortens the glide while mouse look holds the cursor.
bool guarded_catch_up_yaw(void* camera, const float fraction) noexcept
{
    if (camera == nullptr || !(fraction > 0.0F) || fraction >= 1.0F) return false;
    __try
    {
        auto* const world = reinterpret_cast<float*>(static_cast<std::byte*>(camera) + 0x0c);
        const auto requested = *reinterpret_cast<const float*>(static_cast<std::byte*>(camera) + 0x59c);
        if (!std::isfinite(*world) || !std::isfinite(requested)) return false;
        const auto next = *world + (requested - *world) * fraction;
        if (!std::isfinite(next)) return false;
        *world = next;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Ownership (our last write vs a fresh native pitch) lives in gdtpc::PitchOverlay, tolerance-based,
// because the engine's per-frame clamp round-trips the field through degrees.
bool guarded_pitch_overlay(void* camera, const float offset_degrees, const float floor_degrees,
    gdtpc::PitchOverlay& overlay) noexcept
{
    if (camera == nullptr) return false;
    __try
    {
        auto* const field = reinterpret_cast<float*>(static_cast<std::byte*>(camera) + 0x10);
        const auto current = *field;
        if (!std::isfinite(current)) return false;
        const auto target = overlay.step(current, offset_degrees, floor_degrees);
        *field = target;
        return *field == target;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_pitch_overlay_from_base(void* camera, const float base_radians, const float offset_degrees,
    const float floor_degrees, gdtpc::PitchOverlay& overlay) noexcept
{
    if (camera == nullptr || !std::isfinite(base_radians)) return false;
    __try
    {
        auto* const field = reinterpret_cast<float*>(static_cast<std::byte*>(camera) + 0x10);
        const auto target = overlay.step_from_base(base_radians, offset_degrees, floor_degrees);
        if (!std::isfinite(target)) return false;
        *field = target;
        return *field == target;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Called before and after collision. Normally the base is the engine's own pitch in the field (collision's
// native SetZoom can refresh it). While virtual zoom holds the engine far, the engine's pitch belongs to the
// far distance, so the base is pitch_for_distance(visual arm) instead: last frame's arm before collision,
// this frame's after it.
void reapply_mouse_look_pitch(void* camera) noexcept
{
    if (!mouse_look_pitch_apply) return;
    const auto virtual_base = virtual_zoom_model.has_value() && virtual_zoom_model->holding() && virtual_zoom_arm_valid;
    auto base = 0.0F;
    gdtpc::PitchProfileFields profile{};
    const auto wrote = virtual_base
        ? guarded_read_pitch_profile(camera, profile) && gdtpc::pitch_for_distance(profile, virtual_zoom_arm, base) &&
              guarded_pitch_overlay_from_base(camera, base, mouse_look_pitch_offset,
                  active_config.mouse_look.pitch_floor_degrees, mouse_look_pitch_overlay)
        : guarded_pitch_overlay(camera, mouse_look_pitch_offset, active_config.mouse_look.pitch_floor_degrees,
              mouse_look_pitch_overlay);
    if (wrote) mouse_look_pitch_writes.fetch_add(1, std::memory_order_relaxed);
}

void publish_mouse_look(const gdtpc::MouseLookDecision& decision) noexcept
{
    mouse_look_state_status.store(static_cast<std::uint32_t>(decision.state), std::memory_order_relaxed);
    mouse_look_aim_y.store(decision.aim_y, std::memory_order_relaxed);
    mouse_look_yaw_delta.store(decision.yaw_delta, std::memory_order_relaxed);
}

// Stop and recoverable-fault paths, on the camera thread before acknowledgment. Phase 1 holds no clip,
// so releasing means only that no further warp or yaw write can happen until a fresh capture edge.
#include "cursor_visibility_runtime.inl"

void force_release_mouse_look() noexcept
{
    release_cursor_visibility();
    release_dot_cursor();
    if (!mouse_look_model.has_value()) return;
    mouse_look_model->reset();
    mouse_look_warp_failed = false;
    mouse_look_pitch_apply = false;
    mouse_look_pitch_overlay.release();
    mouse_look_pitch_status.store(0.0F, std::memory_order_relaxed);
    mouse_look_state_status.store(static_cast<std::uint32_t>(gdtpc::MouseLookState::ineligible), std::memory_order_relaxed);
    mouse_look_yaw_delta.store(0.0F, std::memory_order_relaxed);
    controller_event_consumed_updates = controller_event_updates.load(std::memory_order_acquire);
    right_stick_y_status.store(0, std::memory_order_relaxed);
    stick_pitch_status.store(0.0F, std::memory_order_relaxed);
}

// The engine's WinWindow for this HWND: GWLP_USERDATA (set by WinWindow::WindowProc on WM_NCCREATE), accepted only when it is
// a committed, writable object whose vtable is the validated WinWindow vtable.
std::uintptr_t guarded_find_win_window(const HWND window) noexcept
{
    if (window == nullptr || win_window_vtable == 0) return 0;
    const auto candidate = static_cast<std::uintptr_t>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (candidate == 0) return 0;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(candidate), &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return 0;
    const auto protection = memory.Protect & 0xff;
    const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    if ((protection != PAGE_READWRITE && protection != PAGE_WRITECOPY) || candidate + 0x30 > region_end) return 0;
    __try
    {
        return *reinterpret_cast<const std::uintptr_t*>(candidate) == win_window_vtable ? candidate : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool guarded_is_win_window(const std::uintptr_t candidate) noexcept
{
    if (candidate == 0 || win_window_vtable == 0) return false;
    __try { return *reinterpret_cast<const std::uintptr_t*>(candidate) == win_window_vtable; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_read_cursor_handle(const std::uintptr_t win_window, std::uintptr_t& handle) noexcept
{
    __try { handle = *reinterpret_cast<const std::uintptr_t*>(win_window + 0x20); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool guarded_write_cursor_handle(const std::uintptr_t win_window, const std::uintptr_t handle) noexcept
{
    __try
    {
        auto* const field = reinterpret_cast<std::uintptr_t*>(win_window + 0x20);
        *field = handle;
        return *field == handle;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Puts the game's cursor back if the window still holds the dot, and shows it at once.
void release_dot_cursor() noexcept
{
    if (!dot_cursor_overlay.owned()) return;
    const auto window = dot_cursor_window;
    std::uintptr_t current = 0, restore = 0;
    // Re-check the object is still the WinWindow before touching it; not via the foreground window, which is some other
    // process on alt-tab, exactly when the game's cursor must be restored.
    const auto still_window = window != 0 && guarded_is_win_window(window);
    if (!still_window || !guarded_read_cursor_handle(window, current))
    {
        dot_cursor_overlay.forget();
        return;
    }
    if (dot_cursor_overlay.relinquish(current, restore) && guarded_write_cursor_handle(window, restore) && restore != 0)
        SetCursor(reinterpret_cast<HCURSOR>(restore));
}

void apply_dot_cursor(const HWND window, const bool captured) noexcept
{
    if (!active_config.mouse_look_dot_cursor || dot_cursor_handle == nullptr)
    {
        dot_cursor_status.store(0, std::memory_order_relaxed);
        return;
    }
    if (!captured)
    {
        release_dot_cursor();
        dot_cursor_status.store(0, std::memory_order_relaxed);
        return;
    }
    const auto win_window = guarded_find_win_window(window);
    if (win_window == 0)
    {
        release_dot_cursor();
        dot_cursor_status.store(2, std::memory_order_relaxed);
        return;
    }
    if (win_window != dot_cursor_window) dot_cursor_overlay.forget();
    dot_cursor_window = win_window;
    std::uintptr_t current = 0, value = 0;
    const auto dot = reinterpret_cast<std::uintptr_t>(dot_cursor_handle);
    if (!guarded_read_cursor_handle(win_window, current) || !dot_cursor_overlay.step(current, dot, value))
    {
        dot_cursor_status.store(2, std::memory_order_relaxed);
        return;
    }
    if (current != value)
    {
        virtual_zoom_write_count.fetch_add(1, std::memory_order_relaxed);
        if (!guarded_write_cursor_handle(win_window, value))
        {
            dot_cursor_overlay.forget();
            dot_cursor_status.store(2, std::memory_order_relaxed);
            return;
        }
        SetCursor(dot_cursor_handle);
    }
    dot_cursor_status.store(1, std::memory_order_relaxed);
}

// One lookup of the player's controller through the game's own id lookup, then a read of its current state. POD only, so
// the structured guard is legal; a raise is reported, never swallowed silently.
gdtpc::PlayerControllerSample guarded_sample_player_controller(void* player, bool& faulted) noexcept
{
    gdtpc::PlayerControllerSample sample{};
    faulted = false;
    if (player == nullptr || object_manager_get == nullptr || object_manager_find == nullptr) return sample;
    __try
    {
        const auto id = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(player) + 0x16c0);
        const auto manager = object_manager_get();
        if (manager == nullptr) __leave;
        const auto controller = static_cast<const std::byte*>(object_manager_find(manager, id));
        if (controller == nullptr) __leave;
        sample.controller_vtable = *reinterpret_cast<const std::uintptr_t*>(controller);
        if (sample.controller_vtable == controller_player_vtable)
        {
            // ControllerAI::GetExecutingState (0x112760): the top temporary state (list head +0x2E8, count +0x2F0, first
            // node's state at +0x10) when any exist, otherwise the current state SetState stores at +0x2A0. The first
            // live build read only the temporary list, which is normally empty, and never saw the talk state.
            sample.state_count = *reinterpret_cast<const std::uint64_t*>(controller + 0x2f0);
            const std::byte* state = nullptr;
            if (sample.state_count != 0)
            {
                const auto head = *reinterpret_cast<const std::byte* const*>(controller + 0x2e8);
                const auto first = *reinterpret_cast<const std::byte* const*>(head);
                state = *reinterpret_cast<const std::byte* const*>(first + 0x10);
            }
            else state = *reinterpret_cast<const std::byte* const*>(controller + 0x2a0);
            if (state != nullptr) sample.top_state_vtable = *reinterpret_cast<const std::uintptr_t*>(state);
        }
        sample.readable = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; sample = {}; }
    return sample;
}

// Samples only in third person with a stable identity; four raised lookups latch the signal off for the process.
gdtpc::NpcTalkSignal sample_npc_talk(void* player, const bool third_person_stable) noexcept
{
    auto signal = gdtpc::NpcTalkSignal::disabled;
    if (!active_config.npc_dialog_releases_cursor) signal = gdtpc::NpcTalkSignal::disabled;
    else if (npc_talk_fault_count >= 4) signal = gdtpc::NpcTalkSignal::faulted;
    else if (!third_person_stable) signal = gdtpc::NpcTalkSignal::unknown;
    else
    {
        bool faulted = false;
        const auto sample = guarded_sample_player_controller(player, faulted);
        if (faulted)
        {
            ++npc_talk_fault_count;
            signal = gdtpc::NpcTalkSignal::faulted;
        }
        else
        {
            signal = gdtpc::classify_npc_talk(sample, controller_player_vtable, talk_to_npc_vtable);
            const auto rva = sample.top_state_vtable - game_module_base;
            controller_state_rva_status.store(sample.top_state_vtable == 0 ? 0U :
                sample.top_state_vtable >= game_module_base && rva < game_module_size ? static_cast<std::uint32_t>(rva) : 0xFFFFFFFFU,
                std::memory_order_relaxed);
        }
    }
    npc_talk_status.store(static_cast<std::uint32_t>(signal), std::memory_order_relaxed);
    return signal;
}

void run_mouse_look(void* camera, const ObservedControlIdentity& before) noexcept
{
    if (!mouse_look_model.has_value()) return;
    if (!active_config.mouse_look.enabled)
    {
        mouse_look_state_status.store(static_cast<std::uint32_t>(gdtpc::MouseLookState::disabled), std::memory_order_relaxed);
        return;
    }
    const auto after = observe_control_identity(camera);
    const auto stable = before.valid && after.valid && before.camera == after.camera &&
        before.engine == after.engine && before.player == after.player;
    if (mouse_look_generation != control_generation)
    {
        mouse_look_model->reset();
        mouse_look_warp_failed = false;
        controller_event_consumed_updates = controller_event_updates.load(std::memory_order_acquire);
        mouse_look_generation = control_generation;
    }

    gdtpc::MouseLookInput input{};
    const auto window = GetForegroundWindow();
    DWORD foreground_process = 0;
    static_cast<void>(GetWindowThreadProcessId(window, &foreground_process));
    const auto foreground = window != nullptr && foreground_process == GetCurrentProcessId();
    bool menu_readable = false;
    std::uint32_t panel_flags = 0xFFFFFFFFU;
    guarded_read_menu_signal(input.menu_raw, menu_readable, panel_flags);
    std::uint32_t input_mode = 0xFFFFFFFFU;
    input.mouse_input_active = stable && guarded_read_mouse_input_mode(after.engine, input_mode) && input_mode == 0;
    input.controller_input_active = stable && input_mode == 2;
    panel_open_flags_status.store(panel_flags, std::memory_order_relaxed);
    // The NPC conversation window is not panel state; the player controller's talk state stands in for it.
    const auto third_person_stable = stable &&
        camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person);
    if (sample_npc_talk(after.player, third_person_stable) == gdtpc::NpcTalkSignal::talking) input.menu_raw = true;
    input.eligible = stable && foreground && menu_readable &&
        phase.load(std::memory_order_acquire) == GdTpcPhase::logging_active &&
        game_state_writes_enabled.load(std::memory_order_acquire) != 0 &&
        control_stop_requested.load(std::memory_order_acquire) == 0 &&
        recoverable_fault_request.load(std::memory_order_acquire) == 0 &&
        camera_mode.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileMode::third_person);
    input.alt_down = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
    input.combat = false; // phase 1: combat state is logged only
    input.native_pitch_valid = mouse_look_pitch_overlay.owned();
    input.native_pitch_degrees = mouse_look_pitch_overlay.native() * 57.29577951308232F;
    RECT client{};
    POINT origin{};
    if (foreground && GetClientRect(window, &client) != FALSE && ClientToScreen(window, &origin) != FALSE)
        input.client = {origin.x + client.left, origin.y + client.top, origin.x + client.right, origin.y + client.bottom};
    POINT cursor{};
    input.cursor_valid = GetCursorPos(&cursor) != FALSE && !mouse_look_warp_failed;
    input.cursor_x = cursor.x;
    input.cursor_y = cursor.y;

    {
        LARGE_INTEGER stick_counter{};
        static_cast<void>(QueryPerformanceCounter(&stick_counter));
        const auto stick_tick = static_cast<std::uint64_t>(stick_counter.QuadPart);
        const auto stick_frequency = performance_frequency();
        const auto stick_dt = stick_frequency > 0 && mouse_look_last_tick != 0 && stick_tick > mouse_look_last_tick
            ? static_cast<float>(static_cast<double>(stick_tick - mouse_look_last_tick) / static_cast<double>(stick_frequency)) : 0.0F;
        TelemetrySnapshot event_sample{};
        sample_controller_event_status(event_sample);
        float event_y = 0.0F;
        if (event_sample.controller_event_updates != controller_event_consumed_updates)
        {
            controller_event_consumed_updates = event_sample.controller_event_updates;
            if (active_config.right_stick_pitch_enabled && input.controller_input_active &&
                event_sample.controller_analog_action == 36)
                event_y = event_sample.controller_analog_y;
        }
        input.pitch_stick_degrees = gdtpc::steam_stick_pitch_degrees(event_y,
            active_config.right_stick_pitch_degrees_per_second, active_config.right_stick_pitch_invert, stick_dt);
        const auto normalized = std::clamp(event_y / 6.0F, -1.0F, 1.0F);
        right_stick_y_status.store(static_cast<std::int32_t>(std::lround(normalized * 32767.0F)), std::memory_order_relaxed);
        stick_pitch_status.store(input.pitch_stick_degrees, std::memory_order_relaxed);
    }
    const auto decision = mouse_look_model->step(input);
    if (decision.captured)
    {
        if (!decision.reanchor && (cursor.x < input.client.left || cursor.x >= input.client.right ||
                                   cursor.y < input.client.top || cursor.y >= input.client.bottom))
            mouse_look_escapes.fetch_add(1, std::memory_order_relaxed);
        if (decision.yaw_delta != 0.0F)
        {
            if (guarded_add_requested_yaw(camera, decision.yaw_delta))
                mouse_look_yaw_writes.fetch_add(1, std::memory_order_relaxed);
        }
        if (decision.warp)
        {
            mouse_look_warp_failed = SetCursorPos(decision.warp_x, decision.warp_y) == FALSE;
            if (!mouse_look_warp_failed) mouse_look_warps.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else mouse_look_warp_failed = false;
    apply_dot_cursor(window, false); // No dot, including when an old INI enables it.
    publish_cursor_capture(window, decision.captured);

    LARGE_INTEGER counter{};
    static_cast<void>(QueryPerformanceCounter(&counter));
    const auto tick = static_cast<std::uint64_t>(counter.QuadPart);
    const auto frequency = performance_frequency();
    const auto delta_seconds = frequency > 0 && mouse_look_last_tick != 0 && tick > mouse_look_last_tick
        ? static_cast<float>(static_cast<double>(tick - mouse_look_last_tick) / static_cast<double>(frequency)) : 0.0F;
    mouse_look_last_tick = tick;
    if (decision.captured)
        static_cast<void>(guarded_catch_up_yaw(camera,
            gdtpc::yaw_catchup_fraction(active_config.mouse_look.yaw_catchup_per_second, delta_seconds)));

    mouse_look_pitch_apply = decision.apply_pitch;
    mouse_look_pitch_offset = decision.pitch_offset_degrees;
    if (!decision.apply_pitch) mouse_look_pitch_overlay.release();
    reapply_mouse_look_pitch(camera);
    mouse_look_pitch_status.store(decision.apply_pitch ? decision.pitch_offset_degrees : 0.0F, std::memory_order_relaxed);
    publish_mouse_look(decision);
}

class WindowsUiProbeMemory final : public gdtpc::UiProbeMemory
{
public:
    bool query(const std::uintptr_t address, const std::size_t wanted, std::size_t& readable,
        bool& private_memory) noexcept override
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0 ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const auto protection = memory.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY) return false;
        const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (region_end <= address) return false;
        const auto available = static_cast<std::size_t>(region_end - address);
        readable = available < wanted ? available : wanted;
        private_memory = memory.Type == MEM_PRIVATE;
        return true;
    }

    bool copy(const std::uintptr_t address, std::uint8_t* const destination, const std::size_t size) noexcept override
    {
        __try
        {
            std::memcpy(destination, reinterpret_cast<const std::uint8_t*>(address), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
};

// Kept separate from run_ui_probe: MSVC refuses __try in a function that also holds an object
// needing unwinding (the probe memory adapter).
bool guarded_read_ui_roots(std::uintptr_t& engine, std::uintptr_t& ui, std::uintptr_t& input_device) noexcept
{
    __try
    {
        engine = reinterpret_cast<std::uintptr_t>(*game_engine_slot);
        if (engine != 0) ui = reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void* const*>(engine + 0x19b0));
        auto* const core_engine = core_engine_slot == nullptr ? nullptr : *core_engine_slot;
        if (core_engine != nullptr && get_input_device != nullptr)
            input_device = reinterpret_cast<std::uintptr_t>(get_input_device(core_engine));
        return engine != 0 && ui != 0 && input_device != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Camera callback, after all control work. Captures only on a rising F10/F11 edge, only while the
// runtime is fully active with a healthy writer and the game in the foreground, and never blocks:
// if the previous record has not been written yet the mark is simply ignored.
void run_ui_probe() noexcept
{
    const auto open_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    const auto closed_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const auto open_edge = open_down && !ui_probe_open_down;
    const auto closed_edge = closed_down && !ui_probe_closed_down;
    ui_probe_open_down = open_down;
    ui_probe_closed_down = closed_down;
    if (!active_config.ui_probe_enabled || open_edge == closed_edge || game_engine_slot == nullptr ||
        core_engine_slot == nullptr || get_input_device == nullptr ||
        phase.load(std::memory_order_acquire) != GdTpcPhase::logging_active ||
        writer_health.load(std::memory_order_acquire) != 1) return;
    DWORD foreground_process = 0;
    static_cast<void>(GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process));
    if (foreground_process != GetCurrentProcessId()) return;

    std::uintptr_t engine = 0;
    std::uintptr_t ui = 0;
    std::uintptr_t input_device = 0;
    if (!guarded_read_ui_roots(engine, ui, input_device)) return;

    auto expected = std::uint32_t{0};
    if (!ui_probe_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
    LARGE_INTEGER counter{};
    static_cast<void>(QueryPerformanceCounter(&counter));
    const gdtpc::UiProbeMark mark{open_edge ? gdtpc::UiProbeLabel::open : gdtpc::UiProbeLabel::closed,
        callback_count.load(std::memory_order_relaxed), static_cast<std::uint64_t>(counter.QuadPart),
        camera_mode.load(std::memory_order_acquire), 1};
    WindowsUiProbeMemory memory;
    const auto length = gdtpc::capture_ui_probe(memory, engine, ui, input_device, mark, gdtpc::UiProbeLimits{},
        ui_probe_buffer, sizeof(ui_probe_buffer));
    ui_probe_length = length;
    ui_probe_state.store(length > 0 ? 2U : 0U, std::memory_order_release);
}

// Telemetry writer only. A failed probe write is counted, never folded into writer health, so the
// accepted telemetry and logical-stop checks are unaffected by the research file.
void flush_ui_probe(const HANDLE file) noexcept
{
    if (ui_probe_state.load(std::memory_order_acquire) != 2) return;
    DWORD written = 0;
    if (file == INVALID_HANDLE_VALUE || ui_probe_length > MAXDWORD ||
        WriteFile(file, ui_probe_buffer, static_cast<DWORD>(ui_probe_length), &written, nullptr) == FALSE ||
        written != ui_probe_length)
        ui_probe_write_failures.fetch_add(1, std::memory_order_relaxed);
    ui_probe_state.store(0, std::memory_order_release);
}

void __fastcall steam_controller_update_logging_hook(void* device, const int elapsed_ms) noexcept
{
    const auto original = steam_controller_update_original;
    if (original == nullptr) return;
    original(device, elapsed_ms); // native handling always runs first and receives untouched arguments
    if (phase.load(std::memory_order_acquire) != GdTpcPhase::logging_active) return;

    std::uint32_t count = 0;
    gdtpc::AnalogEventSample analog{};
    bool faulted = false;
    __try
    {
        if (device == nullptr) __leave;
        const auto bytes = static_cast<const std::uint8_t*>(device);
        const auto begin = *reinterpret_cast<const gdtpc::SteamControllerEvent* const*>(bytes + 0x10);
        const auto end = *reinterpret_cast<const gdtpc::SteamControllerEvent* const*>(bytes + 0x18);
        if (begin == nullptr && end == nullptr) __leave;
        const auto begin_address = reinterpret_cast<std::uintptr_t>(begin);
        const auto end_address = reinterpret_cast<std::uintptr_t>(end);
        if (begin == nullptr || end == nullptr || end_address < begin_address ||
            begin_address % alignof(gdtpc::SteamControllerEvent) != 0 ||
            end_address % alignof(gdtpc::SteamControllerEvent) != 0 ||
            (end_address - begin_address) % sizeof(gdtpc::SteamControllerEvent) != 0)
        {
            faulted = true;
            __leave;
        }
        const auto native_count = static_cast<std::size_t>((end_address - begin_address) / sizeof(gdtpc::SteamControllerEvent));
        if (native_count > 256)
        {
            faulted = true;
            __leave;
        }
        count = static_cast<std::uint32_t>(native_count);
        analog = gdtpc::select_strongest_analog_event(begin, native_count);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        faulted = true;
    }

    controller_event_epoch.fetch_add(1, std::memory_order_acq_rel);
    controller_event_count_status.store(faulted ? 0U : count, std::memory_order_relaxed);
    controller_analog_action_status.store(faulted || !analog.valid ? -1 : analog.action_id, std::memory_order_relaxed);
    controller_analog_x_status.store(faulted || !analog.valid ? 0.0F : analog.x, std::memory_order_relaxed);
    controller_analog_y_status.store(faulted || !analog.valid ? 0.0F : analog.y, std::memory_order_relaxed);
    if (faulted) controller_event_faults.fetch_add(1, std::memory_order_relaxed);
    controller_event_updates.fetch_add(1, std::memory_order_relaxed);
    controller_event_epoch.fetch_add(1, std::memory_order_release);
}

void sample_controller_event_status(TelemetrySnapshot& sample) noexcept
{
    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = controller_event_epoch.load(std::memory_order_acquire);
        if ((before & 1U) != 0) continue;
        sample.controller_event_updates = controller_event_updates.load(std::memory_order_relaxed);
        sample.controller_event_count = controller_event_count_status.load(std::memory_order_relaxed);
        sample.controller_analog_action = controller_analog_action_status.load(std::memory_order_relaxed);
        sample.controller_analog_x = controller_analog_x_status.load(std::memory_order_relaxed);
        sample.controller_analog_y = controller_analog_y_status.load(std::memory_order_relaxed);
        sample.controller_event_faults = controller_event_faults.load(std::memory_order_relaxed);
        const auto after = controller_event_epoch.load(std::memory_order_acquire);
        if (before == after) return;
    }
    sample.controller_event_count = 0;
    sample.controller_analog_action = -1;
    sample.controller_analog_x = 0.0F;
    sample.controller_analog_y = 0.0F;
}
#endif

void __fastcall update_from_input_logging_hook(void* camera) noexcept
{
    const auto original = update_from_input_original;
    if (original == nullptr) return;
#if defined(GDTPC_GATE1_PROFILE_WRITES)
    thread_local bool entered = false;
    if (entered) { original(camera); return; }
    entered = true;
    const auto phase_before = phase.load(std::memory_order_acquire);
    const auto before = (phase_before == GdTpcPhase::logging_active || phase_before == GdTpcPhase::shutdown_pending) ?
        observe_control_identity(camera) : ObservedControlIdentity{};
#endif
    const auto ticket = callback_evidence.enter(GetCurrentThreadId());
    original(camera);
    const auto current_phase = phase.load(std::memory_order_acquire);
#if defined(GDTPC_GATE1_PROFILE_WRITES)
    const auto active = current_phase == GdTpcPhase::logging_active || current_phase == GdTpcPhase::shutdown_pending;
#else
    const auto active = current_phase == GdTpcPhase::logging_active;
#endif
    const auto evidence = callback_evidence.original_returned(ticket, active);
    if (active)
    {
#if defined(GDTPC_GATE1_PROFILE_WRITES)
#if defined(GDTPC_CAMERA_COLLISION)
        // The zoom target is handed back to the player BEFORE anything else looks at the camera, and
        // re-held at the end of the collision pass if an obstruction is still there. The hold then
        // exists only across the gap between callbacks, which is the only window the engine's own
        // zoom animation runs in and therefore the only window it is needed.
        //
        // Everything else consequently sees the player's zoom rather than our arm. That matters most
        // for the profile switch: leaving third person while pinned at a wall would otherwise have it
        // remember the arm as the player's third-person zoom.
        release_held_target(camera);
#endif
        run_profile_control(camera, before);
#if defined(GDTPC_CAMERA_COLLISION)
        // Mouse look first, so collision queries the camera with this frame's yaw and pitch; the pitch
        // overlay is re-applied afterwards in case collision's SetZoom refreshed derived pitch.
        // Virtual zoom (plan section 8) slots in as: zoom intent after mouse look, collision on the visual
        // distance, pitch from pitch_for_distance(arm), then the eye pull that renders the arm.
        run_mouse_look(camera, before);
        const auto shoulder = run_shoulder_control(camera, before);
        run_virtual_zoom_intent(camera, before);
        run_collision_control(camera, before, shoulder.translation, shoulder.force_query);
        reapply_mouse_look_pitch(camera);
        apply_virtual_zoom_view(camera);
        run_far_plane_cap(camera);
#endif
#endif
        capture_camera_state(camera, evidence);
#if defined(GDTPC_CAMERA_COLLISION)
        run_ui_probe();
#endif
    }
#if defined(GDTPC_GATE1_PROFILE_WRITES)
    entered = false;
#endif
}

bool write_ascii(const HANDLE file, const char* text, const std::size_t length) noexcept
{
    if (file == INVALID_HANDLE_VALUE || text == nullptr || length > MAXDWORD) return false;
    DWORD written = 0;
    return WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr) != FALSE && written == length;
}

constexpr char telemetry_header[] = "sequence,tick_ms,callbacks,generation,sample_valid,camera,player,world_yaw,world_pitch,world_fov,requested_yaw,zoom_blend,zoom_target_blend,zoom_a,zoom_b,facing_valid,facing_x,facing_y,facing_z,facing_heading,game_engine,ui,dialog_active,action_set,input_mode,foreground,dropped,callback_thread_id,hook_entry,original_return,owner_thread_id,owner_thread_mismatches,camera_mode,restore_state,writes_enabled,control_writes,restore_writes,abandoned_excursions,collision_arm,collision_desired,collision_state,collision_queries,collision_hits,collision_faults,collision_writes,toggle_down,toggle_edges,profile_transition,collision_write_result,zoom_step_clicks,main_thread_id,window_thread_id,cam28_x,cam28_y,cam28_z,cam94_x,cam94_y,cam94_z,camera_offset_x,camera_offset_y,camera_offset_z,target_offset_x,target_offset_y,target_offset_z,shoulder_side,shoulder_edges,shoulder_writes,menu_flag,menu_ui9918,menu_e1858,mouse_look_state,aim_y,yaw_delta,cursor_warps,cursor_escapes,mouse_yaw_writes,combat_state,combat_aux,panel_candidates,pitch_offset,pitch_writes,visual_distance,visual_arm,eye_pull,engine_distance,virtual_zoom_state,virtual_zoom_clicks,virtual_zoom_faults,camera_shake_suppressions,far_plane,render_far_plane,far_plane_percent,view_distance_locked,npc_talk,controller_state_rva,dot_cursor,panel_open_flags,controller_event_updates,controller_event_count,controller_analog_action,controller_analog_x,controller_analog_y,controller_event_faults,right_stick_y,stick_pitch_delta,cursor_visibility,cursor_owner,cursor_thread\r\n";

int format_telemetry_row(const TelemetrySnapshot& sample, char* line, const std::size_t capacity) noexcept
{
    return std::snprintf(line, capacity,
        "%llu,%llu,%llu,%llu,%u,0x%llx,0x%llx,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%.9g,%.9g,%.9g,0x%llx,0x%llx,%u,%u,%u,%u,%llu,%u,%llu,%llu,%u,%llu,%u,%u,%u,%llu,%llu,%llu,%.9g,%.9g,%u,%llu,%llu,%llu,%llu,%u,%llu,%u,%u,%llu,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%llu,%llu,%u,0x%llx,0x%llx,%u,%.9g,%.9g,%llu,%llu,%llu,%u,%u,0x%x,%.9g,%llu,%.9g,%.9g,%.9g,%.9g,%u,%llu,%llu,%llu,%.9g,%.9g,%u,%u,%u,0x%x,%u,0x%x,%llu,%u,%d,%.9g,%.9g,%llu,%d,%.9g,%u,%u,%u\r\n",
        static_cast<unsigned long long>(sample.sequence), static_cast<unsigned long long>(sample.tick_ms),
        static_cast<unsigned long long>(sample.callbacks), static_cast<unsigned long long>(sample.generation), sample.sample_valid,
        static_cast<unsigned long long>(sample.camera), static_cast<unsigned long long>(sample.player),
        sample.world_yaw, sample.world_pitch, sample.world_fov, sample.requested_yaw,
        sample.zoom_blend, sample.zoom_target_blend,
        sample.zoom_a, sample.zoom_b,
        sample.facing_valid, sample.facing_x, sample.facing_y, sample.facing_z, sample.facing_heading,
        static_cast<unsigned long long>(sample.game_engine), static_cast<unsigned long long>(sample.ui),
        sample.dialog_active, sample.action_set, sample.input_mode, sample.foreground,
        static_cast<unsigned long long>(telemetry_dropped.load(std::memory_order_relaxed)),
        sample.callback_evidence.callback_thread_id,
        static_cast<unsigned long long>(sample.callback_evidence.hook_entry),
        static_cast<unsigned long long>(sample.callback_evidence.original_return),
        sample.callback_evidence.owner_thread_id,
        static_cast<unsigned long long>(sample.callback_evidence.owner_thread_mismatches),
        sample.camera_mode, sample.restore_state, sample.writes_enabled,
        static_cast<unsigned long long>(sample.control_writes),
        static_cast<unsigned long long>(sample.restore_writes),
        static_cast<unsigned long long>(sample.abandoned_excursions),
        sample.collision_arm, sample.collision_desired, sample.collision_state,
        static_cast<unsigned long long>(sample.collision_queries),
        static_cast<unsigned long long>(sample.collision_hits),
        static_cast<unsigned long long>(sample.collision_faults),
        static_cast<unsigned long long>(sample.collision_writes),
        sample.toggle_down, static_cast<unsigned long long>(sample.toggle_edges), sample.profile_transition,
        sample.collision_write_result, static_cast<unsigned long long>(sample.zoom_step_clicks),
        sample.main_thread_id, sample.window_thread_id,
        sample.camera_local_x, sample.camera_local_y, sample.camera_local_z,
        sample.camera_world_x, sample.camera_world_y, sample.camera_world_z,
        sample.camera_offset_x, sample.camera_offset_y, sample.camera_offset_z,
        sample.target_offset_x, sample.target_offset_y, sample.target_offset_z,
        sample.shoulder_side, static_cast<unsigned long long>(sample.shoulder_edges),
        static_cast<unsigned long long>(sample.shoulder_writes),
        sample.menu_flag, static_cast<unsigned long long>(sample.menu_ui9918),
        static_cast<unsigned long long>(sample.menu_e1858),
        sample.mouse_look_state, sample.aim_y, sample.yaw_delta,
        static_cast<unsigned long long>(sample.cursor_warps), static_cast<unsigned long long>(sample.cursor_escapes),
        static_cast<unsigned long long>(sample.mouse_yaw_writes), sample.combat_state, sample.combat_aux, sample.panel_candidates,
        sample.pitch_offset, static_cast<unsigned long long>(sample.pitch_writes),
        sample.visual_distance, sample.visual_arm, sample.eye_pull, sample.engine_distance, sample.virtual_zoom_state,
        static_cast<unsigned long long>(sample.virtual_zoom_clicks), static_cast<unsigned long long>(sample.virtual_zoom_faults),
        static_cast<unsigned long long>(sample.camera_shake_suppressions),
        sample.far_plane, sample.render_far_plane, sample.far_plane_percent, sample.view_distance_locked,
        sample.npc_talk, sample.controller_state_rva, sample.dot_cursor, sample.panel_open_flags,
        static_cast<unsigned long long>(sample.controller_event_updates), sample.controller_event_count,
        sample.controller_analog_action, sample.controller_analog_x, sample.controller_analog_y,
        static_cast<unsigned long long>(sample.controller_event_faults),
        sample.right_stick_y, sample.stick_pitch_delta, sample.cursor_visibility, sample.cursor_owner, sample.cursor_thread);
}

// Byte sink owned exclusively by the worker thread for its whole lifetime.
class FileSink final
{
public:
    explicit FileSink(const HANDLE file) noexcept : file_{file} {}
    [[nodiscard]] bool write(const char* text, const std::size_t length) noexcept { return write_ascii(file_, text, length); }
    [[nodiscard]] bool flush() noexcept { return FlushFileBuffers(file_) != FALSE; }
    void close() noexcept { if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; } }
private:
    HANDLE file_;
};

DWORD WINAPI logging_writer(LPVOID parameter) noexcept
{
    FileSink sink{static_cast<HANDLE>(parameter)}; // worker exclusively owns this handle
    auto healthy = sink.write(telemetry_header, sizeof(telemetry_header) - 1);
#if defined(GDTPC_CAMERA_COLLISION)
    // The probe file is opened, written and closed only by this worker. An enabled probe that cannot
    // create its file fails activation closed, before any hook exists, rather than silently
    // producing a session with no research evidence.
    const auto probe_file = active_config.ui_probe_enabled && !ui_probe_path.empty()
        ? CreateFileW(ui_probe_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
              FILE_ATTRIBUTE_NORMAL, nullptr)
        : INVALID_HANDLE_VALUE;
    if (active_config.ui_probe_enabled && probe_file == INVALID_HANDLE_VALUE) healthy = false;
#endif
    writer_health.store(healthy ? 1U : 2U, std::memory_order_release);
    if (healthy)
        healthy = gdtpc::run_drain_loop<decltype(telemetry_queue), TelemetrySnapshot>(
            telemetry_queue, sink,
#if defined(GDTPC_CAMERA_COLLISION)
            [probe_file] {
                flush_ui_probe(probe_file);
                return WaitForSingleObject(logging_stop_event, 50) == WAIT_OBJECT_0;
            },
#else
            [] { return WaitForSingleObject(logging_stop_event, 50) == WAIT_OBJECT_0; },
#endif
            format_telemetry_row);
#if defined(GDTPC_CAMERA_COLLISION)
    flush_ui_probe(probe_file);
    if (probe_file != INVALID_HANDLE_VALUE)
    {
        static_cast<void>(FlushFileBuffers(probe_file));
        CloseHandle(probe_file);
    }
#endif
    if (!healthy) writer_health.store(2, std::memory_order_release);
    const auto flushed = sink.flush();
    sink.close();
    writer_health.store(healthy && flushed ? 4U : 2U, std::memory_order_release);
    return healthy && flushed ? 0U : 1U;
}

bool stop_writer(const DWORD timeout_ms) noexcept
{
    if (logging_stop_event != nullptr) SetEvent(logging_stop_event);
    if (logging_thread != nullptr)
    {
        writer_health.store(3, std::memory_order_release);
        const auto wait = WaitForSingleObject(logging_thread, timeout_ms);
        if (wait != WAIT_OBJECT_0) return false;
        DWORD exit_code = 1;
        if (GetExitCodeThread(logging_thread, &exit_code) == FALSE) exit_code = 1;
        CloseHandle(logging_thread);
        logging_thread = nullptr;
        writer_health.store(exit_code == 0 ? 4U : 2U, std::memory_order_release);
    }
    if (logging_stop_event != nullptr)
    {
        CloseHandle(logging_stop_event);
        logging_stop_event = nullptr;
    }
    return true;
}

bool load_and_validate_config(const wchar_t* path) noexcept
{
    if (path == nullptr || *path == L'\0') return false;
    const auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 && size.QuadPart <= 65536;
    std::string contents(ok ? static_cast<std::size_t>(size.QuadPart) : 0, '\0');
    DWORD read = 0;
    ok = ok && ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) != FALSE && read == contents.size();
    CloseHandle(file);
    if (!ok) return false;
    gdtpc::RuntimeConfig config;
    std::string error;
    if (!gdtpc::parse_runtime_config(contents, config, error)) return false;
    // Retained under the lifecycle lock and never reparsed; settings are immutable for the run.
    active_config = config;
    config_loaded.store(1, std::memory_order_release);
    return true;
}

}

GdTpcPhase initialize_validation_locked() noexcept;

GDTPC_API GdTpcRuntimeStatus __cdecl GdTpcGetRuntimeStatus() noexcept
{
    return {1, phase.load(std::memory_order_acquire), hooks_installed.load(std::memory_order_acquire),
        game_state_writes_enabled.load(std::memory_order_acquire)};
}

GDTPC_API GdTpcRuntimeStatusV2 __cdecl GdTpcGetRuntimeStatusV2() noexcept
{
    return {abi_version, sizeof(GdTpcRuntimeStatusV2), phase.load(std::memory_order_acquire),
        hooks_installed.load(std::memory_order_acquire), game_state_writes_enabled.load(std::memory_order_acquire),
        writer_health.load(std::memory_order_acquire), restore_state.load(std::memory_order_acquire),
        config_loaded.load(std::memory_order_acquire),
        callback_count.load(std::memory_order_relaxed), telemetry_published.load(std::memory_order_relaxed),
        telemetry_dropped.load(std::memory_order_relaxed), session_generation.load(std::memory_order_relaxed),
        control_write_count.load(std::memory_order_acquire), restore_write_count.load(std::memory_order_acquire),
        collision_enabled_status.load(std::memory_order_acquire), collision_state.load(std::memory_order_acquire),
        collision_arm.load(std::memory_order_relaxed), 0,
        collision_query_count.load(std::memory_order_relaxed), collision_hit_count.load(std::memory_order_relaxed),
        collision_fault_count.load(std::memory_order_relaxed), collision_write_count.load(std::memory_order_relaxed)};
}

GDTPC_API GdTpcPhase __cdecl GdTpcInitializeLogging(const wchar_t* log_path) noexcept
{
    return GdTpcInitializeLoggingV2(log_path, nullptr);
}

GDTPC_API GdTpcPhase __cdecl GdTpcInitializeLoggingV2(const wchar_t* log_path, const wchar_t* config_path) noexcept
{
    try
    {
    std::scoped_lock lock(validation_mutex);
        const auto current = phase.load(std::memory_order_acquire);
        if (current == GdTpcPhase::logging_active) return current; // idempotent, never creates a second worker
        if (current != GdTpcPhase::inert) return current;
        const auto validated = initialize_validation_locked();
        if (validated != GdTpcPhase::ready_validation_only) return validated;
        if (log_path == nullptr || *log_path == L'\0')
        {
            reject(GdTpcPhase::internal_error, L"Logging path was not supplied.");
            return phase.load(std::memory_order_acquire);
        }
        if (!load_and_validate_config(config_path))
        {
            reject(GdTpcPhase::rejected_config, L"Runtime configuration is missing, empty, oversized, or invalid for Gate 0.");
            return phase.load(std::memory_order_acquire);
        }
#if defined(GDTPC_GATE1_PROFILE_WRITES)
        constexpr float radians_per_degree = 0.01745329251994329577F;
        profile_switch.emplace(
            active_config.third_person_camera, active_config.third_person_fov_degrees * radians_per_degree);
#if defined(GDTPC_CAMERA_COLLISION)
        collision_model.emplace(active_config.collision);
        zoom_step_model.emplace(gdtpc::zoom_step_settings(active_config));
        shoulder_offset_model.emplace();
        collision_enabled_status.store(active_config.collision_enabled ? 1U : 0U, std::memory_order_release);
        collision_state.store(static_cast<std::uint32_t>(gdtpc::CollisionState::ineligible), std::memory_order_release);
        collision_query_count.store(0, std::memory_order_relaxed);
        collision_hit_count.store(0, std::memory_order_relaxed);
        collision_fault_count.store(0, std::memory_order_relaxed);
        collision_write_count.store(0, std::memory_order_relaxed);
        collision_arm.store(0.0F, std::memory_order_relaxed);
        collision_desired.store(0.0F, std::memory_order_relaxed);
        shoulder_generation = 0;
        shoulder_camera = 0;
        shoulder_side_status.store(0, std::memory_order_relaxed);
        shoulder_edge_count.store(0, std::memory_order_relaxed);
        shoulder_write_count.store(0, std::memory_order_relaxed);
        // Beside the CSV, so the run sidecar's log path also locates the research evidence.
        ui_probe_path = std::wstring(log_path) + L".uiprobe.bin";
        ui_probe_state.store(0, std::memory_order_release);
        ui_probe_write_failures.store(0, std::memory_order_relaxed);
        mouse_look_model.emplace(active_config.mouse_look);
        mouse_look_generation = 0;
        mouse_look_warp_failed = false;
        mouse_look_state_status.store(0, std::memory_order_relaxed);
        mouse_look_warps.store(0, std::memory_order_relaxed);
        mouse_look_escapes.store(0, std::memory_order_relaxed);
        mouse_look_yaw_writes.store(0, std::memory_order_relaxed);
        mouse_look_pitch_apply = false;
        mouse_look_pitch_overlay.release();
        mouse_look_last_tick = 0;
        controller_event_consumed_updates = 0;
        controller_event_updates.store(0, std::memory_order_relaxed);
        controller_event_epoch.store(0, std::memory_order_relaxed);
        controller_event_count_status.store(0, std::memory_order_relaxed);
        controller_analog_action_status.store(-1, std::memory_order_relaxed);
        controller_analog_x_status.store(0.0F, std::memory_order_relaxed);
        controller_analog_y_status.store(0.0F, std::memory_order_relaxed);
        controller_event_faults.store(0, std::memory_order_relaxed);
        right_stick_y_status.store(0, std::memory_order_relaxed);
        stick_pitch_status.store(0.0F, std::memory_order_relaxed);
        mouse_look_pitch_status.store(0.0F, std::memory_order_relaxed);
        mouse_look_pitch_writes.store(0, std::memory_order_relaxed);
        virtual_zoom_model.emplace(gdtpc::virtual_zoom_settings(active_config));
        virtual_zoom_eye = {};
        virtual_zoom_generation = 0;
        virtual_zoom_camera = 0;
        virtual_zoom_last_tick = 0;
        virtual_zoom_frame_holding = false;
        virtual_zoom_fell_back = false;
        virtual_zoom_arm = 0.0F;
        virtual_zoom_arm_valid = false;
        virtual_zoom_previous_eye = {};
        virtual_zoom_previous_eye_valid = false;
        virtual_zoom_write_count.store(0, std::memory_order_relaxed);
        virtual_zoom_state_status.store(0, std::memory_order_relaxed);
        virtual_zoom_visual_status.store(0.0F, std::memory_order_relaxed);
        virtual_zoom_arm_status.store(0.0F, std::memory_order_relaxed);
        virtual_zoom_pull_status.store(0.0F, std::memory_order_relaxed);
        virtual_zoom_engine_status.store(0.0F, std::memory_order_relaxed);
        virtual_zoom_clicks.store(0, std::memory_order_relaxed);
        virtual_zoom_faults.store(0, std::memory_order_relaxed);
        camera_shake_suppressions.store(0, std::memory_order_relaxed);
        far_plane_overlay = {};
        far_plane_percent_status.store(0, std::memory_order_relaxed);
#endif
        control_stop_ack_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        recoverable_fault_ack_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (control_stop_ack_event == nullptr || recoverable_fault_ack_event == nullptr)
        {
            reset_profile_control_before_activation();
            reject(GdTpcPhase::internal_error, L"Gate 1 restoration acknowledgment event could not be created.");
            return phase.load(std::memory_order_acquire);
        }
#endif

        const auto file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
#if defined(GDTPC_GATE1_PROFILE_WRITES)
            reset_profile_control_before_activation();
#endif
            reject(GdTpcPhase::internal_error, L"Logging file could not be created.");
            return phase.load(std::memory_order_acquire);
        }

        logging_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (logging_stop_event == nullptr)
        {
            CloseHandle(file);
#if defined(GDTPC_GATE1_PROFILE_WRITES)
            reset_profile_control_before_activation();
#endif
            reject(GdTpcPhase::internal_error, L"Logging stop event could not be created.");
            return phase.load(std::memory_order_acquire);
        }
        writer_health.store(0, std::memory_order_release);
        logging_thread = CreateThread(nullptr, 0, logging_writer, file, 0, nullptr);
        if (logging_thread == nullptr)
        {
            CloseHandle(file);
            CloseHandle(logging_stop_event);
            logging_stop_event = nullptr;
#if defined(GDTPC_GATE1_PROFILE_WRITES)
            reset_profile_control_before_activation();
#endif
            reject(GdTpcPhase::internal_error, L"Logging writer thread could not be created.");
            return phase.load(std::memory_order_acquire);
        }

        capture_host_thread_identity();

        const auto game = GetModuleHandleW(L"Game.dll");
        game_engine_slot = reinterpret_cast<void* const*>(
            GetProcAddress(game, "?gGameEngine@GAME@@3PEAVGameEngine@1@EA"));
        update_from_input_original = reinterpret_cast<UpdateFromInput>(
            GetProcAddress(game, "?UpdateFromInputImpl@GameCamera@GAME@@MEAAXXZ"));
#if defined(GDTPC_GATE1_PROFILE_WRITES)
        get_engine_camera = reinterpret_cast<GetEngineObject>(
            GetProcAddress(game, "?GetCamera@GameEngine@GAME@@QEAAPEAVGameCamera@2@XZ"));
        get_main_player = reinterpret_cast<GetEngineObject>(
            GetProcAddress(game, "?GetMainPlayer@GameEngine@GAME@@QEBAPEAVPlayer@2@XZ"));
        set_camera_zoom = reinterpret_cast<SetCameraZoom>(
            GetProcAddress(game, "?SetZoom@GameCamera@GAME@@QEAAXM@Z"));
#if defined(GDTPC_CAMERA_COLLISION)
        const auto engine = GetModuleHandleW(L"Engine.dll");
        core_engine_slot = reinterpret_cast<void* const*>(
            GetProcAddress(engine, engine_data_exports[1].name));
        get_input_device = reinterpret_cast<GetEngineObject>(
            GetProcAddress(engine, "?GetInputDevice@Engine@GAME@@QEAAPEAVInputDevice@2@XZ"));
        steam_controller_update_original = reinterpret_cast<SteamControllerUpdate>(
            GetProcAddress(engine, "?Update@SteamControllerDevice@GAME@@QEAAXH@Z"));
        collision_entries.get_region = reinterpret_cast<decltype(collision_entries.get_region)>(
            GetProcAddress(engine, "?GetRegion@WorldCamera@GAME@@QEBAPEAVRegion@2@XZ"));
        collision_entries.calculate_view_position = reinterpret_cast<decltype(collision_entries.calculate_view_position)>(
            GetProcAddress(engine, "?CalculateViewPosition@WorldCamera@GAME@@MEBA?AVWorldVec3@2@AEBV32@@Z"));
        collision_entries.get_level_ptr = reinterpret_cast<decltype(collision_entries.get_level_ptr)>(
            GetProcAddress(engine, "?GetLevelPtr@Region@GAME@@QEBAPEAVLevel@2@XZ"));
        object_manager_get = reinterpret_cast<ObjectManagerGet>(
            GetProcAddress(engine, "?Get@?$Singleton@VObjectManager@GAME@@@GAME@@SAPEAVObjectManager@2@XZ"));
        {
            const auto game_base = reinterpret_cast<const std::uint8_t*>(game);
            const auto game_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                game_base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game_base)->e_lfanew);
            game_module_base = reinterpret_cast<std::uintptr_t>(game);
            game_module_size = game_nt->OptionalHeader.SizeOfImage;
            // Validated before activation (RVA + prefix on the hash-verified module).
            object_manager_find = reinterpret_cast<ObjectManagerFind>(game_module_base + object_manager_find_rva);
        }
        win_window_vtable = reinterpret_cast<std::uintptr_t>(GetProcAddress(engine, engine_data_exports[0].name));
        dot_cursor_window = 0;
        dot_cursor_overlay = {};
        dot_cursor_status.store(0, std::memory_order_relaxed);
        controller_player_vtable = reinterpret_cast<std::uintptr_t>(GetProcAddress(game, game_data_exports[0].name));
        talk_to_npc_vtable = reinterpret_cast<std::uintptr_t>(GetProcAddress(game, game_data_exports[1].name));
        npc_talk_fault_count = 0;
        npc_talk_status.store(0, std::memory_order_relaxed);
        controller_state_rva_status.store(0xFFFFFFFFU, std::memory_order_relaxed);
        collision_entries.get_intersection = reinterpret_cast<decltype(collision_entries.get_intersection)>(
            GetProcAddress(engine, "?GetIntersection@Level@GAME@@QEBAXAEBVRay@2@AEAVIntersection@2@W4PhysicsSurface@2@PEAPEAVEntity@2@MPEBV62@_N@Z"));
#endif
#endif
        HMODULE pinned_module = nullptr;
        const auto pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&GdTpcInitializeLoggingV2), &pinned_module) != FALSE;
        if (!pinned || game_engine_slot == nullptr || update_from_input_original == nullptr ||
#if defined(GDTPC_GATE1_PROFILE_WRITES)
            get_engine_camera == nullptr || get_main_player == nullptr || set_camera_zoom == nullptr ||
#if defined(GDTPC_CAMERA_COLLISION)
            !collision_entries.complete() || core_engine_slot == nullptr || get_input_device == nullptr ||
            steam_controller_update_original == nullptr ||
            object_manager_get == nullptr || object_manager_find == nullptr ||
            controller_player_vtable == 0 || talk_to_npc_vtable == 0 || win_window_vtable == 0 ||
#endif
#endif
#if defined(GDTPC_CAMERA_COLLISION)
            !prepare_cursor_visibility() || !attach_cursor_visibility() ||
            steam_controller_update_hook.attach(reinterpret_cast<void**>(&steam_controller_update_original),
                reinterpret_cast<void*>(&steam_controller_update_logging_hook)) != NO_ERROR ||
#endif
            update_from_input_hook.attach(reinterpret_cast<void**>(&update_from_input_original),
                reinterpret_cast<void*>(&update_from_input_logging_hook)) != NO_ERROR)
        {
            // Initialization never intentionally leaves a partial hook set. A detach failure is still safe because both
            // replacements call only their originals while the phase is inert, but the module must remain resident and
            // the game must be exited before any retry.
            if (update_from_input_hook.installed()) static_cast<void>(update_from_input_hook.detach());
#if defined(GDTPC_CAMERA_COLLISION)
            rollback_cursor_visibility();
            if (steam_controller_update_hook.installed()) static_cast<void>(steam_controller_update_hook.detach());
#endif
            // A bounded join keeps the remote initialization thread from blocking forever. On
            // timeout the worker keeps its own file/thread/event ownership and writer_health
            // stays at 3 (stop pending); nothing is reclaimed underneath it.
            const auto joined = stop_writer(initialization_stop_timeout_ms);
#if defined(GDTPC_GATE1_PROFILE_WRITES)
            reset_profile_control_before_activation();
#endif
            reject(GdTpcPhase::hook_failed, joined
                ? L"Camera logging hook could not be installed; the writer completed and no hook is present."
                : L"Camera logging hook could not be installed; the writer did not confirm completion and its resources are retained.");
            return phase.load(std::memory_order_acquire);
        }

#if defined(GDTPC_CAMERA_COLLISION)
        hooks_installed.store(2, std::memory_order_release);
#else
        hooks_installed.store(1, std::memory_order_release);
#endif
#if defined(GDTPC_GATE1_PROFILE_WRITES)
        game_state_writes_enabled.store(1, std::memory_order_release);
#endif
        clear_last_error();
        phase.store(GdTpcPhase::logging_active, std::memory_order_release);
        return GdTpcPhase::logging_active;
    }
    catch (...)
    {
        const auto joined = stop_writer(initialization_stop_timeout_ms);
#if defined(GDTPC_GATE1_PROFILE_WRITES)
        if (hooks_installed.load(std::memory_order_acquire) == 0) reset_profile_control_before_activation();
#endif
        reject(GdTpcPhase::internal_error, joined
            ? L"Unexpected logging initialization failure; the writer completed."
            : L"Unexpected logging initialization failure; the writer did not confirm completion and its resources are retained.");
        return GdTpcPhase::internal_error;
    }
}

GDTPC_API DWORD WINAPI GdTpcInitializeRemote(void* raw_request) noexcept
{
    if (raw_request == nullptr) return static_cast<DWORD>(GdTpcPhase::internal_error);
    auto* request = static_cast<GdTpcRemoteInitializeRequest*>(raw_request);
    __try
    {
        if (request->abi_version != GDTPC_REMOTE_REQUEST_ABI || request->structure_size != sizeof(*request) ||
            request->log_path[GDTPC_REMOTE_PATH_CAPACITY - 1] != L'\0' ||
            request->config_path[GDTPC_REMOTE_PATH_CAPACITY - 1] != L'\0')
            return static_cast<DWORD>(GdTpcPhase::internal_error);
        const auto result = GdTpcInitializeLoggingV2(request->log_path, request->config_path);
        request->result = GdTpcGetRuntimeStatusV2();
        MemoryBarrier();
        request->completed = 1;
        return static_cast<DWORD>(result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return static_cast<DWORD>(GdTpcPhase::internal_error);
    }
}

GDTPC_API GdTpcPhase __cdecl GdTpcRequestLogicalStop(const std::uint32_t timeout_ms) noexcept
{
    try
    {
        std::scoped_lock lock(validation_mutex);
        const auto current = phase.load(std::memory_order_acquire);
        if (current == GdTpcPhase::stopped_resident) return current;
        if (current != GdTpcPhase::logging_active && current != GdTpcPhase::shutdown_pending) return current;
#if defined(GDTPC_GATE1_PROFILE_WRITES)
        control_stop_requested.store(1, std::memory_order_release);
        if (control_stop_ack_event == nullptr || WaitForSingleObject(control_stop_ack_event, timeout_ms) != WAIT_OBJECT_0)
        {
            restore_state.store(static_cast<std::uint32_t>(gdtpc::ProfileRestoreState::pending), std::memory_order_release);
            phase.store(GdTpcPhase::shutdown_pending, std::memory_order_release);
            return GdTpcPhase::shutdown_pending;
        }
        game_state_writes_enabled.store(0, std::memory_order_release);
        if (restore_state.load(std::memory_order_acquire) == static_cast<std::uint32_t>(gdtpc::ProfileRestoreState::impossible))
        {
            static_cast<void>(stop_writer(timeout_ms));
            reject(GdTpcPhase::internal_error, L"Gate 1 stop could not verify restoration; normal process exit is required.");
            return GdTpcPhase::internal_error;
        }
#else
        phase.store(GdTpcPhase::stop_requested, std::memory_order_release);
#endif
        if (!stop_writer(timeout_ms))
        {
            phase.store(GdTpcPhase::shutdown_pending, std::memory_order_release);
            return GdTpcPhase::shutdown_pending;
        }
        // Production logical stop deliberately retains the process-lifetime hook; it is original-only.
        phase.store(GdTpcPhase::stopped_resident, std::memory_order_release);
        return GdTpcPhase::stopped_resident;
    }
    catch (...) { reject(GdTpcPhase::internal_error, L"Unexpected logical-stop failure."); return GdTpcPhase::internal_error; }
}

GDTPC_API DWORD WINAPI GdTpcRequestLogicalStopRemote(void* raw_request) noexcept
{
    if (raw_request == nullptr) return static_cast<DWORD>(GdTpcPhase::internal_error);
    auto* request = static_cast<GdTpcRemoteStopRequest*>(raw_request);
    __try
    {
        if (request->abi_version != GDTPC_REMOTE_REQUEST_ABI || request->structure_size != sizeof(*request))
            return static_cast<DWORD>(GdTpcPhase::internal_error);
        const auto result = GdTpcRequestLogicalStop(request->timeout_ms);
        request->result = GdTpcGetRuntimeStatusV2();
        MemoryBarrier();
        request->completed = 1;
        return static_cast<DWORD>(result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return static_cast<DWORD>(GdTpcPhase::internal_error);
    }
}

#if defined(GDTPC_GATE1_PROFILE_WRITES)
bool read_gate1_fault_request(const GdTpcRemoteGate1FaultRequest* request, std::uint32_t& request_abi,
    std::uint32_t& request_size, std::uint32_t& timeout_ms) noexcept
{
    __try
    {
        request_abi = request->abi_version;
        request_size = request->structure_size;
        timeout_ms = request->timeout_ms;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool complete_gate1_fault_request(GdTpcRemoteGate1FaultRequest* request,
    const GdTpcRuntimeStatusV2& status, const std::uint64_t control_before,
    const std::uint64_t restore_before) noexcept
{
    __try
    {
        request->control_writes_before = control_before;
        request->restore_writes_before = restore_before;
        request->result = status;
        MemoryBarrier();
        request->completed = 1;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#endif

GDTPC_API DWORD WINAPI GdTpcRequestGate1RecoverableFaultRemote(void* raw_request) noexcept
{
#if !defined(GDTPC_GATE1_PROFILE_WRITES)
    static_cast<void>(raw_request);
    return static_cast<DWORD>(GdTpcPhase::internal_error);
#else
    if (raw_request == nullptr) return static_cast<DWORD>(GdTpcPhase::internal_error);
    auto* request = static_cast<GdTpcRemoteGate1FaultRequest*>(raw_request);
    std::uint32_t request_abi = 0, request_size = 0, timeout_ms = 0;
    if (!read_gate1_fault_request(request, request_abi, request_size, timeout_ms))
        return static_cast<DWORD>(GdTpcPhase::internal_error);
    if (request_abi != GDTPC_REMOTE_REQUEST_ABI || request_size != sizeof(*request) || timeout_ms == 0)
        return static_cast<DWORD>(GdTpcPhase::internal_error);
    try
    {
        std::scoped_lock lock(validation_mutex);
        if (phase.load(std::memory_order_acquire) != GdTpcPhase::logging_active ||
            game_state_writes_enabled.load(std::memory_order_acquire) == 0 ||
            camera_mode.load(std::memory_order_acquire) != static_cast<std::uint32_t>(gdtpc::ProfileMode::native) ||
            recoverable_fault_ack_event == nullptr || recoverable_fault_request.load(std::memory_order_acquire) != 0)
            return static_cast<DWORD>(GdTpcPhase::internal_error);
        ResetEvent(recoverable_fault_ack_event);
        const auto control_before = control_write_count.load(std::memory_order_acquire);
        const auto restore_before = restore_write_count.load(std::memory_order_acquire);
        recoverable_fault_request.store(1, std::memory_order_release);
        const auto wait = WaitForSingleObject(recoverable_fault_ack_event, timeout_ms);
        if (wait != WAIT_OBJECT_0)
        {
            recoverable_fault_request.store(0, std::memory_order_release);
            return static_cast<DWORD>(GdTpcPhase::internal_error);
        }
        const auto status = GdTpcGetRuntimeStatusV2();
        if (!complete_gate1_fault_request(request, status, control_before, restore_before))
            return static_cast<DWORD>(GdTpcPhase::internal_error);
        return static_cast<DWORD>(status.phase);
    }
    catch (...) { return static_cast<DWORD>(GdTpcPhase::internal_error); }
#endif
}

GDTPC_API std::uint32_t __cdecl GdTpcValidateKnownFiles(const wchar_t* game_root) noexcept
{
    if (game_root == nullptr || *game_root == L'\0')
        return 0;
    try
    {
        const std::wstring root(game_root);
        return validate_hash(join_path(root, L"x64\\Grim Dawn.exe"), executable_hash) &&
            validate_hash(join_path(root, L"x64\\Game.dll"), game_hash) &&
            validate_hash(join_path(root, L"x64\\Engine.dll"), engine_hash) ? 1U : 0U;
    }
    catch (...)
    {
        return 0;
    }
}

GdTpcPhase initialize_validation_locked() noexcept
{
    try
    {
    if (phase.load(std::memory_order_acquire) != GdTpcPhase::inert)
        return phase.load(std::memory_order_acquire);

    phase.store(GdTpcPhase::validating, std::memory_order_release);
    if (!is_grim_dawn_host())
    {
        reject(GdTpcPhase::rejected_host, L"Host executable is not Grim Dawn.exe.");
        return phase.load(std::memory_order_acquire);
    }

    std::array<wchar_t, 32768> executable_path{};
    const auto executable_length = GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (executable_length == 0 || executable_length >= executable_path.size() || !validate_hash(executable_path.data(), executable_hash))
    {
        reject(GdTpcPhase::rejected_host, L"Host executable hash is unsupported.");
        return phase.load(std::memory_order_acquire);
    }

    const auto game = GetModuleHandleW(L"Game.dll");
    const auto engine = GetModuleHandleW(L"Engine.dll");
    if (game == nullptr || engine == nullptr)
    {
        reject(GdTpcPhase::rejected_module, L"Required Grim Dawn modules are not loaded.");
        return phase.load(std::memory_order_acquire);
    }

    std::array<wchar_t, 32768> module_path{};
    const auto game_length = GetModuleFileNameW(game, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (game_length == 0 || game_length >= module_path.size() || !validate_hash(module_path.data(), game_hash))
    {
        reject(GdTpcPhase::rejected_module, L"Game.dll hash is unsupported.");
        return phase.load(std::memory_order_acquire);
    }
    module_path.fill(L'\0');
    const auto engine_length = GetModuleFileNameW(engine, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (engine_length == 0 || engine_length >= module_path.size() || !validate_hash(module_path.data(), engine_hash))
    {
        reject(GdTpcPhase::rejected_module, L"Engine.dll hash is unsupported.");
        return phase.load(std::memory_order_acquire);
    }

    if (!validate_access_points(game, game_access_points.data(), game_access_points.size()) ||
        !validate_access_points(engine, engine_access_points.data(), engine_access_points.size()) || !validate_engine_slot(game) ||
        !validate_internal_code(game, object_manager_find_rva, object_manager_find_prefix, std::size(object_manager_find_prefix)) ||
        !validate_data_exports(game, game_data_exports.data(), game_data_exports.size()) ||
        !validate_data_exports(engine, engine_data_exports.data(), engine_data_exports.size()))
    {
        reject(GdTpcPhase::rejected_access_point, L"A required exported access point failed validation.");
        return phase.load(std::memory_order_acquire);
    }

    clear_last_error();
    phase.store(GdTpcPhase::ready_validation_only, std::memory_order_release);
    return phase.load(std::memory_order_acquire);
    }
    catch (...)
    {
        reject(GdTpcPhase::internal_error, L"Unexpected validation failure.");
        return GdTpcPhase::internal_error;
    }
}

GDTPC_API GdTpcPhase __cdecl GdTpcInitializeValidation() noexcept
{
    try { std::scoped_lock lock(validation_mutex); return initialize_validation_locked(); }
    catch (...) { reject(GdTpcPhase::internal_error, L"Validation lock failure."); return GdTpcPhase::internal_error; }
}

namespace
{
void write_check_error(wchar_t* const destination, const std::uint32_t capacity, const char* const message) noexcept
{
    if (destination == nullptr || capacity == 0) return;
    std::uint32_t index = 0;
    for (; message != nullptr && message[index] != '\0' && index + 1 < capacity; ++index)
        destination[index] = static_cast<wchar_t>(static_cast<unsigned char>(message[index]));
    destination[index] = L'\0';
}
}

GDTPC_API std::uint32_t __cdecl GdTpcCheckConfigFile(const wchar_t* const config_path, wchar_t* const error_text,
    const std::uint32_t capacity) noexcept
{
    try
    {
        if (config_path == nullptr || *config_path == L'\0')
        {
            write_check_error(error_text, capacity, "No settings file path was supplied.");
            return 0;
        }
        const auto file = CreateFileW(config_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            write_check_error(error_text, capacity, "The settings file could not be opened.");
            return 0;
        }
        LARGE_INTEGER size{};
        auto ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 && size.QuadPart <= 65536;
        std::string contents(ok ? static_cast<std::size_t>(size.QuadPart) : 0, '\0');
        DWORD read = 0;
        ok = ok && ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) != FALSE && read == contents.size();
        CloseHandle(file);
        if (!ok)
        {
            write_check_error(error_text, capacity, "The settings file is empty, larger than 64 KB, or unreadable.");
            return 0;
        }
        gdtpc::RuntimeConfig config;
        std::string error;
        if (!gdtpc::parse_runtime_config(contents, config, error))
        {
            write_check_error(error_text, capacity, error.c_str());
            return 0;
        }
        write_check_error(error_text, capacity, "");
        return 1;
    }
    catch (...)
    {
        write_check_error(error_text, capacity, "Unexpected failure while checking the settings file.");
        return 0;
    }
}

GDTPC_API std::uint32_t __cdecl GdTpcGetLastErrorText(wchar_t* destination, const std::uint32_t capacity) noexcept
{
    try
    {
        std::scoped_lock lock(error_mutex);
        const auto length = wcsnlen_s(last_error.data(), last_error.size());
        const auto required = static_cast<std::uint32_t>(length + 1);
        if (destination != nullptr && capacity > 0)
            wcsncpy_s(destination, capacity, last_error.data(), _TRUNCATE);
        return required;
    }
    catch (...)
    {
        if (destination != nullptr && capacity > 0) *destination = L'\0';
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
