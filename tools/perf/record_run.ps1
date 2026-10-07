<#
Record the machine while Spirula trains, for tools/perf/perf_report.py.

Launches spirula.exe with SS_TRAIN_PERF pointing at the session folder, so the
trainer writes train_perf.csv beside this script's samples, then records once a
second until every spirula.exe has exited (or Ctrl+C):

  system.csv       CPU per core and total, RAM, disk, and the summed CPU,
                   memory, threads and I/O of every spirula.exe (a dense
                   step's child process included)
  gpu_engines.csv  Windows' per-engine GPU utilization for spirula.exe, by
                   adapter LUID, for every GPU (integrated and discrete)
  nvidia.csv       nvidia-smi: utilization, VRAM, clocks, power, temperature
                   and clock-limit reasons, when an NVIDIA driver is present
  meta.json        machine, adapters and session details

Windows only; needs nothing beyond PowerShell and the GPU driver. Counter names
are the English ones, so a localized Windows needs them translated.

  pwsh tools/perf/record_run.ps1                       # launch the GUI and record
  pwsh tools/perf/record_run.ps1 -Attach               # record an already running Spirula
  pwsh tools/perf/record_run.ps1 -Exe <path to spirula.exe> -Out <folder> -- train --data <dataset folder>
#>
[CmdletBinding()]
param(
    [string]$Exe = (Join-Path $PSScriptRoot "..\..\build_vulkan\spirula.exe"),
    [string]$Out = "",
    [switch]$Attach,
    [double]$Interval = 1.0,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$SpirulaArgs
)
$ErrorActionPreference = "Stop"

if (-not $Out) {
    $Out = Join-Path $PSScriptRoot ("..\..\build_vulkan\perf\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Write-Host "Recording into $Out"

function UnixMs { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }
function NewCounter($cat, $name, $inst) {
    try { $c = [System.Diagnostics.PerformanceCounter]::new($cat, $name, $inst, $true); [void]$c.NextValue(); $c }
    catch { $null }
}
function Read($c) { if ($null -eq $c) { return "" } try { [math]::Round($c.NextValue(), 3) } catch { "" } }

# ---- launch or attach -----------------------------------------------------
$launched = $null
if (-not $Attach) {
    $env:SS_TRAIN_PERF = $Out
    $exePath = (Resolve-Path $Exe).Path
    $launched = if ($SpirulaArgs) { Start-Process -FilePath $exePath -ArgumentList $SpirulaArgs -PassThru }
                else { Start-Process -FilePath $exePath -PassThru }
    Write-Host "Started spirula.exe (pid $($launched.Id)); train_perf.csv will be written here."
} else {
    Write-Host "Attaching: train_perf.csv needs SS_TRAIN_PERF set when Spirula was started."
}

# ---- nvidia-smi, queried each sample (~130 ms) so its rows share our clock --------
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
$smiFields = "index,name,utilization.gpu,utilization.memory,memory.used,memory.total,temperature.gpu," +
             "power.draw,clocks.sm,clocks.mem,pstate,clocks_event_reasons.active"

# ---- static counters ---------------------------------------------------------
$cores = [Environment]::ProcessorCount
$cpu = @(0..($cores - 1) | ForEach-Object { NewCounter "Processor" "% Processor Time" "$_" })
$cpuTotal = NewCounter "Processor" "% Processor Time" "_Total"
$memAvail = NewCounter "Memory" "Available Bytes" ""
$memCommit = NewCounter "Memory" "Committed Bytes" ""
$diskRead = NewCounter "PhysicalDisk" "Disk Read Bytes/sec" "_Total"
$diskWrite = NewCounter "PhysicalDisk" "Disk Write Bytes/sec" "_Total"
$diskBusy = NewCounter "PhysicalDisk" "% Disk Time" "_Total"
$ramTotal = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory

# Adapters: LUID -> dedicated memory limit, which tells the discrete GPU from the integrated one.
$adapters = @{}
try {
    $cat = [System.Diagnostics.PerformanceCounterCategory]::new("GPU Adapter Memory")
    foreach ($inst in $cat.GetInstanceNames()) {
        if ($inst -match "luid_0x[0-9a-fA-F]+_0x[0-9a-fA-F]+") {
            $adapters[$Matches[0]] = @{ instance = $inst }
        }
    }
} catch {}

# ---- per-process counters, rescanned as processes and GPU engines appear --------
$procCounters = @{}   # pid -> hashtable of counters
$engineCounters = @{} # instance -> counter
function Rescan {
    $pids = @(Get-Process spirula -ErrorAction SilentlyContinue | ForEach-Object Id)
    foreach ($id in @($procCounters.Keys)) { if ($pids -notcontains $id) { $procCounters.Remove($id) } }
    foreach ($id in $pids) {
        if ($procCounters.ContainsKey($id)) { continue }
        $inst = "spirula:$id"
        $procCounters[$id] = @{
            cpu     = NewCounter "Process V2" "% Processor Time" $inst
            private = NewCounter "Process V2" "Private Bytes" $inst
            ws      = NewCounter "Process V2" "Working Set" $inst
            threads = NewCounter "Process V2" "Thread Count" $inst
            read    = NewCounter "Process V2" "IO Read Bytes/sec" $inst
            write   = NewCounter "Process V2" "IO Write Bytes/sec" $inst
        }
    }
    try {
        $cat = [System.Diagnostics.PerformanceCounterCategory]::new("GPU Engine")
        $names = $cat.GetInstanceNames()
        foreach ($n in @($engineCounters.Keys)) { if ($names -notcontains $n) { $engineCounters.Remove($n) } }
        foreach ($id in $pids) {
            foreach ($n in ($names | Where-Object { $_ -like "pid_$($id)_*" })) {
                if (-not $engineCounters.ContainsKey($n)) {
                    $c = NewCounter "GPU Engine" "Utilization Percentage" $n
                    if ($c) { $engineCounters[$n] = $c }
                }
            }
        }
    } catch {}
    return $pids
}

# ---- meta ----------------------------------------------------------------------
$cpuInfo = Get-CimInstance Win32_Processor | Select-Object -First 1
$gpuInfo = @(Get-CimInstance Win32_VideoController | ForEach-Object { @{ name = $_.Name; driver = $_.DriverVersion } })
$meta = [ordered]@{
    started_unix_ms = (UnixMs); interval_s = $Interval; out = $Out
    cpu = $cpuInfo.Name; cores = $cpuInfo.NumberOfCores; logical_processors = $cores
    ram_bytes = $ramTotal; gpus = $gpuInfo; adapter_luids = @($adapters.Keys)
    os = (Get-CimInstance Win32_OperatingSystem).Caption
    spirula = if ($launched) { @{ exe = $exePath; pid = $launched.Id; args = $SpirulaArgs } } else { @{ attached = $true } }
}
$meta | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $Out "meta.json")

# ---- sampling loop ---------------------------------------------------------------
$sys = [System.IO.StreamWriter]::new((Join-Path $Out "system.csv"))
$eng = [System.IO.StreamWriter]::new((Join-Path $Out "gpu_engines.csv"))
$coreCols = (0..($cores - 1) | ForEach-Object { "cpu_$_" }) -join ","
$sys.WriteLine("unix_ms,cpu_total,$coreCols,ram_avail_bytes,ram_committed_bytes,ram_total_bytes," +
               "disk_read_bps,disk_write_bps,disk_busy_pct,spirula_procs,spirula_cpu_pct,spirula_private_bytes," +
               "spirula_ws_bytes,spirula_threads,spirula_read_bps,spirula_write_bps")
$eng.WriteLine("unix_ms,pid,luid,engine,util_pct")
$nv = if ($smi) { [System.IO.StreamWriter]::new((Join-Path $Out "nvidia.csv")) } else { $null }
if ($nv) { $nv.WriteLine("unix_ms,$($smiFields -replace '\.', '_')") }

$pids = Rescan
$lastScan = [DateTime]::UtcNow
$seen = $pids.Count -gt 0
Start-Sleep -Milliseconds ([int]($Interval * 1000))
try {
    while ($true) {
        $tick = [DateTime]::UtcNow
        if (($tick - $lastScan).TotalSeconds -ge 15) { $pids = Rescan; $lastScan = $tick }
        if ($pids.Count -gt 0) { $seen = $true }
        $alive = @(Get-Process spirula -ErrorAction SilentlyContinue)
        if ($seen -and $alive.Count -eq 0) { break }
        if ($alive.Count -ne $pids.Count) { $pids = Rescan; $lastScan = $tick }

        $ms = UnixMs
        $agg = @{ cpu = 0.0; private = 0.0; ws = 0.0; threads = 0.0; read = 0.0; write = 0.0 }
        foreach ($pc in $procCounters.Values) {
            foreach ($k in @($agg.Keys)) { $v = Read $pc[$k]; if ($v -ne "") { $agg[$k] += $v } }
        }
        $coreVals = ($cpu | ForEach-Object { Read $_ }) -join ","
        $sys.WriteLine("$ms,$(Read $cpuTotal),$coreVals,$(Read $memAvail),$(Read $memCommit),$ramTotal," +
                       "$(Read $diskRead),$(Read $diskWrite),$(Read $diskBusy),$($procCounters.Count)," +
                       "$($agg.cpu),$($agg.private),$($agg.ws),$($agg.threads),$($agg.read),$($agg.write)")
        foreach ($kv in $engineCounters.GetEnumerator()) {
            $v = Read $kv.Value
            if ($v -ne "" -and $kv.Key -match "^pid_(\d+)_(luid_0x[0-9a-fA-F]+_0x[0-9a-fA-F]+)_.*engtype_(.+)$") {
                $eng.WriteLine("$ms,$($Matches[1]),$($Matches[2]),$($Matches[3]),$v")
            }
        }
        if ($nv) {
            foreach ($line in @(& $smi.Source "--query-gpu=$smiFields" "--format=csv,noheader,nounits" 2>$null)) {
                if ($line) { $nv.WriteLine("$ms," + ($line -replace ',\s+', ',')) }
            }
            $nv.Flush()
        }
        $sys.Flush(); $eng.Flush()
        $spent = ([DateTime]::UtcNow - $tick).TotalMilliseconds
        Start-Sleep -Milliseconds ([int][math]::Max(50, $Interval * 1000 - $spent))
    }
} finally {
    $sys.Close(); $eng.Close()
    if ($nv) { $nv.Close() }
    Write-Host "Recording stopped. Report:"
    Write-Host "  python tools/perf/perf_report.py `"$Out`""
}
