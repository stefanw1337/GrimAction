[CmdletBinding()]
param(
    # An immutable, live-accepted camera-collision release (artifacts\releases\collision\<id>).
    [Parameter(Mandatory = $true)][string]$ReleasePath,
    # Player-facing version, e.g. 0.1.0.
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$')][string]$Version
)

# Builds the player download: the release's runtime DLL and staged settings, a self-contained single-file injector (no .NET
# install needed), the launcher scripts, the player guide and notices, all hashed into bin\package-manifest.json and zipped.
# The injector is published from the current source, so build the package from the same source tree as the release.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

function Assert-GameClosed([string]$Step) {
    if (Get-Process -Name 'Grim Dawn' -ErrorAction SilentlyContinue) {
        throw "Grim Dawn is running. Package builds are refused ($Step)."
    }
}
function Get-RelativePath([string]$Root, [string]$FullName) {
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd([char]92)
    $fileFull = [System.IO.Path]::GetFullPath($FullName)
    if (-not $fileFull.StartsWith($rootFull + [char]92, [StringComparison]::OrdinalIgnoreCase)) { throw "Path is outside the package: $FullName" }
    return $fileFull.Substring($rootFull.Length + 1)
}

Assert-GameClosed 'start'
$release = [System.IO.Path]::GetFullPath($ReleasePath)
$releaseId = Split-Path -Leaf $release
& (Join-Path $PSScriptRoot 'Test-ReleaseManifest.ps1') -ReleasePath $release -ExpectedGate 2 -ExpectedVariant 'camera-collision' -ExpectedGameStateWritesEnabled 1
& (Join-Path $PSScriptRoot 'Test-ReleaseNotProhibited.ps1') -ReleasePath $release

$packageName = "GrimAction-$Version"
$packagesRoot = Join-Path $projectRoot 'artifacts\packages'
$stage = Join-Path $packagesRoot $packageName
$zip = Join-Path $packagesRoot "$packageName.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) { throw "Package $packageName already exists; packages are immutable. Use a new -Version." }

$dotnet = Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'
if (-not (Test-Path -LiteralPath $dotnet)) { $dotnet = (Get-Command dotnet -ErrorAction Stop).Source }
$publishOut = Join-Path $projectRoot "artifacts\build-intermediates\package\$packageName\injector"
Assert-GameClosed 'injector publish'
& $dotnet publish (Join-Path $projectRoot 'src\GdTpc.Injector\GdTpc.Injector.csproj') -c Release -r win-x64 --self-contained true `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=none -o $publishOut
if ($LASTEXITCODE -ne 0) { throw "Injector publish exited with code $LASTEXITCODE." }
$publishedInjector = Join-Path $publishOut 'GdTpc.Injector.exe'
& $publishedInjector --self-test
if ($LASTEXITCODE -ne 0) { throw "Self-contained injector self-tests exited with code $LASTEXITCODE." }

Assert-GameClosed 'staging'
$bin = Join-Path $stage 'bin'
$settingsDir = Join-Path $stage 'settings'
New-Item -ItemType Directory -Force -Path $bin, $settingsDir, (Join-Path $stage 'logs') | Out-Null
Copy-Item -LiteralPath $publishedInjector -Destination $bin
Copy-Item -LiteralPath (Join-Path $release 'gdtpc_runtime_collision.dll') -Destination $bin
Copy-Item -LiteralPath (Join-Path $projectRoot 'package\Start-GrimAction.ps1') -Destination $bin
Copy-Item -LiteralPath (Join-Path $projectRoot 'package\Stop-GrimAction.ps1') -Destination $bin
Copy-Item -LiteralPath (Join-Path $projectRoot 'package\Start GrimAction.cmd') -Destination $stage
Copy-Item -LiteralPath (Join-Path $projectRoot 'package\Stop GrimAction.cmd') -Destination $stage
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\PLAYER_GUIDE.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\CURSOR_TEST.md') -Destination (Join-Path $stage 'TEST-MUSEPEKER.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\ANTIVIRUS.md') -Destination (Join-Path $stage 'ANTIVIRUS.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD-PARTY-NOTICES.txt') -Destination $stage
# Settings are the release's live-accepted file with a short header; the player may edit them, so they are not hashed.
$utf8 = [System.Text.UTF8Encoding]::new($false)
$settingsHeader = "; GrimAction settings. Edit values, save, then run Start GrimAction. Every key must stay present.`r`n; Ranges and meanings: README.md, section Settings. A copy of the original is in settings\runtime.default.ini.`r`n"
$releaseSettings = [System.IO.File]::ReadAllText((Join-Path $release 'runtime.ini'))
[System.IO.File]::WriteAllText((Join-Path $settingsDir 'runtime.ini'), $settingsHeader + $releaseSettings, $utf8)
[System.IO.File]::WriteAllText((Join-Path $settingsDir 'runtime.default.ini'), $settingsHeader + $releaseSettings, $utf8)
$gamePathText = "# Only needed if GrimAction cannot find Grim Dawn by itself.`r`n# Put your Grim Dawn folder (the one that contains x64\Grim Dawn.exe) on the line below, e.g.`r`n# D:\SteamLibrary\steamapps\common\Grim Dawn`r`n"
[System.IO.File]::WriteAllText((Join-Path $settingsDir 'game-path.txt'), $gamePathText, $utf8)
[System.IO.File]::WriteAllText((Join-Path $stage 'logs\README.txt'), "Session logs (CSV camera telemetry) are written here. They stay on your PC; delete them any time.`r`n", $utf8)

foreach ($ini in 'runtime.ini', 'runtime.default.ini') {
    & (Join-Path $bin 'GdTpc.Injector.exe') --check-config (Join-Path $bin 'gdtpc_runtime_collision.dll') (Join-Path $settingsDir $ini)
    if ($LASTEXITCODE -ne 0) { throw "Packaged $ini was rejected by --check-config." }
}

# Everything except the editable settings and logs is integrity-checked by the launcher before each start.
$hashed = @(Get-ChildItem -File -Recurse -LiteralPath $stage | Where-Object {
    $relative = Get-RelativePath $stage $_.FullName
    -not $relative.StartsWith('settings\') -and -not $relative.StartsWith('logs\') -and $relative -ne 'bin\package-manifest.json'
})
$manifest = [ordered]@{
    schemaVersion = 1
    product = 'GrimAction'
    version = $Version
    runtimeRelease = $releaseId
    supportedGame = 'Grim Dawn Steam x64, build listed in config/supported-builds.json'
    createdUtc = [DateTimeOffset]::UtcNow.ToString('O')
    files = @($hashed | ForEach-Object { [ordered]@{ path = (Get-RelativePath $stage $_.FullName); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
}
[System.IO.File]::WriteAllText((Join-Path $bin 'package-manifest.json'), ($manifest | ConvertTo-Json -Depth 5), $utf8)

Assert-GameClosed 'zip'
Compress-Archive -Path $stage -DestinationPath $zip
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$zip.sha256", "$zipHash  $packageName.zip`r`n", $utf8)
Write-Output "PASS: $packageName built from release $releaseId"
Write-Output "  folder: $stage"
Write-Output "  zip   : $zip"
Write-Output "  sha256: $zipHash"
