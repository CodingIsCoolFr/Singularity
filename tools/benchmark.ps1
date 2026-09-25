# Singularity vs Discord: resource use, measured side by side.
#
# Watches the two programs while they run - it starts nothing and changes
# nothing - and reports what each one costs the machine:
#
#   - memory: working set (RAM in use) and private bytes (RAM that is the
#     program's own), summed over every process it runs; Discord is one main
#     process plus helper processes (GPU, renderer, utility), Singularity is
#     one process
#   - CPU: average and peak, as a share of the whole machine
#   - processes and threads
#   - size on disk of the install
#
# Put both programs in the same state first (same server and channel open,
# or both in the same call) and leave them alone while it samples.
#
#   powershell -ExecutionPolicy Bypass -File tools\benchmark.ps1 -Seconds 60 -Label "idle in a text channel"
#
# Results are printed as a table and written to tools\benchmark-results.json
# (appended, one entry per run) so runs can be compared later.

param(
    [int]$Seconds = 60,
    [int]$IntervalMs = 1000,
    [string]$Label = "unlabelled run"
)

$ErrorActionPreference = 'Stop'
$cores = [Environment]::ProcessorCount

function Get-Tree([string[]]$rootNames) {
    # Every process whose image is one of $rootNames, plus all their children.
    $all = Get-CimInstance Win32_Process
    $ids = New-Object System.Collections.Generic.HashSet[int]
    foreach ($p in $all) { if ($rootNames -contains $p.Name) { [void]$ids.Add([int]$p.ProcessId) } }
    do {
        $added = 0
        foreach ($p in $all) {
            if ($ids.Contains([int]$p.ParentProcessId) -and -not $ids.Contains([int]$p.ProcessId)) {
                [void]$ids.Add([int]$p.ProcessId); $added++
            }
        }
    } while ($added -gt 0)
    return @($ids)
}

function Get-Sample([int[]]$ids) {
    $ws = 0L; $priv = 0L; $cpu = 0.0; $threads = 0; $count = 0
    foreach ($id in $ids) {
        try {
            $p = Get-Process -Id $id -ErrorAction Stop
            $ws += $p.WorkingSet64; $priv += $p.PrivateMemorySize64
            $cpu += $p.TotalProcessorTime.TotalMilliseconds
            $threads += $p.Threads.Count; $count++
        } catch { }
    }
    [pscustomobject]@{ WS = $ws; Private = $priv; CpuMs = $cpu; Threads = $threads; Count = $count }
}

function Get-FolderMB([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    [math]::Round((Get-ChildItem $path -Recurse -File -ErrorAction SilentlyContinue |
        Measure-Object Length -Sum).Sum / 1MB)
}

$apps = [ordered]@{
    'Singularity' = @{ Roots = @('Singularity.exe') }
    'Discord'     = @{ Roots = @('Discord.exe') }
}

foreach ($name in @($apps.Keys)) {
    $apps[$name].Ids = Get-Tree $apps[$name].Roots
    if ($apps[$name].Ids.Count -eq 0) { throw "$name is not running. Open it, then run this again." }
}

# Size on disk: the folder each running program lives in (one version of
# each - both keep older versions beside it, which would not be a fair count).
$singExe = (Get-Process -Name Singularity | Select-Object -First 1).Path
$discordExe = (Get-Process -Name Discord | Where-Object Path | Select-Object -First 1).Path
$apps['Singularity'].DiskMB = Get-FolderMB (Split-Path $singExe)
$apps['Discord'].DiskMB = Get-FolderMB (Split-Path $discordExe)

Write-Host "Sampling for $Seconds s ($Label)..."
$history = @{}
foreach ($name in $apps.Keys) { $history[$name] = New-Object System.Collections.ArrayList }

$previous = @{}
$clock = [Diagnostics.Stopwatch]::StartNew()
$lastMs = 0
foreach ($name in $apps.Keys) { $previous[$name] = Get-Sample $apps[$name].Ids }
while ($clock.ElapsedMilliseconds -lt $Seconds * 1000) {
    Start-Sleep -Milliseconds $IntervalMs
    $nowMs = $clock.ElapsedMilliseconds
    foreach ($name in $apps.Keys) {
        # Helper processes come and go; look again each time.
        $apps[$name].Ids = Get-Tree $apps[$name].Roots
        $s = Get-Sample $apps[$name].Ids
        $cpuPct = [math]::Max(0, ($s.CpuMs - $previous[$name].CpuMs)) / (($nowMs - $lastMs) * $cores) * 100
        [void]$history[$name].Add([pscustomobject]@{ WS = $s.WS; Private = $s.Private; Cpu = $cpuPct;
                                                     Threads = $s.Threads; Count = $s.Count })
        $previous[$name] = $s
    }
    $lastMs = $nowMs
}

$results = foreach ($name in $apps.Keys) {
    $h = $history[$name]
    [pscustomobject]@{
        App            = $name
        'RAM avg MB'   = [math]::Round(($h | Measure-Object WS -Average).Average / 1MB)
        'RAM peak MB'  = [math]::Round(($h | Measure-Object WS -Maximum).Maximum / 1MB)
        'Private MB'   = [math]::Round(($h | Measure-Object Private -Average).Average / 1MB)
        'CPU avg %'    = [math]::Round(($h | Measure-Object Cpu -Average).Average, 2)
        'CPU peak %'   = [math]::Round(($h | Measure-Object Cpu -Maximum).Maximum, 2)
        Processes      = ($h | Measure-Object Count -Maximum).Maximum
        Threads        = [math]::Round(($h | Measure-Object Threads -Average).Average)
        'Disk MB'      = $apps[$name].DiskMB
    }
}

$results | Format-Table -AutoSize

$cpuName = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$ramGB = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$entry = [pscustomobject]@{
    Date      = (Get-Date).ToString('s')
    Label     = $Label
    Seconds   = $Seconds
    Machine   = "$cpuName, $ramGB GB RAM, $cores logical cores"
    Windows   = (Get-CimInstance Win32_OperatingSystem).Caption
    Singularity = Split-Path (Split-Path $singExe) -Leaf
    Discord   = Split-Path (Split-Path $discordExe) -Leaf
    Results   = $results
}

$out = Join-Path $PSScriptRoot 'benchmark-results.json'
$runs = New-Object System.Collections.ArrayList

# Every saved run, flattened. Windows PowerShell 5.1 hands a parsed JSON array
# back as ONE object, so @(...) around it made a one-item list holding the
# whole array, which was then saved as {"value": [...], "Count": n} - nested
# one level deeper on every run. This walks any such nesting and keeps only
# real runs (objects with a Date).
function Add-Runs($node) {
    if ($null -eq $node) { return }
    if ($node -is [System.Array]) { foreach ($n in $node) { Add-Runs $n }; return }
    if ($node -is [string]) { return }
    $names = $node.PSObject.Properties.Name
    if ($names -contains 'Date') { [void]$runs.Add($node); return }
    if ($names -contains 'value') { Add-Runs $node.value }
}
if (Test-Path $out) {
    $parsed = Get-Content $out -Raw | ConvertFrom-Json
    Add-Runs $parsed
}
[void]$runs.Add($entry)
# Each run on its own, then joined: ConvertTo-Json on a whole array is what
# produces that wrapper in the first place.
$json = '[' + (($runs | ForEach-Object { ConvertTo-Json $_ -Depth 5 }) -join ",`n") + ']'
[IO.File]::WriteAllText($out, $json, (New-Object Text.UTF8Encoding $false))
Write-Host "Saved to $out"
