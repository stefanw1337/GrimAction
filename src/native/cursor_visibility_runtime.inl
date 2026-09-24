// Included inside runtime.cpp's private namespace in collision builds only.
using CursorShow = void(__fastcall*)(void*, bool);
using CursorPresent = void(__fastcall*)(void*, void*);
CursorShow cursor_show_original{};
CursorPresent cursor_present_original{};
gdtpc::DetourHook cursor_show_hook, cursor_present_hook;
std::atomic<bool> cursor_capture_requested{false};
std::atomic<std::uint64_t> cursor_capture_tick{0};
std::atomic<HWND> cursor_game_window{nullptr};
std::atomic<bool> cursor_visibility_ready{false};
thread_local gdtpc::CursorVisibilityOverlay cursor_visibility_overlay;

bool cursor_hide_requested() noexcept {
    const auto window = cursor_game_window.load(std::memory_order_acquire);
    return gdtpc::cursor_capture_fresh(cursor_capture_requested.load(std::memory_order_acquire),
        phase.load(std::memory_order_acquire) == GdTpcPhase::logging_active &&
        control_stop_requested.load(std::memory_order_acquire) == 0 &&
        recoverable_fault_request.load(std::memory_order_acquire) == 0,
        window != nullptr && GetForegroundWindow() == window,
        GetTickCount64(), cursor_capture_tick.load(std::memory_order_acquire));
}
void apply_cursor_visibility() noexcept {
    if (!cursor_visibility_ready.load(std::memory_order_acquire)) { cursor_visibility_status.store(3); return; }
    if (GetCurrentThreadId() != cursor_owner_thread.load(std::memory_order_acquire)) {
        std::uint32_t expected_status = 0;
        cursor_visibility_status.compare_exchange_strong(expected_status, 2U); return;
    }
    cursor_apply_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    CURSORINFO info{sizeof(CURSORINFO)};
    if (!GetCursorInfo(&info)) return;
    const bool hide = cursor_hide_requested();
    const auto change = cursor_visibility_overlay.step(hide, (info.flags & CURSOR_SHOWING) != 0);
    // This exact validated DX11 function never dereferences its this argument.
    // It normalizes ShowCursor's counter sign instead of incrementing it blindly.
    if (change.has_value()) cursor_show_original(nullptr, *change);
    cursor_visibility_status.store(hide ? 1U : 0U, std::memory_order_relaxed);
}
void publish_cursor_capture(HWND window, bool captured) noexcept {
    DWORD pid=0;
    const auto owner = window != nullptr ? GetWindowThreadProcessId(window,&pid) : 0;
    if (owner != 0 && pid == GetCurrentProcessId()) {
        cursor_game_window.store(window, std::memory_order_release);
        cursor_owner_thread.store(owner, std::memory_order_release);
    } else captured = false;
    cursor_capture_tick.store(GetTickCount64(), std::memory_order_release);
    cursor_capture_requested.store(captured, std::memory_order_release);
    apply_cursor_visibility();
}
void release_cursor_visibility() noexcept {
    cursor_capture_requested.store(false, std::memory_order_release);
    apply_cursor_visibility();
}
void __fastcall cursor_show_replacement(void* device, bool visible) noexcept {
    if (GetCurrentThreadId() == cursor_owner_thread.load(std::memory_order_acquire)) {
        const bool hide = cursor_hide_requested();
        visible = cursor_visibility_overlay.native_request(visible, hide);
        if (hide) {
            CURSORINFO info{sizeof(CURSORINFO)};
            if (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) == 0) return;
        }
    }
    cursor_show_original(device,visible);
}
void __fastcall cursor_present_replacement(void* device, void* surface) noexcept {
    // Apply before Present can block for VSync, not after the frame is displayed.
    // Also restores a stale request if menus pause camera updates.
    apply_cursor_visibility();
    cursor_present_original(device,surface);
}
bool prepare_cursor_visibility() noexcept {
    try {
    const auto module = GetModuleHandleW(L"Direct3D11.dll");
    if (module == nullptr) { cursor_visibility_status.store(3); return true; } // DX9: unchanged
    constexpr std::array<std::uint8_t,32> expected{0x3a,0xa8,0x1f,0x51,0xe5,0xb8,0x58,0xf2,0x78,0x86,0x31,0x8c,0x4a,0x33,0xf3,0xcc,0x1b,0x6d,0x90,0xb8,0x4d,0xb9,0xac,0xf2,0xf5,0x73,0x43,0xfc,0xd4,0xf9,0xab,0x3a};
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if (length==0 || length>=path.size() || !validate_hash(path.data(),expected)) return false;
    constexpr std::uint8_t show_prefix[]{0x40,0x53,0x48,0x83,0xec,0x20,0x33,0xc9,0x0f,0xb6,0xda,0x84,0xd2,0x0f,0x94,0xc1};
    constexpr std::uint8_t present_prefix[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48};
    const AccessPoint points[]{
        {"?ShowHardwareCursor@Direct3DDevice11@GAME@@UEAAX_N@Z",show_prefix,std::size(show_prefix),0x9f30},
        {"?PresentSurface@Direct3DDevice11@GAME@@UEAAXPEAVRenderSurface@2@@Z",present_prefix,std::size(present_prefix),0x7c40}};
    if (!validate_access_points(module,points,std::size(points))) return false;
    cursor_show_original=reinterpret_cast<CursorShow>(GetProcAddress(module,"?ShowHardwareCursor@Direct3DDevice11@GAME@@UEAAX_N@Z"));
    cursor_present_original=reinterpret_cast<CursorPresent>(GetProcAddress(module,"?PresentSurface@Direct3DDevice11@GAME@@UEAAXPEAVRenderSurface@2@@Z"));
    const auto base=reinterpret_cast<std::uintptr_t>(module);
    return reinterpret_cast<std::uintptr_t>(cursor_show_original)==base+0x9f30 &&
        reinterpret_cast<std::uintptr_t>(cursor_present_original)==base+0x7c40;
    } catch (...) { return false; }
}
bool attach_cursor_visibility() noexcept {
    if (cursor_show_original==nullptr) return true;
    const bool attached = cursor_show_hook.attach(reinterpret_cast<void**>(&cursor_show_original),reinterpret_cast<void*>(&cursor_show_replacement))==NO_ERROR &&
        cursor_present_hook.attach(reinterpret_cast<void**>(&cursor_present_original),reinterpret_cast<void*>(&cursor_present_replacement))==NO_ERROR;
    cursor_visibility_ready.store(attached, std::memory_order_release);
    return attached;
}
void rollback_cursor_visibility() noexcept {
    cursor_visibility_ready.store(false, std::memory_order_release);
    if (cursor_present_hook.installed()) static_cast<void>(cursor_present_hook.detach());
    if (cursor_show_hook.installed()) static_cast<void>(cursor_show_hook.detach());
}
