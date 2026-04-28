param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildDir = $null,
    [string]$HostBuildDir = $null,
    [string]$LogFile = $null,
    [string]$ToolchainFile = $null,
    [string]$NinjaPath = 'C:\ninja-win\ninja.exe',
    [string]$CMakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    [ValidateSet('Ninja', 'NMake')]
    [string]$HostGenerator = 'Ninja',
    [ValidateSet('Ninja', 'NMake')]
    [string]$TargetGenerator = 'Ninja',
    [switch]$ForcePlatformFilesystem,
    [int]$HeartbeatSeconds = 60
)

$ErrorActionPreference = 'Stop'
$HeartbeatSeconds = 60

if (-not [System.IO.Path]::IsPathRooted($RepoRoot)) {
    $RepoRoot = (Resolve-Path $RepoRoot).Path
} else {
    $RepoRoot = (Resolve-Path $RepoRoot).Path
}

if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot 'build'
}

if (-not $HostBuildDir) {
    $HostBuildDir = Join-Path $RepoRoot 'libs\JUCE\build'
}

if (-not $LogFile) {
    $LogFile = Join-Path $RepoRoot 'build-qnx-surge.log'
}

if (-not $ToolchainFile) {
    $ToolchainFile = Join-Path $RepoRoot 'libs\JUCE\extras\Build\CMake\QNXAarch64Toolchain.cmake'
}

function Write-LogLine {
    param([string]$Message)
    Add-Content -LiteralPath $LogFile -Value ("[{0}] {1}" -f (Get-Date -Format 'ddd MM/dd/yyyy HH:mm:ss.fff'), $Message)
}

function Invoke-TimedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Phase,
        [Parameter(Mandatory = $true)]
        [string[]]$CommandArgs,
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory
    )

    Write-LogLine "starting $Phase"
    $phaseStdOut = Join-Path $env:TEMP ("surge-qnx-{0}.stdout.log" -f $Phase)
    $phaseStdErr = Join-Path $env:TEMP ("surge-qnx-{0}.stderr.log" -f $Phase)
    Remove-Item -LiteralPath $phaseStdOut, $phaseStdErr -ErrorAction SilentlyContinue

    try {
        Write-LogLine "$Phase stdout: $phaseStdOut"
        Write-LogLine "$Phase stderr: $phaseStdErr"
        Write-LogLine "$Phase executable: $Executable"
        $proc = Start-Process -FilePath $Executable -ArgumentList $CommandArgs -WorkingDirectory $WorkingDirectory -PassThru -NoNewWindow -RedirectStandardOutput $phaseStdOut -RedirectStandardError $phaseStdErr
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        Write-LogLine "$Phase pid: $($proc.Id)"

        while (-not $proc.HasExited) {
            Start-Sleep -Seconds $HeartbeatSeconds
            if (-not $proc.HasExited) {
                Write-LogLine "$Phase still running after $([math]::Round($timer.Elapsed.TotalMinutes, 1)) min (pid $($proc.Id))"
            }
        }

        $timer.Stop()
        $proc.WaitForExit()
        Write-LogLine "$Phase finished with exit code $($proc.ExitCode) after $([math]::Round($timer.Elapsed.TotalMinutes, 1)) min"
        Write-LogLine "$Phase stdout captured at $phaseStdOut"
        Write-LogLine "$Phase stderr captured at $phaseStdErr"
        return $proc.ExitCode
    }
    catch {
        Write-LogLine "$Phase failed: $($_.Exception.ToString())"
        throw
    }
}

try {
    Set-Content -LiteralPath $LogFile -Value ("[{0}] starting timed QNX build" -f (Get-Date -Format 'ddd MM/dd/yyyy HH:mm:ss.fff'))
    Write-LogLine "repo root: $RepoRoot"
    Write-LogLine "host generator: $HostGenerator"
    Write-LogLine "target generator: $TargetGenerator"
    Write-LogLine "host build dir: $HostBuildDir"
    Write-LogLine "build dir: $BuildDir"
    Write-LogLine "toolchain: $ToolchainFile"
    Write-LogLine "ninja: $NinjaPath"
    $runQnxBuildCmd = Join-Path $PSScriptRoot 'Run-QNXBuild.cmd'
    Write-LogLine "qnx wrapper: $runQnxBuildCmd"
    Write-LogLine "building native juceaide first"
    $hostJuceaideScript = Join-Path $PSScriptRoot 'Build-HostJuceaide.ps1'
    $hostJuceaideArgs = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', $hostJuceaideScript,
        '-RepoRoot', $RepoRoot,
        '-BuildDir', $HostBuildDir,
        '-Generator', $HostGenerator,
        '-CMakeExe', $CMakeExe
    )
    if ($HostGenerator -eq 'Ninja') {
        $hostJuceaideArgs += @('-NinjaPath', $NinjaPath)
    }

    $hostJuceaideExit = Invoke-TimedCommand -Phase 'host-juceaide' -Executable 'powershell.exe' -CommandArgs $hostJuceaideArgs -WorkingDirectory $RepoRoot
    if ($hostJuceaideExit -ne 0) {
        throw "Host juceaide build failed with exit code $hostJuceaideExit"
    }

    $hostJuceaidePathFile = Join-Path $HostBuildDir 'juceaide-path.txt'
    if (-not (Test-Path -LiteralPath $hostJuceaidePathFile)) {
        throw "Host juceaide path file not found: $hostJuceaidePathFile"
    }

    $hostJuceaidePath = (Get-Content -LiteralPath $hostJuceaidePathFile -ErrorAction Stop | Select-Object -First 1).Trim()
    if (-not (Test-Path -LiteralPath $hostJuceaidePath)) {
        throw "Host juceaide executable not found: $hostJuceaidePath"
    }

    Write-LogLine "native juceaide: $hostJuceaidePath"
    Write-LogLine "resolving cmake.exe"
    if (-not (Test-Path -LiteralPath $CMakeExe)) {
        $CMakeExe = (Get-Command cmake.exe -ErrorAction Stop).Source
    }
    Write-LogLine "cmake.exe resolved to $CMakeExe"
    if ($TargetGenerator -eq 'Ninja') {
        $env:CMAKE_MAKE_PROGRAM = $NinjaPath
        Write-LogLine "CMAKE_MAKE_PROGRAM set to $NinjaPath for $TargetGenerator target generator"
    } else {
        Remove-Item Env:CMAKE_MAKE_PROGRAM -ErrorAction SilentlyContinue
        Write-LogLine "CMAKE_MAKE_PROGRAM cleared for $TargetGenerator target generator"
    }

    $configureArgs = @(
        '-S', $RepoRoot,
        '-B', $BuildDir,
        '-G', $TargetGenerator,
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile",
        "-DJUCE_JUCEAIDE_PATH=$hostJuceaidePath",
        '-DSURGE_BUILD_FX=OFF',
        '-DSURGE_BUILD_TESTRUNNER=OFF',
        '-DSURGE_BUILD_CLAP=OFF',
        '-DSURGE_BUILD_LV2=OFF',
        '-DSURGE_SKIP_LUA=TRUE'
    )

    if ($TargetGenerator -eq 'Ninja') {
        $configureArgs += '-DCMAKE_MAKE_PROGRAM=C:/ninja-win/ninja.exe'
    }

    if ($ForcePlatformFilesystem) {
        $configureArgs += '-DSST_PLUGININFRA_FILESYSTEM_FORCE_PLATFORM=ON'
    }

    $buildArgs = @(
        '--build', $BuildDir,
        '--config', 'Release',
        '--target', 'surge-xt_Standalone'
    )

    if ($TargetGenerator -eq 'Ninja') {
        $buildArgs += @('--parallel', '4')
    }

    Write-LogLine "configure phase uses native juceaide import"
    $configureCommandArgs = @('/c', $runQnxBuildCmd, $CMakeExe) + $configureArgs
    $configureExit = Invoke-TimedCommand -Phase 'configure' -Executable 'cmd.exe' -CommandArgs $configureCommandArgs -WorkingDirectory $RepoRoot
    if ($configureExit -ne 0) {
        exit $configureExit
    }

    $buildCommandArgs = @('/c', $runQnxBuildCmd, $CMakeExe) + $buildArgs
    $buildExit = Invoke-TimedCommand -Phase 'build' -Executable 'cmd.exe' -CommandArgs $buildCommandArgs -WorkingDirectory $RepoRoot
    exit $buildExit
}
catch {
    Write-LogLine "top-level failure: $($_.Exception.ToString())"
    exit 1
}
