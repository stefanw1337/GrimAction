# GrimAction 0.2.4

GrimAction adds an over-the-shoulder, mouse-look camera to Grim Dawn. Movement stays the game's own WASD (or controller)
movement. Press F8 in game to switch between the normal camera and third person.

GrimAction runs from its own folder and does not replace game files. Start it after the game reaches the main menu.
Fully exiting the game removes the loaded runtime.

## Requirements

- Grim Dawn on Steam, **64-bit**, at the game version this release supports (see "Supported game version" below).
- Windows 10 or 11, 64-bit.
- Nothing else. The download includes everything it needs (no .NET install).

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

- **Cursor visibility:** the DX11 cursor is hidden during captured mouse look. See `CURSOR_TEST.md` for the release checks. The legacy dot-cursor setting is ignored.
- **Controller:** a controller works with the game's own controller camera, and the right stick now also tilts the camera
  up and down (turn it off with `right_stick_pitch_enabled=false`).
- **After an NPC dialog** the cursor stays free until you move; moving puts it back to mouse look.
- **Menus not covered:** devotion, blacksmith, rift gates and the death screen don't free the cursor automatically. Hold Left
  Alt.
- **Occasional hitches** in some outdoor areas (for example the bridge between Lower Crossing and Devil's Crossing). They
  come from the game loading distant scenery. The view-distance setting can reduce scenery loading; see
  `third_person_far_plane_percent`.
- **Change graphics settings before starting GrimAction**, or restart the game after changing them.
- **Collision:** low camera angles can still pass through terrain, and some small objects are not detected. The new downward probes are an improvement under test, not a guarantee of complete ground protection.
- **Sky:** the fallback background can leave a dark horizon band. It uses stock expansion assets and does not change sunlight or map lighting.
- **Session changes:** if F8 or logical Stop stops responding after returning to the main menu or changing worlds, fully exit and restart the game.
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
| `third_person_far_plane_percent` | 100 | 50 to 100 | Percentage of native view distance. Lower values can reduce scenery loading. |
| `right_stick_pitch_enabled` | true | true / false | Controller right stick tilts the camera up and down |
| `right_stick_pitch_degrees_per_second` | 90 | 10 to 360 | How fast the right stick tilts |
| `right_stick_pitch_invert` | false | true / false | Flip the right stick's up and down |
| `npc_dialog_releases_cursor` | true | true / false | Free the cursor while talking to NPCs |
| `menu_release_frames` | 3 | 1 to 60 | Frames a menu must stay closed before mouse look returns |

Leave these alone unless you know why you're changing them: `distance_min` and `collision_min_distance` (both 4; lower
values stop F8 from switching back), `virtual_zoom_engine_distance`, the `collision_*` values, and the `[general]` section
(fixed in this version: F8, starting in the normal camera). `mouse_look_dot_cursor` and `ui_probe_enabled` are reserved for
features that don't work yet; leave them `false`.

## Safety and what it does

GrimAction is a DLL that is loaded into the running game (DLL injection) and adjusts the camera each frame. It:

- checks that your installed game files exactly match the version it was built for, and refuses to load otherwise;
- adjusts the camera, cursor capture and fallback sky rendering while active;
- keeps session logs locally and does not send telemetry;
- can't be unloaded while the game runs. Stop turns it off; quitting the game removes it.

The source code is public, and the fixed package files are listed with their SHA-256 hashes in `bin\package-manifest.json`.

## Antivirus

Because GrimAction injects a DLL, some antivirus programs flag it as a "trojan/injector". A detection needs investigation; it is not automatically a false positive. Please
do not disable your antivirus. `ANTIVIRUS.md` explains your options, including a narrow exclusion for the GrimAction folder
only.

## Uninstall

Fully exit Grim Dawn, then delete the GrimAction folder. Remove any antivirus exclusion you previously added for it.

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

Offline compatibility checks pass for the x64 game files identified by `oct2026-d3915e25-x64` in the compatibility manifest. When Grim Dawn updates, GrimAction refuses to load until a
matching release is published. This release still requires in-game acceptance testing on that patch.

## License

MIT. See `LICENSE.txt`. Includes Microsoft Detours (MIT), see `THIRD-PARTY-NOTICES.txt`. Grim Dawn is a trademark of Crate
Entertainment. GrimAction is an unofficial fan project, not affiliated with or endorsed by Crate Entertainment.
