# Native F8 fallback sky prototype

Based on fork commit 5b09f3d (merged mouse cursor fix). This branch does not install into or start the game.

The validated Engine.dll exports Region::AddToScene at RVA 0x193e30, Skybox constructor at 0x1b1460 and Skybox::AddToScene at 0x1b1770. Initialization checks their export addresses and instruction prefixes in addition to the existing module SHA-256 gate. Engine-owned string assignment at 0x23180 is prefix-checked. The Skybox constructor consumes its native by-value string; the runtime uses the engine allocator through the validated assignment routine instead of crossing CRT heaps.

Disassembly of Region::AddToScene establishes the current-region comparison at scene+8 -> world+0x2e8, main-pass flag at scene+0x10, region sky pointer at +0x198 and native camera translation at scene+0xb0. Region sky record at +0x178 uses the release MSVC 32-byte string layout (size at +0x188). Native Coords is 3x3 orientation followed by translation. These are build-specific facts, not portable engine APIs.

The hook calls original Region::AddToScene exactly once, then queues a fallback only for the main pass/current region with no authored sky record or object, a fresh camera callback, active runtime and third-person mode. It binds resource construction and submission to one render thread. No region/record pointer is overwritten. Camera-relative coordinates follow the original native sky submission. The one cached sky is retained through process exit because render queues may retain its mesh. Logical Stop and F8-off suppress future submissions without detaching.

Limitations: GDX3's grey sky is required in this first test; missing-resource behavior and appearance require live confirmation. Indoor classification and user-selectable/per-mod profiles are not implemented. Existing broken skies remain untouched. Do not interpret a sky_submissions counter as proof of rendered pixels. Unknown engine builds are rejected. No shader/lighting/fog settings are modified.

Offline validation: Build-NativeValidation.ps1 -CameraCollision -GameRoot E:\TEMP\Grim Dawn passed, including the new skybox gate tests, cursor tests, existing camera/lifecycle/configuration tests, native module validation, injector checks and release hashes. No live game was launched. See PLAYER_GUIDE.md for the required live checks.


## Failed first live test and measured correction

The user reported no visible sky with test1. The test location/log is still needed to distinguish a skipped authored sky, missing GDX3 record, and successful submission with invisible geometry.

Extracting the actual eldritch mesh from the GDX2 Level Art archive proves its Y bounds are -114.7247..3.9290: it is a lower hemisphere. Test1 incorrectly submitted it with identity orientation. A ray/triangle audit at 5-degree increments across azimuth 0..355 and elevation 5..85 found 0/1224 sky intersections within the measured 250-unit far plane. Rotating 180 degrees about X gives 1224/1224 intersections, all no farther than 160.22 units, without reversing triangle winding. The render submission now uses that rotation. This geometric check does NOT verify actual game pixels, resource loading, indoor behavior, or fix the separate authored-Northshire gate. Do not call this a working release until the user test/log confirms it.


## Test3 log-driven corrections

User confirms test2 sky in Northshire and RoT, black sky in PathOfGrimDawn, and ground clipping across mods. Authorized session-20261001-005422.csv contains generations 1/3/5 with active render far planes 250/250/120. Sky submissions continue in all three; there are 1812 collision queries, 528 hits, zero query faults, and no minimum-arm clamp (smallest arm 13.2263). This rules out the proposed minimum-arm clamp as the observed failure. A 0.35-unit skin along a six-degree ray provides only 0.037 units of vertical clearance. Test3 adds four camera-envelope offset rays to the center query and dynamically fits sky size to the measured far plane. Offline regression tests and the full native build passed; live test3 results remain unconfirmed.


## Test3 user acceptance (2026-10-01)

The user corrected the earlier failed-camera screenshots: those were test2. With test3, the user confirms that the camera no longer goes through ground and that PathOfGrimDawn renders a sky. A black horizon band remains; the fallback mesh is an upper hemisphere after rotation and has 30 transparent rim vertices. A separate main-menu/mod-switch failure affecting F8 and logical Stop remains undiagnosed. These limitations were explicitly accepted for pushing and merging this version. No further runtime changes were made after the tested package.
