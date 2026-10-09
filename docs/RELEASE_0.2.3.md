# GrimAction 0.2.3

Compatibility update for the supported October x64 Grim Dawn build (`oct2026-d3915e25-x64`).

## Changes

- Updated native hook addresses, game-file fingerprints and input/controller offsets.
- Extended offline validation for the DX11 cursor and sky rendering hooks.
- Added downward camera-clearance probes for one-sided ground.
- Kept authored skies under the game's control and removed project-specific runtime handling.
- Updated the English player guide and cursor test instructions.

## Install

Fully exit Grim Dawn before changing GrimAction versions. Extract `GrimAction-0.2.3.zip` to a separate folder, launch the x64 game, and run `Start GrimAction.cmd` at the main menu.

## Validation and limitations

Native regression tests, exact game-file validation and package integrity checks passed. The maintainer confirmed this release working in game on October 9, 2026.

Low-angle terrain penetration, a dark fallback horizon band, and camera/Stop failures after world changes remain known limitations. Fully restart the game if controls stop responding. View distance remains limited to 50-100 percent; experimental expansion is not included.

Only the exact verified game build is supported. This is not a claim of compatibility with every patch.
