<#
    Builds a release and publishes it to both repositories.

    There are two, and they have to agree. The private one here is where the
    source and the tags live; the public singularity-updates is what the
    program itself reads, because a private repository answers 404 to anyone
    without a token and an updater pointed at it would need a token shipped
    inside the binary.

    Publishing to both by hand is how they drifted the first time: 0.1.3 went
    to the channel, the source repository stopped at 0.1.2, and its releases
    page said "Latest" about something that was not.

    The version is read from CMakeLists.txt rather than passed in, so there is
    no way to tag one number and ship another.

    Usage:
        .\release.ps1
        .\release.ps1 -DryRun     # build and check, publish nothing
#>
param(
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$root    = $PSScriptRoot
$source  = 'CodingIsCoolFr/Singularity'
$channel = 'CodingIsCoolFr/singularity-updates'

# ---------------------------------------------------------------------------
# The version, from the one place that decides it.
# ---------------------------------------------------------------------------
$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(Singularity VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not find the version in CMakeLists.txt"
}
$version = $Matches[1]
$tag     = "v$version"

Write-Host "Releasing $tag" -ForegroundColor Cyan

# Every other file that carries the number has to agree, or the program
# reports one version while the installer writes another.
$mismatch = @()
$checks = @{
    'src\main.cpp'              = "setApplicationVersion\(QStringLiteral\(`"$([regex]::Escape($version))`"\)\)"
    'installer\Singularity.iss' = "#define AppVersion    `"$([regex]::Escape($version))`""
}
foreach ($file in $checks.Keys) {
    $text = Get-Content (Join-Path $root $file) -Raw
    if ($text -notmatch $checks[$file]) { $mismatch += $file }
}
if ($mismatch.Count -gt 0) {
    throw "These still carry a different version than $version : $($mismatch -join ', ')"
}

# A tag that already exists means this version was published before, and
# republishing it would quietly change what people already have.
$existing = git -C $root tag --list $tag
if ($existing -and -not $DryRun) {
    throw "$tag already exists. Bump the version rather than replacing a published one."
}

if (git -C $root status --porcelain) {
    throw "There are uncommitted changes. Commit them first, so the tag points at what shipped."
}

# ---------------------------------------------------------------------------
# Build.
# ---------------------------------------------------------------------------
# Stop is relaxed just for the build.
#
# vcvars writes a complaint about a missing vswhere to the error stream on
# this machine and still sets the environment correctly. With Stop in force
# that write becomes a terminating error and the release dies on a message
# that means nothing. The exit code is what actually decides.
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& (Join-Path $root 'build.ps1') -Installer
$buildExit = $LASTEXITCODE
$ErrorActionPreference = $previous

if ($buildExit -ne 0) { throw "Build failed with exit code $buildExit" }

$setup = Join-Path $root "dist-installer\Singularity-$version-setup.exe"
if (-not (Test-Path $setup)) { throw "No installer at $setup" }

$size = "{0:N0} MB" -f ((Get-Item $setup).Length / 1MB)
Write-Host "`nInstaller: $setup ($size)" -ForegroundColor Green

if ($DryRun) {
    Write-Host "Dry run, nothing published." -ForegroundColor Yellow
    return
}

# ---------------------------------------------------------------------------
# Publish. The channel goes first: it is the one the program reads, so if
# only one of the two lands it should be that one.
# ---------------------------------------------------------------------------
$notesFile = Join-Path $env:TEMP "singularity-notes-$version.md"
if (-not (Test-Path $notesFile)) {
    Set-Content -Path $notesFile -Encoding UTF8 -Value @"
Singularity $version.

**Download ``Singularity-$version-setup.exe`` below.** Windows x64, installs for you only, no administrator prompt.

Website: <https://synchord.pages.dev>

## Warning

Discord does not permit third party clients on a normal user account, and using one can get the account banned. Your password is sent to ``discord.com`` and nowhere else, is never written to disk and never logged; only the returned token is kept, sealed with your Windows account key.
"@
    Write-Host "Wrote placeholder notes to $notesFile - edit it and re-run to say more." -ForegroundColor Yellow
}

git -C $root tag -a $tag -m "Singularity $version"
git -C $root push -q origin $tag

foreach ($repo in @($channel, $source)) {
    Write-Host "Publishing to $repo" -ForegroundColor Cyan
    gh release create $tag "$setup#Singularity $version installer (Windows x64)" `
        --title "Singularity $version" --notes-file $notesFile --repo $repo
    if ($LASTEXITCODE -ne 0) { throw "Publishing to $repo failed" }
}

Write-Host "`n$tag is out, on both repositories." -ForegroundColor Green
