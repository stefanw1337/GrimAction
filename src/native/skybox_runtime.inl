// Included inside runtime.cpp's anonymous namespace. Engine.dll is hash-validated
// by initialization before these additional exports and instruction prefixes are checked.
// No map records or region pointers are changed. The native sky owns only visuals.
using SkyRegionDraw = void(__fastcall*)(void*, void*, const void*, const void*, const void*);
using SkyConstruct = void*(__fastcall*)(void*, void*);
using SkySubmit = void(__fastcall*)(const void*, void*, const void*, void*);
using SkyStringAssign = void*(__fastcall*)(void*, const void*, std::size_t, std::size_t);
SkyRegionDraw sky_region_original{};
SkyConstruct sky_construct{};
SkySubmit sky_submit{};
SkyStringAssign sky_string_assign{};
gdtpc::DetourHook sky_region_hook;
std::atomic<std::uint64_t> sky_tick{0};
std::atomic<DWORD> sky_thread{0};
alignas(16) unsigned char sky_storage[64]{}; // native Skybox has one owned mesh pointer
bool sky_attempted{}; // only the bound render thread accesses this and the storage

bool sky_guarded_submit(void* region, void* scene) noexcept {
    __try {
        auto* r = static_cast<unsigned char*>(region);
        auto* s = static_cast<unsigned char*>(scene);
        auto* world = *reinterpret_cast<unsigned char**>(s + 8);
        if (world == nullptr || !gdtpc::skybox_fallback_allowed(
            phase.load(std::memory_order_acquire) == GdTpcPhase::logging_active &&
                control_stop_requested.load(std::memory_order_acquire) == 0,
            camera_mode.load(std::memory_order_acquire) == 1,
            *reinterpret_cast<unsigned*>(s + 0x10) == 0,
            *reinterpret_cast<void**>(world + 0x2e8) == region,
            *reinterpret_cast<void**>(r + 0x198) != nullptr ||
                *reinterpret_cast<std::size_t*>(r + 0x188) != 0,
            GetTickCount64(), sky_tick.load(std::memory_order_acquire))) return false;
        if (!sky_attempted) {
            sky_attempted = true;
            // Engine-owned allocation: never pass a DLL-CRT std::string allocation
            // into a constructor which consumes and frees its by-value argument.
            struct NativeString { union { const char* ptr; char small[16]; }; std::size_t size; std::size_t capacity; };
            static_assert(sizeof(NativeString) == 32);
            constexpr char record[] = "records/level art/terrain/skybox_grey01a.dbr";
            NativeString borrowed{}; borrowed.ptr = record;
            borrowed.size = sizeof(record)-1; borrowed.capacity = sizeof(record)-1;
            NativeString owned{}; owned.capacity = 15;
            sky_string_assign(&owned, &borrowed, 0, ~std::size_t{0});
            sky_construct(sky_storage, &owned); // consumes native-owned string
        }
        if (*reinterpret_cast<void**>(sky_storage) == nullptr) {
            sky_status.store(2, std::memory_order_relaxed); return false;
        }
        // Reproduce Region::AddToScene's identity rotation and camera translation.
        // The original region call uses scene + B0/B4/B8, not the region origin.
        float coords[12]{1,0,0, 0,1,0, 0,0,1, 0,0,0};
        std::memcpy(coords+9, s+0xb0, sizeof(float)*3);
        sky_submit(sky_storage, region, coords, scene);
        sky_status.store(1, std::memory_order_relaxed);
        sky_submissions.fetch_add(1, std::memory_order_relaxed);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        sky_status.store(3, std::memory_order_release); return false;
    }
}
void __fastcall sky_region_draw(void* region, void* scene, const void* coords,
    const void* frustum, const void* world_frustum) noexcept {
    // Call the original exactly once. Keep resource creation on its render thread.
    sky_region_original(region, scene, coords, frustum, world_frustum);
    if (phase.load(std::memory_order_acquire) != GdTpcPhase::logging_active ||
        sky_status.load(std::memory_order_acquire) == 3) return;
    DWORD expected=0; const auto thread=GetCurrentThreadId();
    sky_thread.compare_exchange_strong(expected,thread);
    if (sky_thread.load() != thread) { sky_status.store(4); return; }
    static_cast<void>(sky_guarded_submit(region,scene));
}
bool prepare_skybox(HMODULE engine) noexcept {
    const auto base=reinterpret_cast<std::uintptr_t>(engine);
    sky_region_original=reinterpret_cast<SkyRegionDraw>(GetProcAddress(engine,
        "?AddToScene@Region@GAME@@QEAAXAEAVGraphicsScene@2@AEBVCoords@2@AEBVFrustum@2@PEBVWorldFrustum@2@@Z"));
    sky_construct=reinterpret_cast<SkyConstruct>(GetProcAddress(engine,
        "??0Skybox@GAME@@QEAA@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"));
    sky_submit=reinterpret_cast<SkySubmit>(GetProcAddress(engine,
        "?AddToScene@Skybox@GAME@@QEBAXPEAVRegion@2@AEBVCoords@2@AEAVGraphicsScene@2@@Z"));
    sky_string_assign=reinterpret_cast<SkyStringAssign>(base+0x23180);
    const unsigned char draw[]{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x57,0x48,0x83,0xec,0x50,0x0f};
    const unsigned char ctor[]{0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d};
    const unsigned char submit[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48};
    const unsigned char assign[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x41};
    return reinterpret_cast<std::uintptr_t>(sky_region_original)==base+0x193e30 &&
        reinterpret_cast<std::uintptr_t>(sky_construct)==base+0x1b1460 &&
        reinterpret_cast<std::uintptr_t>(sky_submit)==base+0x1b1770 &&
        std::memcmp(reinterpret_cast<void*>(sky_region_original),draw,sizeof(draw))==0 &&
        std::memcmp(reinterpret_cast<void*>(sky_construct),ctor,sizeof(ctor))==0 &&
        std::memcmp(reinterpret_cast<void*>(sky_submit),submit,sizeof(submit))==0 &&
        std::memcmp(reinterpret_cast<void*>(sky_string_assign),assign,sizeof(assign))==0;
}
