<#
    Builds Singularity and copies the Qt runtime next to the exe.

    Usage:
        .\build.ps1              # release build
        .\build.ps1 -Debug       # debug build
        .\build.ps1 -Clean       # wipe the build folder first
#>
param(
    [switch]$Debug,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$root       = $PSScriptRoot
$qtRoot     = 'C:\Qt\6.10.3\msvc2022_64'
$vcvars     = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$ninjaDir   = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$config     = if ($Debug) { 'Debug' } else { 'Release' }
$buildDir   = Join-Path $root "build\$config"

if (-not (Test-Path $qtRoot))  { throw "Qt not found at $qtRoot" }
if (-not (Test-Path $vcvars))  { throw "Visual Studio build tools not found at $vcvars" }

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

# Everything runs inside one cmd session so the MSVC environment survives.
$script = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1

rem Visual Studio ships Ninja but does not always put it on PATH.
set "PATH=$ninjaDir;%PATH%"

cmake -S "$root" -B "$buildDir" -G Ninja -DCMAKE_BUILD_TYPE=$config -DCMAKE_PREFIX_PATH="$qtRoot"
if errorlevel 1 exit /b 1

cmake --build "$buildDir"
if errorlevel 1 exit /b 1

"$qtRoot\bin\windeployqt.exe" --$($config.ToLower()) --no-translations --no-system-d3d-compiler --no-opengl-sw "$buildDir\Singularity.exe"
if errorlevel 1 exit /b 1
"@

$scriptPath = Join-Path $env:TEMP 'singularity-build.cmd'
Set-Content -Path $scriptPath -Value $script -Encoding ASCII

& cmd.exe /c $scriptPath
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

Write-Host ""
Write-Host "Built: $buildDir\Singularity.exe" -ForegroundColor Green
