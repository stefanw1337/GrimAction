# GrimAction design overview

This is a summary for contributors: what the runtime does, how it's put together, and the rules it follows. Grim Dawn
internals below are for the one supported build (October 2026 patch, x64; oct2026-d3915e25-x64). Every address is checked at load, and a mismatch
refuses to load.

Some source comments cite sections of `IMPLEMENTATION_PLAN.md`. That working plan and its session logs aren't published; this
document summarizes what they settled.

## Shape

```text
Start GrimAction.cmd ─► Start-GrimAction.ps1 ─► GdTpc.Injector.exe ──(CreateRemoteThread: LoadLibraryW)──► Grim Dawn.exe
                                                        │                                                     │
                                                        └── --check-config (loads the DLL locally)             ▼
                                                                                              gdtpc_runtime_collision.dll
                                                                                              one Detours hook on
                                                                                              GameCamera::UpdateFromInputImpl
```

- **Injector** (`src/GdTpc.Injector`, C#): validates the installed game files through the runtime's own export, finds the
  64-bit game process, refuses if the runtime is already loaded, loads the DLL, then calls a versioned initialization entry
  with a request block (settings path, log path). It accepts only a fully active status: one hook, settings loaded, telemetry
  writer healthy. It never unloads anything. `--collision-stop` requests a logical stop through a second versioned entry.
- **Runtime** (`src/native`, C++20): installs exactly one Detours hook. The callback calls the original function exactly
  once, then runs the camera features on the game's camera object. A background thread writes CSV telemetry from a bounded
  ring.
- **Launcher** (`package/`): PowerShell and cmd files for players. It checks the package hashes, checks settings, finds the
  game through Steam, and maps injector exit codes to plain messages.

## Safety rules (non-negotiable)

1. **Fail closed.** Executable and module SHA-256, every export RVA and code prefix, internal code prefixes, and data export
   RVAs must match `config/supported-builds.json`. The build script cross-checks these constants against the source and the
   installed module files. Any mismatch means the runtime doesn't load, or stays inert.
2. **One hook, no detach.** Once installed, the hook is never removed and the DLL is never unloaded while the game runs. Stop
   is logical: it restores the camera, stops writes and joins the writer. An operation whose completion can't be confirmed is
   reported as indeterminate, and retrying is prohibited until the game exits.
3. **Minimal writes.** Only camera fields are written, and each through an ownership overlay. An overlay writes a value only
   if the field still holds the runtime's last write (or the engine's clamp of it); otherwise the engine or the player changed
   it and the overlay yields. Every read and write is structured-exception guarded, and repeated faults latch a feature off
   for the session.
4. **Input.** The runtime moves the OS cursor and writes camera yaw and pitch only while mouse look applies. It never polls
   XInput (that broke controller input) and never forces the game's input mode (the game resets it every frame).
5. **Strict settings.** Every key is required, and unknown, duplicate or out-of-range values reject the whole file. Features
   that depend on others are refused without them (for example, mouse look requires collision).
6. **Immutable releases.** Builds refuse to run while Grim Dawn is open. Each release is a timestamped folder with a SHA-256
   manifest. Two builds of the same source are byte-identical (`/Brepro`). Runtime images that failed live are refused by
   hash through `config/prohibited-releases.json`.
7. **No AV evasion.** Binaries are never reshaped to avoid antivirus detection. See `ANTIVIRUS.md`.

## Features

All of these are pure models (`*_model.cpp`) with standalone test executables. `runtime.cpp` does the memory access and
wiring.

### Profile switch (F8)

Two independent camera profiles: the native isometric camera and third person, each with its own zoom, distance range,
pitch range and FOV. The switch captures a snapshot of the native camera and restores it exactly. Zoom is stored by the
engine as a blend between the profile's distance endpoints, and a blend outside [0, 1] makes snapshots invalid. That's why
`distance_min` and the collision floor are both 4.

### Collision (spring arm)

The runtime casts a ray from the look-at point to the desired camera position through the engine's level query. It pulls
the arm in with a skin margin and extends it back at a limited rate, after a few clear queries. It holds the engine's zoom
target while shortened and hands it back on release.

### Mouse look

Arbitration each frame: **ineligible** (not third person, not foreground, loading) > **Left Alt** (cursor free) >
**panel open** (cursor free) > **captured**.

- **Captured:** mouse delta turns requested yaw and adds a pitch offset. The cursor is warped to an aim point on screen
  (between `aim_band_top` and `aim_band_bottom`), so the game's own cursor-based targeting aims where the camera looks.
  Every capture edge re-anchors, so a released cursor never produces a jump.
- **Panel open:** flag bytes in the game UI object for inventory/character/stash/vendor, quests, skills, map and the Escape
  menu. Whole-qword tests were wrong because padding bytes vary.
- **NPC dialog:** the player controller's executing state is compared with the TalkToNpc state vtable. The controller is
  resolved through the game's object manager.

### Virtual zoom

The engine's own zoom distance drives targeting reach, so shortening it for a close camera also shortened how far you
could click. Virtual zoom holds the engine at a far distance (E = 85) and draws the camera at a separate visual distance V:

- scroll clicks on the held target step V (x1.14, clamped to [4, E], smoothed);
- collision is tested on V;
- the eye is pulled in along the view direction through the camera's eye offset by (D − V), where D is the engine's
  distance;
- pitch follows V with the same curve the engine uses for distance.

F8, alt-tab and loading screens hand V back to the engine first, so the native camera sees a consistent zoom.

### Shoulder offset (F9)

Cycles center, right and left. The camera target is offset sideways and upward relative to yaw, and collision re-queries on
every change.

### Far plane

In third person the world camera's far plane (`WorldCamera+0x18`) is scaled to `third_person_far_plane_percent`. This gates
distant scenery loading, which causes hitches when the camera looks toward the horizon. While virtual zoom is holding,
the result preserves the capped sector's default-camera scenery depth beyond the target as the rendered arm extends.
The runtime restores the sector's native value when leaving third person.

## Telemetry

One CSV row per camera frame (98 columns) goes to the log path given at injection. The build script checks that the header
and format string have the same column count. Logs are local only.

## What doesn't work (yet)

- **Hiding the game's hand cursor from inside the runtime.** The game draws it itself in the SteamStub-protected
  executable. Two optional extras work around it: `extras/hidden-hand-cursor` replaces the cursor texture in the game's
  own `UI.arc` with a dot (the only thing that edits a game file, and only when the player runs it), and
  `extras/reshade/ThirdPersonDot.fx` draws a dot over the frame instead.
- **Aim magnetism.** Scaling the game's native selection bias had no effect; a real version would need a cursor snap.
- `mouse_look_dot_cursor` is parsed and validated but has no effect.

Right-stick pitch **works** as of 0.2.0. Persistent GameEngine/UI/InputDevice probes found no axis, so with the player's
explicit approval the runtime hooks exact-build-validated `SteamControllerDevice::Update`, calls the original first, and
only observes its completed event vector, never modifying, suppressing or synthesizing events. The camera action id was
identified in a marked live session before any pitch write. XInput polling remains prohibited.

## Adding support for a new game build

1. Run `scripts/Test-SupportedBuild.ps1`. It fails on the new build.
2. Re-derive every export RVA and prefix, internal code prefix and data export in `config/supported-builds.json` for the new
   `Game.dll`/`Engine.dll`, and the three file hashes. Update the matching constants in `src/native/runtime.cpp`. The build
   script refuses to build if the two disagree or if an internal prefix doesn't match the installed module file.
3. Re-verify every structure offset used in `runtime.cpp` (camera, UI flags, controller state) before any live test.
