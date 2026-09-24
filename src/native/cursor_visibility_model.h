#pragma once
#include <cstdint>
#include <optional>
namespace gdtpc {
inline bool cursor_capture_fresh(bool captured, bool active, bool foreground, std::uint64_t now, std::uint64_t tick) noexcept {
    return captured && active && foreground && now >= tick && now - tick <= 250;
}
// Owner-thread only. Preserve the latest native request while our overlay owns visibility.
class CursorVisibilityOverlay final {
    bool owned_{};
    bool native_visible_{true};
public:
    bool owned() const noexcept { return owned_; }
    bool native_request(bool visible, bool hide) noexcept {
        native_visible_ = visible;
        owned_ = hide;
        return visible && !hide;
    }
    std::optional<bool> step(bool hide, bool currently_visible) noexcept {
        if (hide) {
            if (!owned_) { native_visible_ = currently_visible; owned_ = true; }
            // Do not normalize the OS counter again when the cursor is already hidden.
            if (currently_visible) return false;
            return {};
        }
        if (!owned_) return {};
        owned_ = false;
        return native_visible_;
    }
};
}
