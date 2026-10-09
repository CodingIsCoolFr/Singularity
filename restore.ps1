<#
    Fetches and builds everything under third_party\.

    That folder is four gigabytes of somebody else's code, so it is not in the
    repository. Restoring it used to mean following four separate sets of
    instructions spread through the README, which is fine until the folder is
    lost and you find out how many of those steps you remembered wrong.

    Safe to re-run. Anything already in place is left alone, so this can be
    used to add one missing piece without rebuilding the rest.

    Usage:
        .\restore.ps1
        .\restore.ps1 -SkipDave     # everything except the eight minute one
#>
param(
    [switch]$SkipDave
)

$ErrorActionPreference = 'Stop'

$root     = $PSScriptRoot
$third    = Join-Path $root 'third_party'
# Overridable for the cloud build, as in build.ps1.
$vcvars   = if ($env:SINGULARITY_VCVARS) { $env:SINGULARITY_VCVARS } else { 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat' }
$ninjaDir = if ($env:SINGULARITY_NINJA)  { $env:SINGULARITY_NINJA }  else { 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja' }

New-Item -ItemType Directory -Force -Path $third | Out-Null

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }

# Runs a batch of commands inside one MSVC environment.
#
# `if errorlevel 1` rather than `||`. vcvars prints a complaint about a
# missing vswhere on some machines and still sets the environment correctly;
# `||` treats that as failure and stops before anything is built.
function RunInMsvc($name, $body) {
    $script = "@echo off`r`ncall `"$vcvars`" >nul`r`nset `"PATH=$ninjaDir;%PATH%`"`r`n$body"
    $path = Join-Path $env:TEMP "singularity-$name.cmd"
    Set-Content -Path $path -Value $script -Encoding ASCII
    & cmd.exe /c $path
    if ($LASTEXITCODE -ne 0) { throw "$name failed with exit code $LASTEXITCODE" }
}

# ---------------------------------------------------------------------------
# FFmpeg. Decodes other people's cameras and screens.
#
# The LGPL build on purpose: linked dynamically, so nothing here imposes the
# GPL on this project. Qt ships its own FFmpeg with no headers and a version
# that moves whenever Qt does, which is why this one is separate.
# ---------------------------------------------------------------------------
if (Test-Path "$third\ffmpeg\lib\avcodec.lib") {
    Step "FFmpeg already present"
} else {
    Step "FFmpeg"
    $url = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip'
    $zip = Join-Path $third 'ffmpeg.zip'
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath "$third\ffmpeg-tmp" -Force
    Move-Item (Get-ChildItem "$third\ffmpeg-tmp" -Directory | Select-Object -First 1).FullName "$third\ffmpeg"
    Remove-Item "$third\ffmpeg-tmp" -Recurse -Force
    Remove-Item $zip -Force
}

# ---------------------------------------------------------------------------
# libsodium. Encrypts each packet of sound.
#
# The official prebuilt MSVC archive rather than a source build. The static
# library wants SODIUM_STATIC defined, which CMakeLists does.
# ---------------------------------------------------------------------------
if (Test-Path "$third\libsodium\x64\Release\v143\static\libsodium.lib") {
    Step "libsodium already present"
} else {
    Step "libsodium"
    $zip = Join-Path $third 'libsodium.zip'
    Invoke-WebRequest -Uri 'https://download.libsodium.org/libsodium/releases/libsodium-1.0.20-stable-msvc.zip' `
                      -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $third -Force
    Remove-Item $zip -Force
}

# ---------------------------------------------------------------------------
# Opus. Compresses the sound itself.
#
# Built from source, because the project publishes no Windows binaries. This
# is the one piece here that is compiled rather than downloaded, and it is
# quick.
# ---------------------------------------------------------------------------
if (Test-Path "$third\opus-install\lib\opus.lib") {
    Step "Opus already present"
} else {
    Step "Opus"
    $version = '1.5.2'
    $src     = Join-Path $third "opus-$version"

    if (-not (Test-Path $src)) {
        $tar = Join-Path $third 'opus.tar.gz'
        Invoke-WebRequest -Uri "https://downloads.xiph.org/releases/opus/opus-$version.tar.gz" `
                          -OutFile $tar -UseBasicParsing
        tar -xzf $tar -C $third
        Remove-Item $tar -Force
    }

    RunInMsvc 'opus' @"
cmake -S "$src" -B "$third\opus-build" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="$third\opus-install" -DOPUS_BUILD_SHARED_LIBRARY=OFF ^
  -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
if errorlevel 1 exit /b 1
cmake --build "$third\opus-build" --target install
if errorlevel 1 exit /b 1
"@
}

# ---------------------------------------------------------------------------
# libdave. Discord's end-to-end encryption, required on every call since
# March 2026. Without it no voice channel can be joined at all.
#
# The slow one, and nearly all of that is OpenSSL. The -md triplet matters:
# libdave's own default builds against the static C runtime, which cannot be
# linked with Qt.
# ---------------------------------------------------------------------------
if ($SkipDave) {
    Step "Skipping libdave, so voice will not work"
} elseif (Test-Path "$third\libdave\cpp\build\libdave.lib") {
    Step "libdave already present"
} else {
    Step "libdave, this takes about eight minutes"

    if (-not (Test-Path "$third\libdave")) {
        git clone -q --recurse-submodules https://github.com/discord/libdave.git "$third\libdave"
    }

    $cpp = "$third\libdave\cpp"
    RunInMsvc 'dave' @"
cd /d "$cpp"
if not exist "vcpkg\vcpkg.exe" call "vcpkg\bootstrap-vcpkg.bat"
if not exist "vcpkg\vcpkg.exe" exit /b 1
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DVCPKG_MANIFEST_DIR=vcpkg-alts/openssl_3 ^
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DBUILD_SHARED_LIBS=OFF -DTESTING=OFF ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -DVCPKG_TARGET_ARCHITECTURE=x86_64 ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
if errorlevel 1 exit /b 1
cmake --build build --target libdave
if errorlevel 1 exit /b 1
"@
}

Write-Host "`nthird_party restored:" -ForegroundColor Green
foreach ($check in @(
    @{ n = 'FFmpeg   '; p = "$third\ffmpeg\lib\avcodec.lib" },
    @{ n = 'libsodium'; p = "$third\libsodium\x64\Release\v143\static\libsodium.lib" },
    @{ n = 'Opus     '; p = "$third\opus-install\lib\opus.lib" },
    @{ n = 'libdave  '; p = "$third\libdave\cpp\build\libdave.lib" })) {
    $ok = Test-Path $check.p
    Write-Host ("  {0}  {1}" -f $check.n, $(if ($ok) { 'yes' } else { 'MISSING' })) `
        -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}
