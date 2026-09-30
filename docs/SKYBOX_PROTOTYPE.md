# Native F8 fallback sky prototype

Based on fork commit 5b09f3d (merged mouse cursor fix). This branch does not install into or start the game.

The validated Engine.dll exports Region::AddToScene at RVA 0x193e30, Skybox constructor at 0x1b1460 and Skybox::AddToScene at 0x1b1770. Initialization checks their export addresses and instruction prefixes in addition to the existing module SHA-256 gate. Engine-owned string assignment at 0x23180 is prefix-checked. The Skybox constructor consumes its native by-value string; the runtime uses the engine allocator through the validated assignment routine instead of crossing CRT heaps.

Disassembly of Region::AddToScene establishes the current-region comparison at scene+8 -> world+0x2e8, main-pass flag at scene+0x10, region sky pointer at +0x198 and native camera translation at scene+0xb0. Region sky record at +0x178 uses the release MSVC 32-byte string layout (size at +0x188). Native Coords is 3x3 orientation followed by translation. These are build-specific facts, not portable engine APIs.

The hook calls original Region::AddToScene exactly once, then queues a fallback only for the main pass/current region with no authored sky record or object, a fresh camera callback, active runtime and third-person mode. It binds resource construction and submission to one render thread. No region/record pointer is overwritten. Camera-relative coordinates follow the original native sky submission. The one cached sky is retained through process exit because render queues may retain its mesh. Logical Stop and F8-off suppress future submissions without detaching.

Limitations: GDX3's grey sky is required in this first test; missing-resource behavior and appearance require live confirmation. Indoor classification and user-selectable/per-mod profiles are not implemented. Existing broken skies remain untouched. Do not interpret a sky_submissions counter as proof of rendered pixels. Unknown engine builds are rejected. No shader/lighting/fog settings are modified.

Offline validation: Build-NativeValidation.ps1 -CameraCollision -GameRoot E:\TEMP\Grim Dawn passed, including the new skybox gate tests, cursor tests, existing camera/lifecycle/configuration tests, native module validation, injector checks and release hashes. No live game was launched. See PLAYER_GUIDE.md for the required live checks.
