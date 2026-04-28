param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildDir = $null,
    [string]$NinjaPath = 'C:\ninja-win\ninja.exe',
    [string]$CMakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
)

$ErrorActionPreference = 'Stop'

if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot 'libs\JUCE\build-host-juceaide-clean'
}

. (Join-Path $PSScriptRoot 'Set-VSBuildEnv.ps1')

if (-not (Test-Path -LiteralPath $CMakeExe)) {
    $CMakeExe = (Get-Command cmake.exe -ErrorAction Stop).Source
}
$juceaideSource = Join-Path $RepoRoot 'libs\JUCE'
$pathFile = Join-Path $BuildDir 'juceaide-path.txt'

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Write-Host "Configuring native juceaide in $BuildDir"
& $cmakeExe -S $juceaideSource -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$NinjaPath" -DJUCE_BUILD_HELPER_TOOLS=ON -DJUCE_BUILD_EXTRAS=OFF -DJUCE_BUILD_EXAMPLES=OFF
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Building native juceaide"
& $cmakeExe --build $BuildDir --config Release --target juceaide --parallel 4
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$juceaideExe = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter juceaide.exe | Select-Object -First 1
if (-not $juceaideExe) {
    throw "Could not locate juceaide.exe under $BuildDir"
}

Set-Content -LiteralPath $pathFile -Value $juceaideExe.FullName
Write-Host "juceaide path: $($juceaideExe.FullName)"
