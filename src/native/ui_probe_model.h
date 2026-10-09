#pragma once

// Read-only menu-detection probe. The player marks moments with a panel open or everything closed,
// and each mark captures a snapshot of the GameEngine object, the UI object it points to, the engine's
// InputDevice, and bounded private-heap children of the UI/InputDevice roots. Offline analysis then
// looks for bytes that consistently separate marked states.
//
// Deliberately free of Win32, like camera_collision_model: the live adapter supplies UiProbeMemory,
// so which pointers are followed and how every record is laid out is provable against a fake.
//
// Record layout, little-endian, no padding:
//   header (64 bytes): "GDUP", u32 version, u64 record_bytes, u64 sequence, u64 tick,
//                      u64 engine, u64 ui, u32 label, u32 camera_mode, u32 foreground, u32 block_count
//   block header (24): u64 address, u64 parent_offset, u32 size, u32 kind; then size bytes

#include <cstddef>
#include <cstdint>

namespace gdtpc
{
enum class UiProbeLabel : std::uint32_t { open = 1, closed = 2 };
enum class UiProbeBlockKind : std::uint32_t
{
    engine = 0,
    ui = 1,
    ui_child = 2,
    input_device = 3,
    input_child = 4
};

inline constexpr std::size_t ui_probe_header_bytes = 64;
inline constexpr std::size_t ui_probe_block_header_bytes = 24;
inline constexpr std::uint32_t ui_probe_version = 1;

class UiProbeMemory
{
public:
    virtual ~UiProbeMemory() = default;
    // How many of `wanted` bytes starting at `address` are committed and readable without crossing
    // the region, and whether the region is private (heap) memory rather than a mapped image.
    [[nodiscard]] virtual bool query(std::uintptr_t address, std::size_t wanted, std::size_t& readable,
        bool& private_memory) noexcept = 0;
    [[nodiscard]] virtual bool copy(std::uintptr_t address, std::uint8_t* destination, std::size_t size) noexcept = 0;
};

struct UiProbeLimits
{
    std::size_t engine_bytes{0x38000};      // covers every GameEngine field read so far (+0x37748)
    std::size_t ui_bytes{0x20000};
    std::size_t pointer_scan_bytes{0x4000}; // only the start of the UI object is scanned for pointers
    std::size_t child_bytes{0x300};
    std::size_t max_children{2048};
    std::size_t input_device_bytes{0x20000};
    std::size_t input_pointer_scan_bytes{0x10000};
    std::size_t input_child_bytes{0x800};
    std::size_t max_input_children{512};
};

struct UiProbeMark
{
    UiProbeLabel label{UiProbeLabel::closed};
    std::uint64_t sequence{};
    std::uint64_t tick{};
    std::uint32_t camera_mode{};
    std::uint32_t foreground{};
};

// Upper bound on one record for the given limits, used to size the preallocated buffer.
[[nodiscard]] std::size_t ui_probe_record_capacity(const UiProbeLimits& limits) noexcept;

// Writes exactly one record into `buffer` and returns its size, or 0 when nothing usable could be
// captured (unreadable engine or UI root, or a buffer smaller than the bound). Never allocates.
[[nodiscard]] std::size_t capture_ui_probe(UiProbeMemory& memory, std::uintptr_t engine, std::uintptr_t ui,
    std::uintptr_t input_device, const UiProbeMark& mark, const UiProbeLimits& limits, std::uint8_t* buffer,
    std::size_t capacity) noexcept;
}
