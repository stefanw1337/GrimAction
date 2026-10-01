# GrimAction skybox test3

This is a GrimAction test package, not a WoW-for-GrimDawn mod update. The cursor fix is preserved. No game installation files or map lighting are changed.

## Confirmed by the user in test2

F8 shows the Northshire sky and restores the black/native view when disabled. Reign of Terror also shows a different sky. The user subsequently confirmed that test3 shows the PathOfGrimDawn sky and prevents the reported camera penetration through the ground.

## Test3 changes

The session log shows 250-unit render far planes in the first two tested worlds and 120 in the third, while fallback submissions continued in all three. The fallback mesh reaches about 166 units, so test3 scales it to fit within 80 percent of the current render far plane. Northshire keeps its own blue sky and its F8-only scale correction, now also respecting the current far plane. Other authored skies are preserved. No sun or lighting changes are added to Reign of Terror. The fallback still relies on the stock GDX3 record and its GDX2 mesh.

Camera collision now tests a 0.6-unit envelope with lower, upper and lateral offset rays instead of only the center ray. This addresses terrain/near-camera intersection at grazing angles. It is an approximation, not a full swept-volume collision solver; geometry without collision can still need map fixes. A regression test covers a center ray that misses flat ground while the camera envelope intersects it.

## Known limitations accepted for this version

- A black band remains near the fallback sky horizon. The stock mesh covers the upper hemisphere and fades at its rim; complete background coverage is not implemented.
- The user reported F8 and logical Stop failing after returning to the main menu and switching mods. This remains undiagnosed; fully restart the game between mods if it occurs.
- View distance above 100 percent is not supported.

## Testing

Quit the game fully before switching builds. Extract separately and use Start GrimAction.cmd. Test PathOfGrimDawn's sky at F8, then ground-level camera angles and walls in Northshire and RoT. Check F8 off, menus and the cursor fix. Retain dark map lighting. Sky clouds/sun artwork for WoW is a separate pending change. Automatic indoor classification and per-mod sky profiles are not implemented.

CSV sky_status: 0 idle, 1 fallback queued, 2 unavailable resource, 3 latched fault, 4 another render thread, 5 Northshire correction queued. sky_submissions is cumulative and does not prove rendered pixels. End the game normally or use logical Stop to flush the log. No live game was launched by the developer. Live confirmations were supplied by the user.

---

# Cursor visibility test build (unreleased fork)

This build adds conditional cursor hiding for the validated DX11 backend. It does not replace cursor textures and never displays a dot. Run the checks in TEST-MUSEPEKER.md before publishing. Offline tests are not evidence of in-game success.

The existing `mouse_look_dot_cursor` key is retained for INI compatibility but ignored. Leave it false. No new setting is required: F8 mouse capture controls hiding. F8 off, menus, NPC conversation, Alt and loss of focus release it. DX9 remains unchanged. Unknown DX11 binaries are rejected before installing hooks. A stale capture request expires after 250 ms on owner-thread callbacks.

Telemetry adds cursor_visibility (0 released, 1 hidden, 2 no owner-thread application yet, 3 backend unavailable), cursor_owner and cursor_thread. UI detection gaps from upstream still require testing; do not claim every panel is supported.

---

# GrimAction: third-person camera for Grim Dawn

GrimAction adds an over-the-shoulder, mouse-look camera to Grim Dawn. Movement stays the game's own WASD (or controller)
movement. Press F8 in game to switch between the normal camera and third person.

Nothing is installed. GrimAction never copies files into the Grim Dawn folder or changes game files (the optional dot
cursor below is the one exception, and only if you run it). You start it after the game reaches the main menu, and it is
gone when you quit the game.

## Requirements

- Grim Dawn on Steam, **64-bit**, at the game version this release supports (see "Supported game version" below).
- Windows 10 or 11, 64-bit.
- Nothing else. The download includes everything it needs (no .NET install).
- Optional: the dot cursor (below) replaces the game's hand cursor with an adjustable dot.

## Install

1. Extract the zip to any folder you like, for example `Documents\GrimAction`. Do **not** put it inside the Grim Dawn folder.
2. That's it.

Windows may say the files came from the internet. The launcher clears that mark on its own files after checking that they
are the files this release shipped with.

## Play

1. Start Grim Dawn normally from Steam and wait for the **main menu**.
2. Double-click **Start GrimAction**. A window opens, checks the files and your settings, and finds the game.
3. When it asks, make sure the main menu is showing and press **Enter**.
4. When you see "GrimAction is running", load your character. Press **F8** for third person.

Each time you relaunch Grim Dawn you run Start GrimAction again. It can only be started once per game session. If you
need to start it again, quit the game completely and relaunch.

To stop: just quit the game. If you want the normal camera back without quitting, double-click **Stop GrimAction**.

## Controls

| Input | Third person |
| --- | --- |
| **F8** | Switch between the normal camera and third person |
| **Mouse** | Look around (turn and tilt the camera). Your attacks go where the camera points. |
| **WASD** | Move (the game's own movement) |
| **Mouse wheel** | Zoom the camera in and out |
| **F9** | Cycle the camera shoulder: center, right, left |
| **Right stick** (controller) | Tilt the camera up and down |
| **Hold Left Alt** | Free the mouse cursor (loot, click the world, use the UI) |
| Open a menu | Inventory, character, skills, quests, map, stash, vendors and the Escape menu free the cursor automatically |
| Talk to an NPC | Frees the cursor for the dialog |

You need WASD movement: in the game options, turn on movement with the keyboard (WASD) if it isn't already.

## Known limits

- **Cursor visibility:** this fork tests conditional hiding during captured DX11 mouse look; see the test-build notes above.
- **Controller:** a controller works with the game's own controller camera, and the right stick now also tilts the camera
  up and down (turn it off with `right_stick_pitch_enabled=false`).
- **After an NPC dialog** the cursor stays free until you move; moving puts it back to mouse look.
- **Menus not covered:** devotion, blacksmith, rift gates and the death screen don't free the cursor automatically. Hold Left
  Alt.
- **Occasional hitches** in some outdoor areas (for example the bridge between Lower Crossing and Devil's Crossing). They
  come from the game loading distant scenery. The default view distance is slightly shortened (95%) to reduce them; see
  `third_person_far_plane_percent`.
- **Change graphics settings before starting GrimAction**, or restart the game after changing them.
- **Collision** keeps the camera out of most walls, but some small objects (a pillar, for example) aren't detected.
- Mouse look is designed for single-player. Use it in multiplayer at your own discretion.

## Settings

Settings live in `settings\runtime.ini`. Open it in Notepad, change a value, save, and run Start GrimAction. Start
GrimAction checks the file first and refuses to start if a value is malformed or out of range. Every key must stay in the file. Lines starting with `;`
are comments. If you break something, copy `settings\runtime.default.ini` over `runtime.ini`.

Settings are read once, when GrimAction starts. To apply a change, quit the game, relaunch, and start GrimAction again.

The settings most people will want:

| Setting | Default | Range | What it does |
| --- | --- | --- | --- |
| `mouse_look_yaw_radians_per_pixel` | 0.002 | above 0 to 0.05 | Turn speed (mouse sensitivity, left and right) |
| `mouse_look_pitch_degrees_per_pixel` | 0.05 | above 0 to 2 | Tilt speed (mouse sensitivity, up and down) |
| `mouse_look_invert_x` | true | true / false | Flip left and right. Set to false if turning feels backwards. |
| `mouse_look_pitch_offset_min` | -60 | -90 to 0 | How far you can tilt up from the normal angle |
| `mouse_look_pitch_offset_max` | 25 | 0 to 60 | How far you can tilt down from the normal angle |
| `mouse_look_pitch_floor_degrees` | -6 | -80 to 45 | Highest the camera can look (negative is above the horizon) |
| `aim_start` | 0.5 | between the two band values | Where on screen your aim point sits (0 top, 1 bottom) |
| `aim_band_top` / `aim_band_bottom` | 0.25 / 0.75 | 0.05 to 0.95 | How far the aim point can travel up and down the screen |
| `shoulder_offset_units` | 0.75 | 0.25 to 8 | How far to the side the shoulder view sits (F9) |
| `shoulder_height_units` | 1.5 | 0 to 10 | How high the camera looks over the character |
| `distance_default` | 42 | between `distance_min` and `distance_max` | Starting zoom |
| `fov_degrees` | 45 | 30 to 60 | Field of view in third person |
| `third_person_far_plane_percent` | 95 | 50 to 100 | View distance. Lower reduces hitches and cuts off the horizon sooner; extreme zoom keeps a safety floor so the camera cannot outrun the rendered world. |
| `right_stick_pitch_enabled` | true | true / false | Controller right stick tilts the camera up and down |
| `right_stick_pitch_degrees_per_second` | 90 | 10 to 360 | How fast the right stick tilts |
| `right_stick_pitch_invert` | false | true / false | Flip the right stick's up and down |
| `npc_dialog_releases_cursor` | true | true / false | Free the cursor while talking to NPCs |
| `menu_release_frames` | 3 | 1 to 60 | Frames a menu must stay closed before mouse look returns |

Leave these alone unless you know why you're changing them: `distance_min` and `collision_min_distance` (both 4; lower
values stop F8 from switching back), `virtual_zoom_engine_distance`, the `collision_*` values, and the `[general]` section
(fixed in this version: F8, starting in the normal camera). `mouse_look_dot_cursor` and `ui_probe_enabled` are reserved for
features that don't work yet; leave them `false`.

## Legacy dot extras

Not included or used in this test build.

## Safety and what it does

GrimAction is a DLL that is loaded into the running game (DLL injection) and adjusts the camera each frame. It:

- checks that your installed game files exactly match the version it was built for, and refuses to load otherwise;
- only changes camera values, and moves the mouse cursor while mouse look is active;
- never connects to the internet and never changes files on disk, apart from its own log files (the optional dot cursor
  extra is the one exception, and only when you run it);
- can't be unloaded while the game runs. Stop turns it off; quitting the game removes it.

The source code is public, and every file in this download is listed with its SHA-256 hash in `bin\package-manifest.json`.

## Antivirus

Because GrimAction injects a DLL, some antivirus programs flag it as a "trojan/injector". These are false positives. Please
don't disable your antivirus. `ANTIVIRUS.md` explains your options, including a narrow exclusion for the GrimAction folder
only.

## Uninstall

If you applied the dot cursor, run `Restore Hand Cursor.cmd` first (or verify game files in Steam). Then delete the
GrimAction folder. If you added the ReShade dot, delete `ThirdPersonDot.fx` from the ReShade shaders folder. If you added an
antivirus exclusion, remove it.

## Troubleshooting

| Message | What to do |
| --- | --- |
| Grim Dawn was not found | Put your Grim Dawn folder (the one containing `x64\Grim Dawn.exe`) on its own line in `settings\game-path.txt`. |
| Grim Dawn is not running | Start the 64-bit game and wait for the main menu first. |
| This Grim Dawn version is not supported | The game was updated. Wait for a GrimAction update. Nothing was loaded. |
| Already loaded in this game session | Quit the game completely, relaunch, and start again. |
| Settings has an invalid value | Fix the line it names, or copy `runtime.default.ini` over `runtime.ini`. |
| Does not match this release | A file was damaged or changed (often by antivirus). Extract the zip again. |
| Windows did not allow access | Run the game and GrimAction the same way: both normally, or both as administrator. |
| Mouse look stopped working after changing graphics options | Quit the game, relaunch, and start GrimAction again. |

At the furthest zoom, looking almost horizontally can expose black above or beyond the level. Grim Dawn's isometric maps
do not include a complete sky or distant backdrop for that viewpoint. Zoom in slightly or look downward; normal play is
unaffected.

## Logs

Each session writes a CSV file of camera telemetry to `logs\`. Logs stay on your PC and are never sent anywhere. If you
report a bug, you can attach the log from that session. Delete old logs any time.

## Supported game version

This release supports Grim Dawn Steam build 24825149 (64-bit). When Grim Dawn updates, GrimAction refuses to load until a
matching release is published.

## License

MIT. See `LICENSE.txt`. Includes Microsoft Detours (MIT), see `THIRD-PARTY-NOTICES.txt`. Grim Dawn is a trademark of Crate
Entertainment. GrimAction is an unofficial fan project, not affiliated with or endorsed by Crate Entertainment.
