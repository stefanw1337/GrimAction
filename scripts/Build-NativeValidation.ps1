[CmdletBinding()]
param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Grim Dawn',
    [ValidateSet(0, 1, 2)][int]$Gate = 0,
    [switch]$CameraCollision,
    # Research: also stage the read-only UI probe (F10 open / F11 closed marks). Collision variant only.
    [switch]$UiProbe
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$effectiveGate = if ($CameraCollision) { 2 } else { $Gate }
$releaseVariant = if ($CameraCollision) { 'camera-collision' } else { 'standard' }
$releaseRootName = if ($CameraCollision) { 'collision' } else { "gate$Gate" }

# Re-checked immediately before every compile, link, publish and artifact write, not only at
# start-up: a build takes minutes and the game may be launched part-way through.
function Assert-GameClosed {
    param([string]$Step)
    if (Get-Process -Name 'Grim Dawn' -ErrorAction SilentlyContinue) {
        throw "Grim Dawn is running. Source work is safe, but $releaseRootName builds and artifact replacement are refused ($Step)."
    }
}

function Invoke-BuildStep {
    param([string]$Step, [string]$Command)
    Assert-GameClosed -Step $Step
    & $env:ComSpec /d /s /c $Command
    if ($LASTEXITCODE -ne 0) { throw "$Step exited with code $LASTEXITCODE." }
}

Assert-GameClosed -Step 'start'
if ($UiProbe -and -not $CameraCollision) { throw '-UiProbe requires -CameraCollision.' }

# [System.IO.Path]::GetRelativePath does not exist on Windows PowerShell 5.1, so the release
# layout must not depend on the host PowerShell edition.
function Get-ReleaseRelativePath {
    param([string]$Root, [string]$FullName)
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd([char]92)
    $fileFull = [System.IO.Path]::GetFullPath($FullName)
    if (-not $fileFull.StartsWith($rootFull + [char]92, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the release root: $FullName"
    }
    return $fileFull.Substring($rootFull.Length + 1)
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools could not be located.'
}

$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'The Microsoft x64 C++ workload is not installed.'
}

$devcmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$nativeRoot = Join-Path $projectRoot 'src\native'
$releaseId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$artifactRoot = Join-Path $projectRoot "artifacts\releases\$releaseRootName\$releaseId"
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
# Compile intermediates are retained as evidence but kept outside the immutable release, so a
# release directory holds only deliverables and two same-source releases can be compared whole.
# Object files carry a compiler timestamp that /Brepro does not remove.
$objRoot = Join-Path $projectRoot "artifacts\build-intermediates\$releaseRootName\$releaseId"
New-Item -ItemType Directory -Path $objRoot -Force | Out-Null

$dllName = if ($CameraCollision) { 'gdtpc_runtime_collision.dll' } elseif ($Gate -eq 0) { 'gdtpc_runtime_logging.dll' } else { "gdtpc_runtime_gate$Gate.dll" }
$dll = Join-Path $artifactRoot $dllName
$harness = Join-Path $artifactRoot 'gdtpc_validation_harness.exe'
$controllerTests = Join-Path $artifactRoot 'gdtpc_controller_tests.exe'
$detourTests = Join-Path $artifactRoot 'gdtpc_detour_tests.exe'
$cameraMemoryTests = Join-Path $artifactRoot 'gdtpc_camera_memory_tests.exe'
$cameraWriteTests = Join-Path $artifactRoot 'gdtpc_camera_write_adapter_tests.exe'
$profileSwitchTests = Join-Path $artifactRoot 'gdtpc_profile_switch_tests.exe'
$runtimeConfigTests = Join-Path $artifactRoot 'gdtpc_runtime_config_tests.exe'
$aimMemoryTests = Join-Path $artifactRoot 'gdtpc_aim_memory_tests.exe'
$collisionTests = Join-Path $artifactRoot 'gdtpc_camera_collision_tests.exe'
$uiProbeTests = Join-Path $artifactRoot 'gdtpc_ui_probe_tests.exe'
$controllerEventTests = Join-Path $artifactRoot 'gdtpc_controller_event_tests.exe'
$mouseLookTests = Join-Path $artifactRoot 'gdtpc_mouse_look_tests.exe'
$virtualZoomTests = Join-Path $artifactRoot 'gdtpc_virtual_zoom_tests.exe'
$levelQueryTests = Join-Path $artifactRoot 'gdtpc_level_query_tests.exe'
$lifecycleTests = Join-Path $artifactRoot 'gdtpc_lifecycle_policy_tests.exe'
$telemetryTests = Join-Path $artifactRoot 'gdtpc_telemetry_tests.exe'
$runtimeHostTests = Join-Path $artifactRoot 'gdtpc_runtime_host_tests.exe'
$runtimeSource = Join-Path $nativeRoot 'runtime.cpp'
$manifestPath = Join-Path $projectRoot 'config\supported-builds.json'
$runtimeText = Get-Content -Raw -LiteralPath $runtimeSource
$headerMatch = [Regex]::Match($runtimeText, 'constexpr char telemetry_header\[\]\s*=\s*"([^"]*)";')
$formatMatch = [Regex]::Match($runtimeText, 'return std::snprintf\([^;]*?\r?\n\s*"([^"]*)"', [Text.RegularExpressions.RegexOptions]::Singleline)
if (-not $headerMatch.Success -or -not $formatMatch.Success) { throw 'Could not inspect the telemetry header and format literals.' }
$headerColumns = @($headerMatch.Groups[1].Value -split ',').Count
$formatColumns = [Regex]::Matches($formatMatch.Groups[1].Value, '%(?!%)(?:[-+ #0]*\d*)?(?:\.\d+)?(?:hh|h|ll|l|j|z|t|L)?[a-zA-Z]').Count
if ($headerColumns -ne $formatColumns) { throw "Telemetry schema mismatch: header has $headerColumns columns but format has $formatColumns fields." }
Write-Output "PASS: telemetry header and format both contain $headerColumns columns."
$supported = (Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json).builds[0]
foreach ($hash in @($supported.executableSha256, $supported.modules[0].sha256, $supported.modules[1].sha256)) {
    $bytes = (($hash -split '(..)' | Where-Object { $_ }) | ForEach-Object { '0x' + $_.ToLowerInvariant() }) -join ', '
    if (-not $runtimeText.Contains($bytes)) { throw "Native hash constants drifted from supported-builds.json: $hash" }
}
foreach ($module in $supported.modules) {
    foreach ($export in $module.exports) {
        $escapedName = [Regex]::Escape([string]$export.name)
        if ($runtimeText -notmatch ('AccessPoint\{"' + $escapedName + '"[^\r\n]*,\s*' + [string]$export.rva + '\}')) {
            throw "Native export/RVA drifted from supported-builds.json: $($export.name)"
        }
        $prefixBytes = (($export.prefixHex -split '(..)' | Where-Object { $_ }) | ForEach-Object { '0x' + $_.ToLowerInvariant() }) -join ', '
        if (-not $runtimeText.Contains($prefixBytes)) { throw "Native prefix drifted from supported-builds.json: $($export.name)" }
    }
}
foreach ($module in $supported.modules) {
    foreach ($code in @($module.internalCode | Where-Object { $_ })) {
        $codeBytes = (($code.prefixHex -split '(..)' | Where-Object { $_ }) | ForEach-Object { '0x' + $_.ToLowerInvariant() }) -join ', '
        if (-not $runtimeText.Contains($codeBytes) -or -not $runtimeText.Contains('= ' + [string]$code.rva + ';')) { throw "Native internal code constant drifted from supported-builds.json: $($code.name)" }
        # Unexported code has no export table entry to cross-check, so read the installed module file and compare the
        # prefix at the recorded RVA directly. A wrong RVA must fail the build, not a live injection.
        $modulePath = Join-Path (Join-Path $GameRoot 'x64') ([string]$module.file)
        $image = [System.IO.File]::ReadAllBytes($modulePath)
        $peOffset = [BitConverter]::ToInt32($image, 0x3c)
        $sectionCount = [BitConverter]::ToUInt16($image, $peOffset + 6)
        $optionalSize = [BitConverter]::ToUInt16($image, $peOffset + 20)
        $fileOffset = -1
        for ($section = 0; $section -lt $sectionCount; $section++) {
            $header = $peOffset + 24 + $optionalSize + 40 * $section
            $virtualSize = [BitConverter]::ToUInt32($image, $header + 8); $virtualAddress = [BitConverter]::ToUInt32($image, $header + 12)
            $rawPointer = [BitConverter]::ToUInt32($image, $header + 20)
            if ([uint32]$code.rva -ge $virtualAddress -and [uint32]$code.rva -lt $virtualAddress + $virtualSize) { $fileOffset = $rawPointer + [uint32]$code.rva - $virtualAddress }
        }
        $expected = [byte[]](($code.prefixHex -split '(..)' | Where-Object { $_ }) | ForEach-Object { [Convert]::ToByte($_, 16) })
        if ($fileOffset -lt 0 -or $fileOffset + $expected.Length -gt $image.Length) { throw "Internal code RVA is outside $($module.file): $($code.name)" }
        for ($i = 0; $i -lt $expected.Length; $i++) {
            if ($image[$fileOffset + $i] -ne $expected[$i]) { throw "Internal code prefix does not match $($module.file) at RVA $($code.rva): $($code.name)" }
        }
    }
    foreach ($data in @($module.dataExports | Where-Object { $_ })) {
        if (-not $runtimeText.Contains('DataExport{"' + [string]$data.name + '", ' + [string]$data.rva + '}')) { throw "Native data export drifted from supported-builds.json: $($data.name)" }
    }
}
Write-Output 'PASS: native hash/export/RVA/prefix constants (including internal code verified against the module file) match supported-builds.json.'
$harnessSource = Join-Path $nativeRoot 'validation_harness.cpp'
$runtimeObject = Join-Path $objRoot 'runtime.obj'
$harnessObject = Join-Path $objRoot 'validation_harness.obj'
$common = '/nologo /std:c++20 /W4 /WX /EHsc /O2 /GS /sdl /DUNICODE /D_UNICODE'

$detoursRoot = Join-Path $projectRoot 'third_party\detours'
$detoursInclude = Join-Path $detoursRoot 'include'
$detoursLibrary = Join-Path $detoursRoot 'lib.X64\detours.lib'
if (-not (Test-Path -LiteralPath $detoursLibrary)) {
    throw 'Pinned Microsoft Detours x64 library has not been built.'
}
$detourHookSource = Join-Path $nativeRoot 'detour_hook.cpp'
$runtimeDetourObject = Join-Path $objRoot 'runtime_detour_hook.obj'
$runtimeConfigSource = Join-Path $nativeRoot 'runtime_config.cpp'
$runtimeCameraSource = Join-Path $nativeRoot 'camera_memory_model.cpp'
$runtimeConfigObject = Join-Path $objRoot 'runtime_config.obj'
$runtimeCameraObject = Join-Path $objRoot 'runtime_camera_memory.obj'
$runtimeCollisionSource = Join-Path $nativeRoot 'camera_collision_model.cpp'
$runtimeCollisionObject = Join-Path $objRoot 'runtime_camera_collision.obj'
$runtimeMouseLookSource = Join-Path $nativeRoot 'mouse_look_model.cpp'
$runtimeMouseLookObject = Join-Path $objRoot 'runtime_mouse_look.obj'
$runtimeVirtualZoomSource = Join-Path $nativeRoot 'virtual_zoom_model.cpp'
$runtimeVirtualZoomObject = Join-Path $objRoot 'runtime_virtual_zoom.obj'
$gateDefine = if ($effectiveGate -gt 0) { ' /DGDTPC_GATE1_PROFILE_WRITES=1' } else { '' }
if ($CameraCollision) { $gateDefine += ' /DGDTPC_CAMERA_COLLISION=1' }
$runtimeExtraSources = if ($effectiveGate -gt 0) { " `"$nativeRoot\camera_write_adapter.cpp`" `"$nativeRoot\profile_switch_model.cpp`"" } else { '' }
$runtimeExtraObjects = ''
if ($CameraCollision) {
    $runtimeExtraSources += " `"$nativeRoot\level_query_adapter.cpp`" `"$nativeRoot\ui_probe_model.cpp`""
    $runtimeExtraObjects = " `"$objRoot\level_query_adapter.obj`" `"$objRoot\ui_probe_model.obj`""
}
$cursorTests = Join-Path $objRoot 'cursor_visibility_model_tests.exe'
Invoke-BuildStep -Step 'cursor visibility model tests compile' -Command "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$nativeRoot\cursor_visibility_model_tests.cpp`" /Fo`"$objRoot\cursor_visibility_model_tests.obj`" /Fe`"$cursorTests`""
& $cursorTests
if ($LASTEXITCODE -ne 0) { throw 'Cursor visibility model tests failed.' }
$runtimeCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common$gateDefine /c /I`"$detoursInclude`" `"$runtimeSource`"$runtimeExtraSources /Fo`"$objRoot\\`" && cl $common /c /I`"$detoursInclude`" `"$detourHookSource`" /Fo`"$runtimeDetourObject`" && cl $common /c `"$runtimeConfigSource`" /Fo`"$runtimeConfigObject`" && cl $common /c `"$runtimeCameraSource`" /Fo`"$runtimeCameraObject`" && cl $common /c `"$runtimeCollisionSource`" /Fo`"$runtimeCollisionObject`" && cl $common /c `"$runtimeMouseLookSource`" /Fo`"$runtimeMouseLookObject`" && cl $common /c `"$runtimeVirtualZoomSource`" /Fo`"$runtimeVirtualZoomObject`" && link /nologo /DLL `"$objRoot\runtime.obj`" `"$objRoot\camera_write_adapter.obj`" `"$objRoot\profile_switch_model.obj`"$runtimeExtraObjects `"$runtimeDetourObject`" `"$runtimeConfigObject`" `"$runtimeCollisionObject`" `"$runtimeMouseLookObject`" `"$runtimeVirtualZoomObject`" `"$runtimeCameraObject`" `"$detoursLibrary`" bcrypt.lib user32.lib gdi32.lib /IMPLIB:`"$objRoot\gdtpc_runtime.lib`" /OUT:`"$dll`" /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
if ($effectiveGate -eq 0) {
    $runtimeCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common /c /I`"$detoursInclude`" `"$runtimeSource`" /Fo`"$runtimeObject`" && cl $common /c /I`"$detoursInclude`" `"$detourHookSource`" /Fo`"$runtimeDetourObject`" && cl $common /c `"$runtimeConfigSource`" /Fo`"$runtimeConfigObject`" && cl $common /c `"$runtimeCameraSource`" /Fo`"$runtimeCameraObject`" && cl $common /c `"$runtimeCollisionSource`" /Fo`"$runtimeCollisionObject`" && cl $common /c `"$runtimeMouseLookSource`" /Fo`"$runtimeMouseLookObject`" && cl $common /c `"$runtimeVirtualZoomSource`" /Fo`"$runtimeVirtualZoomObject`" && link /nologo /DLL `"$runtimeObject`" `"$runtimeDetourObject`" `"$runtimeConfigObject`" `"$runtimeCollisionObject`" `"$runtimeMouseLookObject`" `"$runtimeVirtualZoomObject`" `"$runtimeCameraObject`" `"$detoursLibrary`" bcrypt.lib user32.lib gdi32.lib /IMPLIB:`"$objRoot\gdtpc_runtime.lib`" /OUT:`"$dll`" /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
}
Invoke-BuildStep -Step 'Native runtime build' -Command $runtimeCommand

$harnessCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$harnessSource`" /Fo`"$harnessObject`" /Fe`"$harness`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Native harness build' -Command $harnessCommand

& $harness $dll $GameRoot
if ($LASTEXITCODE -ne 0) { throw "Native validation harness exited with code $LASTEXITCODE." }

$controllerSource = Join-Path $nativeRoot 'controller_model.cpp'
$controllerTestsSource = Join-Path $nativeRoot 'controller_model_tests.cpp'
$controllerCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$controllerSource`" `"$controllerTestsSource`" /Fo`"$objRoot\\`" /Fe`"$controllerTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Native controller tests build' -Command $controllerCommand
& $controllerTests
if ($LASTEXITCODE -ne 0) { throw "Native controller tests exited with code $LASTEXITCODE." }

$detourTestsSource = Join-Path $nativeRoot 'detour_hook_tests.cpp'
$detourCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common /MT /I`"$detoursInclude`" `"$detourHookSource`" `"$detourTestsSource`" /Fo`"$objRoot\\`" /Fe`"$detourTests`" `"$detoursLibrary`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Detour lifecycle tests build' -Command $detourCommand
& $detourTests
if ($LASTEXITCODE -ne 0) { throw "Detour lifecycle tests exited with code $LASTEXITCODE." }

$cameraMemorySource = Join-Path $nativeRoot 'camera_memory_model.cpp'
$cameraMemoryTestsSource = Join-Path $nativeRoot 'camera_memory_model_tests.cpp'
$cameraMemoryCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$cameraMemorySource`" `"$cameraMemoryTestsSource`" /Fo`"$objRoot\\`" /Fe`"$cameraMemoryTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Camera memory-model tests build' -Command $cameraMemoryCommand
& $cameraMemoryTests
if ($LASTEXITCODE -ne 0) { throw "Camera memory-model tests exited with code $LASTEXITCODE." }

$cameraWriteSource = Join-Path $nativeRoot 'camera_write_adapter.cpp'
$cameraWriteTestsSource = Join-Path $nativeRoot 'camera_write_adapter_tests.cpp'
$cameraWriteCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$cameraMemorySource`" `"$cameraWriteSource`" `"$cameraWriteTestsSource`" /Fo`"$objRoot\\`" /Fe`"$cameraWriteTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step "Gate $Gate camera write-adapter fault tests build" -Command $cameraWriteCommand
& $cameraWriteTests
if ($LASTEXITCODE -ne 0) { throw "Camera write-adapter fault tests exited with code $LASTEXITCODE." }

$profileSwitchSource = Join-Path $nativeRoot 'profile_switch_model.cpp'
$profileSwitchTestsSource = Join-Path $nativeRoot 'profile_switch_model_tests.cpp'
$profileSwitchCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$cameraMemorySource`" `"$cameraWriteSource`" `"$profileSwitchSource`" `"$profileSwitchTestsSource`" /Fo`"$objRoot\\`" /Fe`"$profileSwitchTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step "Gate $Gate profile-switch state/fault tests build" -Command $profileSwitchCommand
& $profileSwitchTests
if ($LASTEXITCODE -ne 0) { throw "Profile-switch state/fault tests exited with code $LASTEXITCODE." }

$runtimeConfigSource = Join-Path $nativeRoot 'runtime_config.cpp'
$runtimeConfigTestsSource = Join-Path $nativeRoot 'runtime_config_tests.cpp'
$runtimeConfigCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$cameraMemorySource`" `"$runtimeCollisionSource`" `"$runtimeMouseLookSource`" `"$runtimeVirtualZoomSource`" `"$runtimeConfigSource`" `"$runtimeConfigTestsSource`" /Fo`"$objRoot\\`" /Fe`"$runtimeConfigTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Runtime configuration tests build' -Command $runtimeConfigCommand
& $runtimeConfigTests
if ($LASTEXITCODE -ne 0) { throw "Runtime configuration tests exited with code $LASTEXITCODE." }

$aimMemorySource = Join-Path $nativeRoot 'aim_memory_model.cpp'
$aimMemoryTestsSource = Join-Path $nativeRoot 'aim_memory_model_tests.cpp'
$aimMemoryCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$aimMemorySource`" `"$aimMemoryTestsSource`" /Fo`"$objRoot\\`" /Fe`"$aimMemoryTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Aim memory-model tests build' -Command $aimMemoryCommand
& $aimMemoryTests
if ($LASTEXITCODE -ne 0) { throw "Aim memory-model tests exited with code $LASTEXITCODE." }

$collisionSource = Join-Path $nativeRoot 'camera_collision_model.cpp'
$collisionTestsSource = Join-Path $nativeRoot 'camera_collision_model_tests.cpp'
$collisionCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$collisionSource`" `"$collisionTestsSource`" /Fo`"$objRoot\\`" /Fe`"$collisionTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Camera collision spring-arm tests build' -Command $collisionCommand
& $collisionTests
if ($LASTEXITCODE -ne 0) { throw "Camera collision spring-arm tests exited with code $LASTEXITCODE." }

$uiProbeSource = Join-Path $nativeRoot 'ui_probe_model.cpp'
$uiProbeTestsSource = Join-Path $nativeRoot 'ui_probe_model_tests.cpp'
$uiProbeCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$uiProbeSource`" `"$uiProbeTestsSource`" /Fo`"$objRoot\\`" /Fe`"$uiProbeTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'UI probe capture tests build' -Command $uiProbeCommand
& $uiProbeTests
if ($LASTEXITCODE -ne 0) { throw "UI probe capture tests exited with code $LASTEXITCODE." }

$controllerEventTestsSource = Join-Path $nativeRoot 'controller_event_model_tests.cpp'
$controllerEventCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$controllerEventTestsSource`" /Fo`"$objRoot\controller_event_model_tests.obj`" /Fe`"$controllerEventTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Steam controller event observation tests build' -Command $controllerEventCommand
& $controllerEventTests
if ($LASTEXITCODE -ne 0) { throw "Steam controller event observation tests exited with code $LASTEXITCODE." }

$mouseLookTestsSource = Join-Path $nativeRoot 'mouse_look_model_tests.cpp'
$mouseLookCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$runtimeMouseLookSource`" `"$mouseLookTestsSource`" /Fo`"$objRoot\\`" /Fe`"$mouseLookTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Mouse-look model tests build' -Command $mouseLookCommand
& $mouseLookTests
if ($LASTEXITCODE -ne 0) { throw "Mouse-look model tests exited with code $LASTEXITCODE." }

$virtualZoomTestsSource = Join-Path $nativeRoot 'virtual_zoom_model_tests.cpp'
$virtualZoomCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$runtimeVirtualZoomSource`" `"$runtimeMouseLookSource`" `"$runtimeCollisionSource`" `"$virtualZoomTestsSource`" /Fo`"$objRoot\\`" /Fe`"$virtualZoomTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Virtual zoom model tests build' -Command $virtualZoomCommand
& $virtualZoomTests
if ($LASTEXITCODE -ne 0) { throw "Virtual zoom model tests exited with code $LASTEXITCODE." }

$levelQuerySource = Join-Path $nativeRoot 'level_query_adapter.cpp'
$levelQueryTestsSource = Join-Path $nativeRoot 'level_query_adapter_tests.cpp'
$levelQueryCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$collisionSource`" `"$levelQuerySource`" `"$levelQueryTestsSource`" /Fo`"$objRoot\\`" /Fe`"$levelQueryTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Level-query adapter fault tests build' -Command $levelQueryCommand
& $levelQueryTests
if ($LASTEXITCODE -ne 0) { throw "Level-query adapter fault tests exited with code $LASTEXITCODE." }

$lifecycleTestsSource = Join-Path $nativeRoot 'lifecycle_policy_tests.cpp'
$lifecycleTestsObject = Join-Path $objRoot 'lifecycle_policy_tests.obj'
$lifecycleCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$lifecycleTestsSource`" /Fo`"$lifecycleTestsObject`" /Fe`"$lifecycleTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Gate 0 lifecycle policy tests build' -Command $lifecycleCommand
& $lifecycleTests
if ($LASTEXITCODE -ne 0) { throw "Gate 0 lifecycle policy tests exited with code $LASTEXITCODE." }

$telemetryTestsSource = Join-Path $nativeRoot 'telemetry_tests.cpp'
$telemetryTestsObject = Join-Path $objRoot 'telemetry_tests.obj'
$telemetryCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$telemetryTestsSource`" /Fo`"$telemetryTestsObject`" /Fe`"$telemetryTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Telemetry ring and writer-drain fault tests build' -Command $telemetryCommand
& $telemetryTests
if ($LASTEXITCODE -ne 0) { throw "Telemetry ring and writer-drain fault tests exited with code $LASTEXITCODE." }

$runtimeHostTestsSource = Join-Path $nativeRoot 'runtime_host_tests.cpp'
$runtimeHostTestsObject = Join-Path $objRoot 'runtime_host_tests.obj'
$runtimeHostCommand = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && cl $common `"$runtimeHostTestsSource`" /Fo`"$runtimeHostTestsObject`" /Fe`"$runtimeHostTests`" /link /Brepro /DYNAMICBASE /NXCOMPAT /GUARD:CF"
Invoke-BuildStep -Step 'Runtime exported-lifecycle host tests build' -Command $runtimeHostCommand
& $runtimeHostTests $dll
if ($LASTEXITCODE -ne 0) { throw "Runtime exported-lifecycle host tests exited with code $LASTEXITCODE." }

$dotnet = Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'
if (-not (Test-Path -LiteralPath $dotnet)) { $dotnet = (Get-Command dotnet -ErrorAction Stop).Source }
$injectorProject = Join-Path $projectRoot 'src\GdTpc.Injector\GdTpc.Injector.csproj'
$injectorOut = Join-Path $artifactRoot 'injector'
Assert-GameClosed -Step 'injector publish'
& $dotnet publish $injectorProject -c Release -r win-x64 --self-contained false -o $injectorOut
if ($LASTEXITCODE -ne 0) { throw "Injector publish exited with code $LASTEXITCODE." }
& (Join-Path $injectorOut 'GdTpc.Injector.exe') --self-test
if ($LASTEXITCODE -ne 0) { throw "Injector lifecycle self-tests exited with code $LASTEXITCODE." }

# Stage the runtime configuration inside the release so the settings the live run uses are
# covered by the release hash manifest instead of being read from a mutable workspace file.
Assert-GameClosed -Step 'release staging'
$stagedConfig = Join-Path $artifactRoot 'runtime.ini'
Copy-Item -LiteralPath (Join-Path $projectRoot 'config\runtime.example.ini') -Destination $stagedConfig
if ($CameraCollision) {
    # The collision variant stages its capabilities ON, so a live session exercises what it is for.
    $collisionConfig = (Get-Content -Raw -LiteralPath $stagedConfig).Replace('collision_enabled=false', 'collision_enabled=true').Replace('zoom_step_enabled=false', 'zoom_step_enabled=true').Replace('shoulder_offset_enabled=false', 'shoulder_offset_enabled=true').Replace('mouse_look_enabled=false', 'mouse_look_enabled=true').Replace('virtual_zoom_enabled=false', 'virtual_zoom_enabled=true').Replace('third_person_far_plane_percent=100', 'third_person_far_plane_percent=95').Replace('npc_dialog_releases_cursor=false', 'npc_dialog_releases_cursor=true').Replace('right_stick_pitch_enabled=false', 'right_stick_pitch_enabled=true')
    if ($UiProbe) { $collisionConfig = $collisionConfig.Replace('ui_probe_enabled=false', 'ui_probe_enabled=true') }
    [System.IO.File]::WriteAllText($stagedConfig, $collisionConfig, [System.Text.UTF8Encoding]::new($false))
}
& $runtimeConfigTests $stagedConfig
if ($LASTEXITCODE -ne 0) { throw "Staged $releaseRootName configuration was rejected by the strict parser." }
# The launcher's pre-injection settings check uses the runtime export through the injector: prove it accepts the staged file
# and rejects a broken one.
& (Join-Path $injectorOut 'GdTpc.Injector.exe') --check-config $dll $stagedConfig
if ($LASTEXITCODE -ne 0) { throw 'Injector --check-config rejected the staged configuration.' }
$brokenConfig = Join-Path $objRoot 'broken-runtime.ini'
[System.IO.File]::WriteAllText($brokenConfig, "[general]`nschema_version=1`n", [System.Text.UTF8Encoding]::new($false))
# Windows PowerShell 5.1 turns redirected native error output into a terminating error under 'Stop'.
$ErrorActionPreference = 'Continue'
& (Join-Path $injectorOut 'GdTpc.Injector.exe') --check-config $dll $brokenConfig 2>$null
$brokenExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($brokenExit -ne 23) { throw "Injector --check-config did not reject an incomplete configuration (exit $brokenExit)." }
$global:LASTEXITCODE = 0
Write-Output 'PASS: injector --check-config accepts the staged settings and rejects an incomplete file.'

$manifestFile = Join-Path $artifactRoot 'release-manifest.json'
$writesEnabled = if ($effectiveGate -gt 0) { 1 } else { 0 }
$manifest = [ordered]@{ schemaVersion = 1; gate = $effectiveGate; variant = $releaseVariant; createdUtc = [DateTimeOffset]::UtcNow.ToString('O'); gameStateWritesEnabled = $writesEnabled; files = @() }
$manifest.files = @(Get-ChildItem -File -Recurse $artifactRoot | ForEach-Object {
    [ordered]@{ path = (Get-ReleaseRelativePath -Root $artifactRoot -FullName $_.FullName); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestFile -Encoding utf8

# Verify the manifest just written describes the release exactly, so a run script that refuses an
# unverifiable release is proven against a good release as well as a tampered one.
& (Join-Path $PSScriptRoot 'Test-ReleaseManifest.ps1') -ReleasePath $artifactRoot -ExpectedGate $effectiveGate -ExpectedVariant $releaseVariant -ExpectedGameStateWritesEnabled $writesEnabled
Write-Output "PASS: immutable $releaseRootName release built, tested and hash-verified at $artifactRoot"
