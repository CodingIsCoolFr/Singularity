<#
    Builds Singularity.

    Two folders come out of this, and keeping them apart is the point:

        build\   everything the compiler needs and nobody else ever opens
        dist\    the program, and only the program

    They used to be one folder, which meant the exe sat among forty odd
    CMake caches, ninja logs and object directories, and finding the thing
    you actually run meant reading every name.

    Usage:
        .\build.ps1              # release build
        .\build.ps1 -Debug       # debug build
        .\build.ps1 -Clean       # wipe both folders first
        .\build.ps1 -Installer   # also build the setup program
#>
param(
    [switch]$Debug,
    [switch]$Clean,
    [switch]$Installer
)

$ErrorActionPreference = 'Stop'

$root       = $PSScriptRoot
$qtRoot     = 'C:\Qt\6.10.3\msvc2022_64'
$vcvars     = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$ninjaDir   = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$config     = if ($Debug) { 'Debug' } else { 'Release' }
$buildDir   = Join-Path $root "build\$config"
$distDir    = Join-Path $root 'dist'

if (-not (Test-Path $qtRoot))  { throw "Qt not found at $qtRoot" }
if (-not (Test-Path $vcvars))  { throw "Visual Studio build tools not found at $vcvars" }

if ($Clean) {
    foreach ($dir in @($buildDir, $distDir)) {
        if (Test-Path $dir) {
            Write-Host "Cleaning $dir"
            Remove-Item -Recurse -Force $dir
        }
    }
}

# Everything runs inside one cmd session so the MSVC environment survives.
#
# --no-compiler-runtime leaves out vc_redist.x64.exe. That is a twenty five
# megabyte installer, not something the program loads, and it was the largest
# file in the folder by a wide margin.
$script = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1

rem Visual Studio ships Ninja but does not always put it on PATH.
set "PATH=$ninjaDir;%PATH%"

cmake -S "$root" -B "$buildDir" -G Ninja -DCMAKE_BUILD_TYPE=$config -DCMAKE_PREFIX_PATH="$qtRoot" -DSINGULARITY_DIST_DIR="$distDir"
if errorlevel 1 exit /b 1

cmake --build "$buildDir"
if errorlevel 1 (
  rem LNK1201: the linker could not write the debug database. The project is
  rem built with /Z7 so nothing should be holding it, but a stray mspdbsrv from
  rem an older build, or a debugger, still can. Stopping that and deleting the
  rem file recovers it. This costs nothing when the real failure was a compile
  rem error, because the second pass then stops in the same place.
  echo Build failed. Clearing the debug database and trying once more.
  taskkill /f /im mspdbsrv.exe >nul 2>nul
  del /q "$buildDir\$config\Singularity.pdb" >nul 2>nul
  cmake --build "$buildDir"
  if errorlevel 1 exit /b 1
)

"$qtRoot\bin\windeployqt.exe" --$($config.ToLower()) --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime --plugindir "$distDir\plugins" "$distDir\Singularity.exe"
if errorlevel 1 exit /b 1
"@

$scriptPath = Join-Path $env:TEMP 'singularity-build.cmd'
Set-Content -Path $scriptPath -Value $script -Encoding ASCII

& cmd.exe /c $scriptPath
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

# Import libraries and the generated resource script land beside the exe
# because the linker puts them there. They are build leftovers, not runtime,
# so they do not belong in what ships.
Get-ChildItem $distDir -File -Include *.exp, *.lib, *.rc -Recurse -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

# Qt looks for its plugins in eight folders beside the exe unless it is told
# otherwise, and those eight were most of the clutter. This points it at one.
# It has to be written after windeployqt, which writes its own qt.conf.
@'
[Paths]
Plugins = plugins
'@ | Set-Content -Path (Join-Path $distDir 'qt.conf') -Encoding ASCII

$count = (Get-ChildItem $distDir -ErrorAction SilentlyContinue | Measure-Object).Count

Write-Host ""
Write-Host "Built: $distDir\Singularity.exe" -ForegroundColor Green
Write-Host "       $count items in dist, build leftovers stayed in build\" -ForegroundColor DarkGray

if ($Installer) {
    # Inno Setup installs per user by default, so the compiler is under the
    # local profile rather than Program Files.
    $iscc = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $iscc) {
        throw "Inno Setup not found. Install it with: winget install JRSoftware.InnoSetup"
    }

    & $iscc (Join-Path $root 'installer\Singularity.iss') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Installer failed with exit code $LASTEXITCODE" }

    $setup = Get-ChildItem (Join-Path $root 'dist-installer') -Filter '*-setup.exe' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1

    Write-Host ""
    Write-Host "Installer: $($setup.FullName)" -ForegroundColor Green
    Write-Host ("           {0:N0} MB" -f ($setup.Length / 1MB)) -ForegroundColor DarkGray
}
