# Building GrimAction

## Requirements

- Windows 10/11 x64.
- Visual Studio 2022 or Build Tools with the **Desktop development with C++** workload (MSVC x64, Windows SDK).
- .NET 10 SDK. Build scripts use `%USERPROFILE%\.dotnet\dotnet.exe` if present, otherwise `dotnet` on `PATH`.
- Grim Dawn installed at the supported build. Builds validate constants against the installed `Game.dll`/`Engine.dll` and
  run an offline harness against the game files. Pass `-GameRoot` if Grim Dawn is not in the default Steam folder.
- **Grim Dawn must be closed** for every build step. The scripts check before each compile, link and write, and refuse
  otherwise.

## Steps

```powershell
# 1. Build the pinned Microsoft Detours library (once).
.\scripts\Build-Detours.ps1

# 2. Check that your installed game is the supported build.
.\scripts\Test-SupportedBuild.ps1 -GameRoot 'D:\SteamLibrary\steamapps\common\Grim Dawn'

# 3. Build the runtime, run every offline test, and write an immutable release.
.\scripts\Build-NativeValidation.ps1 -CameraCollision -GameRoot 'D:\SteamLibrary\steamapps\common\Grim Dawn'
#    -> artifacts\releases\collision\<release-id>\

# 4. Build the player package (self-contained injector, launcher, docs, zip + SHA-256).
.\scripts\Build-PlayerPackage.ps1 -ReleasePath artifacts\releases\collision\<release-id> -Version 0.2.3
#    -> artifacts\packages\GrimAction-0.2.3.zip
```

Step 3 compiles with `/W4 /WX /sdl /GS /GUARD:CF` and links with `/Brepro`. Building the same source twice gives
byte-identical releases, which you can use to verify a download against a local build.

## Running a development release

```powershell
# Game at the main menu:
.\scripts\Run-CameraCollision.ps1 -ReleasePath artifacts\releases\collision\<release-id>
# Optional logical stop:
.\scripts\Stop-CameraCollision.ps1 -ReleasePath artifacts\releases\collision\<release-id>
```

`Run-CameraCollision.ps1` verifies the release manifest and the prohibited-release list before injecting, and writes the
telemetry CSV to `artifacts\observation\collision`. It injects once per game process. To inject again, quit the game
normally.

## Testing rules for contributors

- One injection per game process. Never add an unload or detach path.
- A release that misbehaves live goes into `config/prohibited-releases.json` by runtime SHA-256, with the reason and the
  release that supersedes it.
- Keep model logic in `*_model.cpp` with a `*_tests.cpp` executable, and add it to `Build-NativeValidation.ps1`.
- New settings keys need a bit in the parser's key mask, a range check, an entry in `config/runtime.example.ini`, and a row
  in `docs/PLAYER_GUIDE.md`.

## Antivirus

Defender may flag build outputs (see `docs/ANTIVIRUS.md`). Add a narrow exclusion for `artifacts\releases` if needed.
Never change the code to avoid a detection.

The player ZIP contains one README for setup, controls and troubleshooting. The cursor acceptance checklist stays in the source repository. Local packaging revisions before publication keep the planned release version; preserve earlier local candidates before rebuilding. Published versions must not be replaced.
