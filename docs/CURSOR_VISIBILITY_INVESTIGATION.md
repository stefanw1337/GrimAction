# Conditional cursor visibility investigation — 2026-09-24

## Requested behaviour
Hide the cursor only during active F8 third-person mouse capture. Restore immediately on F8 off, Escape/menus, NPC conversation, Left Alt, loss of focus, stop, or fault. No permanent blank/dot texture replacement. No reset credit consumed. Investigation only; no runtime DLL built or injected.

## Confirmed source evidence
- `src/native/runtime.cpp:2436` already receives `decision.captured` at `apply_dot_cursor`.
- `runtime.cpp:2197` reads/writes `WinWindow + 0x20`, then calls `SetCursor` for the existing dot attempt. This changes a handle, not cursor visibility. The observed failure does not prove that a renderer visibility hook will fail.
- `runtime.cpp:2360` onward gates capture on foreground, stable player/camera identity, readable menu state, active phase, no stop/fault, and third-person mode. NPC talk is folded into the menu signal.
- `mouse_look_model.cpp:138` onward distinguishes captured, Alt, menu, controller, disabled and ineligible states.
- `runtime.cpp:2118` force-release is the existing stop/fault cleanup point.
- Upstream documents the current dot option as ineffective. Do not ship it as a working fix.

## New native DX11 evidence (static analysis, not gameplay)
Read only `E:/TEMP/Grim Dawn/x64` developer copies. Engine.dll and Game.dll SHA-256 exactly match config/supported-builds.json.

Direct3D11.dll SHA-256:
`3aa81f51e5b858f27886318c4a33f3cc1b6d90b84db9acf2f57343fcd4f9ab3a`

Exported methods in this file:
- `?ShowHardwareCursor@Direct3DDevice11@GAME@@UEAAX_N@Z` RVA `0x9f30`.
- `?SetHardwareCursor@Direct3DDevice11@GAME@@UEAAXIHHPEBD@Z` RVA `0x9f90`.
- `?PresentSurface@Direct3DDevice11@GAME@@UEAAXPEAVRenderSurface@2@@Z` RVA `0x7c40`.

ShowHardwareCursor resolves its IAT calls to USER32.ShowCursor. It preserves the requested boolean from DL; for true it ensures a nonnegative display counter, for false a negative counter. It starts with one counter step in the opposite direction and then adjusts to the requested sign. Repeated blind ShowCursor calls must NOT be substituted; their counter semantics matter.

SetHardwareCursor imports CreateIconIndirect, CreateBitmap, SetDIBits and calls the window interface. This is evidence of an OS cursor path in the DX11 backend. It weakens the earlier blanket assumption that only an inaccessible software-rendered cursor can be involved. It does NOT yet prove this is the path active in the user's session.

## Proposed bounded implementation
1. Add exact module hash/export-prefix validation for Direct3D11.dll; never reuse these addresses on another binary or on DX9.
2. First instrument ShowHardwareCursor / SetHardwareCursor call timing and owner-thread IDs in a diagnostic build. Keep visibility unchanged in this probe. Establish which thread owns the effective cursor counter and whether another path redraws the pointer.
3. Publish an atomic hide request from the existing capture decision, with foreground/freshness/stop guards. Clear it on all release and early-return paths. Do not derive it only from F8 being enabled.
4. Override visibility on the observed owner thread, preserving the latest native visibility request. Reapply on native show calls while capture is active; restore native visibility immediately on release, even when no native show call occurs then. Use an observed owner-thread callback, not an assumed render-thread callback.
5. Default to visible/no suppression when signals are unreadable or stale. Do not alter cursor textures or steal mouse input. Preserve normal game-requested hidden cursors when no overlay is owned.
6. Add a separate setting (e.g. hide_cursor_while_captured), parser mask, example config, documentation, model tests and release manifest validation. Do not silently repurpose mouse_look_dot_cursor.

## Validation before claiming a fix
Unit-test capture/release, F8 off, Escape, inventory, NPC begin/end, Alt, focus loss, controller transition, stop/fault, stale signal and repeated requests without counter drift. Verify menu detection gaps documented upstream (devotion, blacksmith, rift gates, death screen) before enabling hiding there. Gameplay test restoration while camera callbacks are paused by menus. Verify both normal and large UI cursor settings.

## Current result
A concrete, exported DX11 visibility path has been identified. No source/runtime behaviour was modified, no game launched, no injection performed, no cursor texture replaced. The next step is a guarded diagnostic build to confirm callback/thread behaviour, followed by the conditional hook and user gameplay testing.

## Implementation update
Conditional DX11 visibility hooks and owner-thread capture integration are now implemented in cursor_visibility_runtime.inl. The initial no-behaviour-change status above describes the investigation baseline. Model tests and the native offline suite pass; in-game testing is still required. No runtime injection or publishing has been performed by the assistant.
