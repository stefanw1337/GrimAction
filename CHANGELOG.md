# Changelog

All notable player-facing changes to GrimAction are recorded here.

## 0.2.3 - 2026-10-09

### Changed

- Updated game-file fingerprints, native hook addresses and input/controller offsets for the supported October x64 game build.
- Extended offline validation to the DX11 cursor backend, sky renderer and internal helper routines.
- Removed project-specific sky handling; authored sky rendering is left to the game.
- Replaced development test notes with an English player guide.

### Camera collision

- Added downward clearance probes for one-sided ground surfaces. Low-angle terrain penetration remains a known limitation and requires further in-game testing.

### Validation

- Exact game-file and native entry-point checks remain required. Compatibility with unverified patches is not claimed.
- In-game acceptance of this release is pending. A dark fallback horizon band and camera/Stop failures after world changes remain known limitations.

## 0.2.0 - 2026-09-15

### Added

- Controller right stick now tilts the camera up and down in third person. Tune it with `right_stick_pitch_degrees_per_second` and `right_stick_pitch_invert`, or turn it off with `right_stick_pitch_enabled=false`.

### Fixed

- Maximum virtual zoom no longer outruns the rendered world in short-view-distance areas; the far-plane performance cap now keeps a safety margin beyond the camera target.
- Native camera shake is suppressed only while virtual zoom needs an eye pull, preventing shake from replacing that pull and briefly throwing the camera far away.
- Switching from a gamepad back to keyboard and mouse now recaptures mouse look cleanly, without requiring a return to the main menu.
- Rift travel no longer leaves mouse look released after using the rift button with Left Alt held.
- Factions and Loot Filter now release and recapture the cursor like the other supported windows.

### Changed

- The optional dot cursor is much larger and outlined, so it is easy to see without the ReShade dot. It is centred exactly on the click point (shown as its lower-right quarter), and its size, outline and shape (quarter or offset round) are settings at the top of `Set-HiddenHandCursor.ps1`.
- Mouse look now follows Grim Dawn's own keyboard/mouse versus controller mode instead of continuing to reposition the OS cursor during controller play.
- Escape-menu detection now requires both of its native UI flags, preventing unrelated Alt-clicks from being mistaken for an open Escape menu.

### Known limitations

- At maximum zoom, looking almost horizontally can expose black beyond Grim Dawn's isometric level scenery. Zooming in slightly or looking downward avoids it.

## 0.1.0 - 2026-09-13

Initial public release.

- Over-the-shoulder third-person camera toggled with F8.
- Mouse look with camera-relative aiming, vertical look, and Left Alt cursor release.
- Automatic cursor release for the main supported panels and NPC conversations.
- Camera collision, proportional virtual zoom, shoulder switching with F9, and a far-plane cap.
- Optional ReShade aim dot and dot/blank replacement for the game's hand cursor.
- Build fingerprint validation, exact restoration on logical stop, reproducible release manifests, and fail-closed behavior on unsupported game builds.
