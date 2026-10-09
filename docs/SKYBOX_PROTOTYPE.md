# Fallback sky rendering

GrimAction can submit a stock sky mesh while the third-person camera is active and the current region has no authored sky. Authored skies remain under the game's control. No map-specific paths or lighting overrides are used.

## Runtime behavior

- Validate the engine fingerprint, exports and internal string helper before installing hooks.
- Call the original region renderer exactly once.
- Submit the fallback only in the main scene, for the current region, with a recent camera heartbeat.
- Create the stock resource on the bound render thread using engine-owned string storage.
- Rotate the stock lower hemisphere upward and fit its radius within 80 percent of the render far plane.
- Stop custom submissions when F8 is off, logical Stop is requested, or the heartbeat expires.

The fallback uses `records/level art/terrain/skybox_grey01a.dbr` and its stock expansion mesh. Missing resources are reported without replacing the game's authored sky.

## Telemetry

`sky_status`: 0 idle, 1 fallback submitted, 2 resource unavailable, 3 guarded failure, 4 unexpected render thread. The former value 5 is reserved and is no longer emitted.

A submission count proves a render call was made, not that the expected pixels were visible.

## Validation and limits

Policy tests cover scene selection, authored-sky exclusion, heartbeat expiry and radius bounds. Native validation checks the exact supported binaries. In-game checks must cover F8, world changes, menus and logical Stop.

A dark band may remain at the horizon because the mesh fades at its rim. Automatic indoor classification is not implemented. Fully restarting the game remains the workaround for the reported camera/Stop failure after world changes.
