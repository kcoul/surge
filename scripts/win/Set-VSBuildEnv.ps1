param(
    [string]$VsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
    [string]$NinjaPath = 'C:\ninja-win\ninja.exe'
)

function Import-BatchEnvironment
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$BatchPath
    )

    if (-not (Test-Path -LiteralPath $BatchPath))
    {
        throw "Batch file not found: $BatchPath"
    }

    $output = & cmd /c "call `"$BatchPath`" >nul && set"

    foreach ($line in $output)
    {
        if ($line -match '^(.*?)=(.*)$')
        {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
        }
    }
}

Import-BatchEnvironment -BatchPath $VsDevCmd

if (-not $env:PATH_ORIG)
{
    $env:PATH_ORIG = $env:PATH
}

if ($NinjaPath -and (Test-Path -LiteralPath $NinjaPath))
{
    $env:CMAKE_MAKE_PROGRAM = $NinjaPath
}

$env:MAKEFLAGS = ''

Write-Host "Loaded Visual Studio build environment."
Write-Host "  CMAKE_MAKE_PROGRAM=$($env:CMAKE_MAKE_PROGRAM)"
Write-Host "  PATH_ORIG set: $([bool]$env:PATH_ORIG)"
