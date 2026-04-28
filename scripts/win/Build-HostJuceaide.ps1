param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildDir = $null,
    [string]$NinjaPath = 'C:\ninja-win\ninja.exe',
    [string]$CMakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    [ValidateSet('Ninja', 'NMake')]
    [string]$Generator = 'Ninja'
)

$ErrorActionPreference = 'Stop'

if (-not $BuildDir) {
    $defaultDirName = if ($Generator -eq 'NMake') { 'build-host-juceaide-nmake' } else { 'build-host-juceaide-clean' }
    $BuildDir = Join-Path $RepoRoot "libs\JUCE\$defaultDirName"
}

if (-not (Test-Path -LiteralPath $CMakeExe)) {
    $CMakeExe = (Get-Command cmake.exe -ErrorAction Stop).Source
}
$juceaideSource = Join-Path $RepoRoot 'libs\JUCE'
$hostEnvWrapper = Join-Path $RepoRoot 'libs\JUCE\tools\run_with_vcvars.bat'
$pathFile = Join-Path $BuildDir 'juceaide-path.txt'
$configureArgs = @(
    '-S', $juceaideSource,
    '-B', $BuildDir,
    '-G', $Generator,
    '-DCMAKE_BUILD_TYPE=Release',
    '-DJUCE_BUILD_HELPER_TOOLS=ON',
    '-DJUCE_BUILD_EXTRAS=OFF',
    '-DJUCE_BUILD_EXAMPLES=OFF'
)

if ($Generator -eq 'Ninja') {
    $configureArgs += "-DCMAKE_MAKE_PROGRAM=$NinjaPath"
} else {
    Remove-Item Env:CMAKE_MAKE_PROGRAM -ErrorAction SilentlyContinue
}

$buildArgs = @('--build', $BuildDir, '--config', 'Release', '--target', 'juceaide')
if ($Generator -eq 'Ninja') {
    $buildArgs += @('--parallel', '4')
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Write-Host "Configuring native juceaide in $BuildDir with $Generator"
& $hostEnvWrapper $CMakeExe @configureArgs
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Building native juceaide with $Generator"
& $hostEnvWrapper $CMakeExe @buildArgs
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$juceaideExe = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter juceaide.exe | Select-Object -First 1
if (-not $juceaideExe) {
    throw "Could not locate juceaide.exe under $BuildDir"
}

Set-Content -LiteralPath $pathFile -Value $juceaideExe.FullName
Write-Host "juceaide path: $($juceaideExe.FullName)"
