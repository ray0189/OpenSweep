<#
.SYNOPSIS
  OpenSweep benchmark harness (master plan Part 14). Runs repeatable workloads in one configuration
  and writes a JSON result file. Analyse one or more result files with: node bench/analyze.js <files>

.DESCRIPTION
  Configurations:
    stock    OpenSweep Core service stopped (and no console instance running).
    monitor  OpenSweep Core running in Monitor mode (measures only, never changes Windows settings).
    active   OpenSweep Core running in Balanced mode (low-risk reversible policies may be applied).
  Workloads (each repeated -Runs times, interleaved per run):
    launch      app launch time (Notepad, Calculator, Paint): first launch after 30 s idle ("cold",
                file cache NOT flushed) and 3 warm launches; time from Start-Process until a new
                top-level window exists and WaitForInputIdle returns.
    contention  N busy background processes (N = logical CPUs) + a foreground wake-up latency probe
                (Sleep(1) overshoot with 1 ms timer resolution), context switches/s, CPU queue length.
                The same probe without contention is recorded as a baseline.
    memory      a helper process fills memory until physical load reaches -MemoryTargetPercent (never
                leaving less than 10 % free), then: warm Notepad launch, probe, hard faults (page reads/s).
    disk        4 KiB random 70/30 read/write latency with Microsoft DiskSpd (if installed; otherwise
                recorded as skipped with the reason).
    idle        CPU % and working set of OpenSweepCore.exe while the PC idles (-IdleMinutes in total,
                split across runs). Not applicable to "stock".
  Run from an elevated Windows PowerShell 5.1 (or PowerShell 7) prompt on an idle, plugged-in PC.
  Nothing is uploaded. The result file contains hardware/Windows details but no user or computer name.

.EXAMPLE
  .\bench\run.ps1 -Config stock -Runs 5
  .\bench\run.ps1 -Config monitor -Runs 5
  .\bench\run.ps1 -Config active -Runs 5
  node bench\analyze.js bench\results\*.json --out bench\results\report.md
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][ValidateSet('stock', 'monitor', 'active')][string]$Config,
  [ValidateRange(1, 100)][int]$Runs = 5,
  [string]$OutDir = '',
  [string]$CoreExe = '',
  [string]$DiskSpd = '',
  [ValidateRange(1, 240)][int]$IdleMinutes = 10,
  [ValidateSet('launch', 'contention', 'memory', 'disk', 'idle')][string[]]$Workloads = @('launch', 'contention', 'memory', 'disk', 'idle'),
  [ValidateRange(5, 300)][int]$ContentionSeconds = 20,
  [ValidateRange(50, 90)][int]$MemoryTargetPercent = 85,
  [switch]$Force
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ServiceName = 'OpenSweepCore'
$PipeName = 'OpenSweepCore.v1'
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot 'results' }

# ------------------------------------------------------------------ helpers
function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  return (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Get-Prop($obj, [string]$name) { if ($null -ne $obj -and $obj.PSObject.Properties[$name]) { return $obj.$name } return $null }

# P/Invoke defined in memory (no compiled DLL written to disk).
$script:Native = $null
function Get-Native {
  if ($script:Native) { return $script:Native }
  $asm = [Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly((New-Object Reflection.AssemblyName 'OpenSweepBench'), [Reflection.Emit.AssemblyBuilderAccess]::Run)
  $tb = $asm.DefineDynamicModule('OpenSweepBench').DefineType('OpenSweepBench.Native', 'Public, Class')
  $attr = [Reflection.MethodAttributes]'Public, Static, PinvokeImpl'
  foreach ($name in @('timeBeginPeriod', 'timeEndPeriod')) {
    $m = $tb.DefinePInvokeMethod($name, 'winmm.dll', $attr, [Reflection.CallingConventions]::Standard, [UInt32], [Type[]]@([UInt32]), [Runtime.InteropServices.CallingConvention]::Winapi, [Runtime.InteropServices.CharSet]::Auto)
    $m.SetImplementationFlags('PreserveSig')
  }
  $script:Native = $tb.CreateType()
  return $script:Native
}

# Minimal OpenSweep Core pipe client (protocol v1: 4-byte little-endian length + UTF-8 JSON).
function Read-Exactly($stream, [int]$count) {
  $buf = New-Object byte[] $count
  $off = 0
  while ($off -lt $count) {
    $n = $stream.Read($buf, $off, $count - $off)
    if ($n -le 0) { throw 'OpenSweep Core closed the pipe' }
    $off += $n
  }
  return , $buf
}
function Invoke-CoreCommand([string]$Cmd, [hashtable]$CmdArgs = $null, [int]$TimeoutMs = 5000) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
  try {
    $pipe.Connect($TimeoutMs)
    $req = @{ v = 1; id = 1; cmd = $Cmd }
    if ($CmdArgs) { $req.args = $CmdArgs }
    $body = [Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject $req -Compress -Depth 5))
    $head = [BitConverter]::GetBytes([UInt32]$body.Length)
    $pipe.Write($head, 0, 4); $pipe.Write($body, 0, $body.Length); $pipe.Flush()
    $len = [BitConverter]::ToUInt32((Read-Exactly $pipe 4), 0)
    if ($len -eq 0 -or $len -gt 1048576) { throw "OpenSweep Core sent a frame of $len bytes" }
    $resp = [Text.Encoding]::UTF8.GetString((Read-Exactly $pipe ([int]$len))) | ConvertFrom-Json
    if (-not $resp.ok) { throw "OpenSweep Core refused '$Cmd': $($resp.error.code) $($resp.error.message)" }
    return $resp.result
  } finally { $pipe.Dispose() }
}

function Get-CoreServiceState {
  $s = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
  if ($s) { return $s.Status.ToString() }
  return 'NotInstalled'
}
function Wait-CoreServiceState([string]$want, [int]$seconds = 20) {
  $end = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $end) { if ((Get-CoreServiceState) -eq $want) { return $true }; Start-Sleep -Milliseconds 300 }
  return $false
}
function Start-CoreService {
  if ((Get-CoreServiceState) -eq 'NotInstalled') {
    if (-not $CoreExe -or -not (Test-Path -LiteralPath $CoreExe)) { throw "The $ServiceName service is not installed. Install OpenSweep Core from the app, or pass -CoreExe <path to OpenSweepCore.exe>." }
    & $CoreExe --install | Out-Host
  }
  if ((Get-CoreServiceState) -ne 'Running') { Start-Service -Name $ServiceName }
  if (-not (Wait-CoreServiceState 'Running')) { throw "$ServiceName did not reach Running" }
  $end = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $end) { try { [void](Invoke-CoreCommand 'hello'); return } catch { Start-Sleep -Milliseconds 300 } }
  throw 'OpenSweep Core is running but does not answer on its pipe'
}
function Set-BenchConfig([string]$cfg) {
  switch ($cfg) {
    'stock' {
      if ((Get-CoreServiceState) -eq 'Running') { Stop-Service -Name $ServiceName; [void](Wait-CoreServiceState 'Stopped') }
      if (Get-Process -Name 'OpenSweepCore' -ErrorAction SilentlyContinue) { throw 'An OpenSweepCore.exe process is still running (console mode?). Close it for the stock configuration.' }
    }
    'monitor' { Start-CoreService; [void](Invoke-CoreCommand 'setMode' @{ mode = 'monitor' }) }
    'active' { Start-CoreService; [void](Invoke-CoreCommand 'setMode' @{ mode = 'balanced' }) }
  }
}

function Get-SystemInfo {
  $os = Get-CimInstance Win32_OperatingSystem
  $cpu = @(Get-CimInstance Win32_Processor)
  $cs = Get-CimInstance Win32_ComputerSystem
  $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
  $disks = @()
  try { $disks = @(Get-PhysicalDisk | ForEach-Object { @{ model = "$($_.FriendlyName)"; media = "$($_.MediaType)"; bus = "$($_.BusType)"; sizeBytes = [double]$_.Size } }) } catch { }
  $bat = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)
  $onAc = $true
  if ($bat.Count -gt 0) { $onAc = ([int]$bat[0].BatteryStatus -eq 2) }
  $plan = ''
  try { $plan = ((& powercfg.exe /getactivescheme) -join ' ').Trim() } catch { }
  $coreVersion = $null; $coreMode = $null
  try { $h = Invoke-CoreCommand 'hello' -TimeoutMs 1500; $coreVersion = $h.version; $coreMode = $h.mode } catch { }
  return [ordered]@{
    osCaption = "$($os.Caption)"; osVersion = "$($os.Version)"; osBuild = "$($os.BuildNumber)"; osUbr = Get-Prop $cv 'UBR'; displayVersion = Get-Prop $cv 'DisplayVersion'
    arch = "$env:PROCESSOR_ARCHITECTURE"; cpu = "$($cpu[0].Name)".Trim(); cpuSockets = $cpu.Count
    cores = [int]($cpu | Measure-Object -Property NumberOfCores -Sum).Sum; logicalProcessors = [int]($cpu | Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum
    ramBytes = [double]$cs.TotalPhysicalMemory; systemModel = "$($cs.Manufacturer) $($cs.Model)".Trim(); hypervisorPresent = [bool](Get-Prop $cs 'HypervisorPresent')
    disks = $disks; powerPlan = $plan; onAcPower = $onAc; psVersion = $PSVersionTable.PSVersion.ToString()
    coreServiceState = Get-CoreServiceState; coreVersion = $coreVersion; coreMode = $coreMode
  }
}

# ------------------------------------------------------------------ result accumulation
$script:Results = [ordered]@{}
$script:Skipped = New-Object System.Collections.ArrayList
function Add-Samples([string]$id, [string]$unit, $values, [string]$note = '', [string]$better = 'lower') {
  if (-not $script:Results.Contains($id)) { $script:Results[$id] = @{ unit = $unit; better = $better; note = $note; runs = 0; samples = New-Object 'System.Collections.Generic.List[double]' } }
  $r = $script:Results[$id]
  $r.runs++
  foreach ($v in @($values)) { if ($null -ne $v) { $r.samples.Add([double]$v) } }
}
function Add-Skipped([string]$id, [string]$reason) {
  if (-not ($script:Skipped | Where-Object { $_.id -eq $id })) { [void]$script:Skipped.Add([ordered]@{ id = $id; reason = $reason }) }
  Write-Warning "skipped ${id}: $reason"
}
function Get-Percentile([double[]]$sorted, [double]$q) {
  if ($sorted.Count -eq 0) { return $null }
  $h = ($sorted.Count - 1) * $q; $lo = [Math]::Floor($h); $hi = [Math]::Ceiling($h)
  return $sorted[$lo] + ($h - $lo) * ($sorted[$hi] - $sorted[$lo])
}
# Evenly spaced subset (keeps result files small; the distribution shape is preserved).
function Get-Subset([double[]]$values, [int]$max = 2000) {
  if ($values.Count -le $max) { return , $values }
  $step = $values.Count / $max
  $out = New-Object double[] $max
  for ($i = 0; $i -lt $max; $i++) { $out[$i] = $values[[int][Math]::Floor($i * $step)] }
  return , $out
}

# ------------------------------------------------------------------ workloads
$LaunchApps = @(
  @{ id = 'notepad'; exe = 'notepad.exe'; procs = @('notepad') },
  @{ id = 'calc'; exe = 'calc.exe'; procs = @('CalculatorApp', 'Calculator', 'calc') },
  @{ id = 'mspaint'; exe = 'mspaint.exe'; procs = @('mspaint', 'PaintApp') }
)
$script:Owned = @{}  # pids this script started (only these are ever closed)
function Close-Owned($app) {
  $procs = @(Get-Process -Name $app.procs -ErrorAction SilentlyContinue | Where-Object { $script:Owned.ContainsKey($_.Id) })
  foreach ($p in $procs) { try { [void]$p.CloseMainWindow() } catch { } }
  Start-Sleep -Milliseconds 1500
  foreach ($p in $procs) { try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } } catch { } }
}
function Measure-Launch($app, [int]$TimeoutMs = 30000) {
  $before = @{}
  foreach ($p in @(Get-Process -Name $app.procs -ErrorAction SilentlyContinue)) { $before[$p.Id] = $true }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [void](Start-Process -FilePath $app.exe -PassThru)
  $ms = $null
  while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
    $p = @(Get-Process -Name $app.procs -ErrorAction SilentlyContinue | Where-Object { -not $before.ContainsKey($_.Id) -and $_.MainWindowHandle -ne [IntPtr]::Zero }) | Select-Object -First 1
    if ($p) {
      try { [void]$p.WaitForInputIdle([Math]::Max(1, $TimeoutMs - [int]$sw.ElapsedMilliseconds)) } catch { }
      $ms = $sw.Elapsed.TotalMilliseconds
      break
    }
    Start-Sleep -Milliseconds 10
  }
  foreach ($p in @(Get-Process -Name $app.procs -ErrorAction SilentlyContinue)) { if (-not $before.ContainsKey($p.Id)) { $script:Owned[$p.Id] = $true } }
  Close-Owned $app
  return $ms
}
function Invoke-LaunchWorkload([int]$run) {
  foreach ($app in $LaunchApps) {
    if (-not (Get-Command $app.exe -ErrorAction SilentlyContinue)) { Add-Skipped "launch.$($app.id)" "$($app.exe) not found on this PC"; continue }
    Start-Sleep -Seconds 30  # settle: the first launch after idle is the "cold" sample
    $cold = Measure-Launch $app
    if ($null -eq $cold) { Add-Skipped "launch.$($app.id)" "no window appeared within 30 s (run $run)"; continue }
    Add-Samples "launch.$($app.id).cold" 'ms' $cold 'first launch after closing the app and 30 s idle; file cache not flushed (not a post-reboot cold start)'
    $warm = @()
    for ($i = 0; $i -lt 3; $i++) { Start-Sleep -Seconds 2; $warm += Measure-Launch $app }
    Add-Samples "launch.$($app.id).warm" 'ms' $warm 'immediately repeated launches (3 per run)'
  }
}

# Foreground wake-up latency: overshoot of Sleep(1) with 1 ms timer resolution, in microseconds.
function Measure-WakeupProbe([int]$seconds) {
  $native = Get-Native
  [void]$native::timeBeginPeriod(1)
  try {
    $freq = [Diagnostics.Stopwatch]::Frequency
    $delays = New-Object 'System.Collections.Generic.List[double]'
    $end = [Diagnostics.Stopwatch]::StartNew()
    while ($end.Elapsed.TotalSeconds -lt $seconds) {
      $t0 = [Diagnostics.Stopwatch]::GetTimestamp()
      [Threading.Thread]::Sleep(1)
      $dt = ([Diagnostics.Stopwatch]::GetTimestamp() - $t0) * 1e6 / $freq
      $delays.Add([Math]::Max(0, $dt - 1000))
    }
    return , $delays.ToArray()
  } finally { [void]$native::timeEndPeriod(1) }
}
function Start-Hidden([string]$script) {
  $ps = (Get-Process -Id $PID).Path
  $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
  return Start-Process -FilePath $ps -ArgumentList @('-NoProfile', '-NonInteractive', '-EncodedCommand', $enc) -WindowStyle Hidden -PassThru
}
function New-Counter([string]$category, [string]$counter, [string]$instance = '') {
  try { if ($instance) { $c = New-Object Diagnostics.PerformanceCounter($category, $counter, $instance, $true) } else { $c = New-Object Diagnostics.PerformanceCounter($category, $counter, $true) }; [void]$c.NextValue(); return $c } catch { return $null }
}
function Invoke-ContentionWorkload([int]$run) {
  $idle = Measure-WakeupProbe 5
  Add-Samples 'probe.idle.wakeupDelayUs' 'us' (Get-Subset $idle) 'Sleep(1) overshoot, no load'
  $n = [Environment]::ProcessorCount
  $spin = @()
  $cs = New-Counter 'System' 'Context Switches/sec'
  $ql = New-Counter 'System' 'Processor Queue Length'
  try {
    for ($i = 0; $i -lt $n; $i++) { $spin += Start-Hidden 'while ($true) { }' }
    Start-Sleep -Seconds 2
    $csv = @(); $qlv = @()
    $job = [Diagnostics.Stopwatch]::StartNew()
    $delays = New-Object 'System.Collections.Generic.List[double]'
    while ($job.Elapsed.TotalSeconds -lt $ContentionSeconds) {
      $d = Measure-WakeupProbe 1
      $delays.AddRange([double[]]$d)
      if ($cs) { $csv += $cs.NextValue() }
      if ($ql) { $qlv += $ql.NextValue() }
    }
    $all = $delays.ToArray(); [Array]::Sort($all)
    Add-Samples 'cpu.contention.wakeupDelayUs' 'us' (Get-Subset $delays.ToArray()) "Sleep(1) overshoot with $n busy background processes"
    Add-Samples 'cpu.contention.wakeupDelayP99Us' 'us' (Get-Percentile $all 0.99) 'per-run p99 of the probe'
    if ($cs) { Add-Samples 'cpu.contention.contextSwitchesPerSec' '/s' $csv } else { Add-Skipped 'cpu.contention.contextSwitchesPerSec' 'performance counter System\Context Switches/sec not available' }
    if ($ql) { Add-Samples 'cpu.contention.queueLength' 'threads' $qlv } else { Add-Skipped 'cpu.contention.queueLength' 'performance counter System\Processor Queue Length not available' }
  } finally {
    foreach ($p in $spin) { try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch { } }
  }
}

function Invoke-MemoryWorkload([int]$run) {
  $os = Get-CimInstance Win32_OperatingSystem
  $total = [double]$os.TotalVisibleMemorySize * 1KB; $avail = [double]$os.FreePhysicalMemory * 1KB
  $used = $total - $avail
  $want = $total * $MemoryTargetPercent / 100 - $used
  $want = [Math]::Min($want, $avail - 0.10 * $total)  # never leave less than 10 % free
  $mb = [int][Math]::Floor($want / 1MB / 64) * 64
  if ($mb -lt 256) { Add-Skipped 'memory.pressure' "physical memory load is already near $MemoryTargetPercent % (would allocate only $mb MB)"; return }
  $ready = Join-Path $env:TEMP "opensweep-bench-mem-$PID-$run.ready"
  Remove-Item -LiteralPath $ready -ErrorAction SilentlyContinue
  # Random bytes so memory compression cannot shrink the pressure; the helper exits by itself after 180 s.
  $script = "`$c = @(); `$r = [Security.Cryptography.RandomNumberGenerator]::Create(); for (`$i = 0; `$i -lt $($mb / 64); `$i++) { `$b = New-Object byte[] 64MB; `$r.GetBytes(`$b); `$c += ,`$b }; Set-Content -LiteralPath '$($ready.Replace("'", "''"))' -Value 'ok'; Start-Sleep -Seconds 180"
  $hog = Start-Hidden $script
  try {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath $ready) -and $sw.Elapsed.TotalSeconds -lt 150 -and -not $hog.HasExited) { Start-Sleep -Milliseconds 250 }
    if (-not (Test-Path -LiteralPath $ready)) { Add-Skipped 'memory.pressure' "the helper could not allocate $mb MB (run $run)"; return }
    $pr = New-Counter 'Memory' 'Page Reads/sec'
    $launch = @()
    for ($i = 0; $i -lt 3; $i++) { $launch += Measure-Launch $LaunchApps[0]; Start-Sleep -Seconds 1 }
    Add-Samples 'memory.pressure.launch.notepad' 'ms' $launch "warm Notepad launch with physical memory load near $MemoryTargetPercent %"
    $d = Measure-WakeupProbe 5
    Add-Samples 'memory.pressure.wakeupDelayUs' 'us' (Get-Subset $d) 'Sleep(1) overshoot under memory pressure'
    if ($pr) { $v = @(); for ($i = 0; $i -lt 10; $i++) { Start-Sleep -Seconds 1; $v += $pr.NextValue() }; Add-Samples 'memory.pressure.pageReadsPerSec' '/s' $v 'hard faults that hit the disk' }
    else { Add-Skipped 'memory.pressure.pageReadsPerSec' 'performance counter Memory\Page Reads/sec not available' }
  } finally {
    try { Stop-Process -Id $hog.Id -Force -ErrorAction SilentlyContinue } catch { }
    Remove-Item -LiteralPath $ready -ErrorAction SilentlyContinue
  }
}

function Invoke-DiskWorkload([int]$run) {
  $exe = $DiskSpd
  if (-not $exe) { $c = Get-Command 'diskspd.exe' -ErrorAction SilentlyContinue; if ($c) { $exe = $c.Source } }
  if (-not $exe -or -not (Test-Path -LiteralPath $exe)) { Add-Skipped 'disk.randrw4k' 'diskspd.exe not found. Install Microsoft DiskSpd (github.com/microsoft/diskspd) and pass -DiskSpd <path>.'; return }
  $file = Join-Path $env:TEMP 'opensweep-bench-diskspd.dat'
  try {
    [xml]$x = (& $exe -c1G -d20 -W3 -r -w30 -t2 -o8 -b4K -L -Sh -Rxml $file) -join "`n"
    $buckets = @($x.Results.TimeSpan.Latency.Bucket)
    $p = { param($q) $b = $buckets | Where-Object { [double]$_.Percentile -eq $q } | Select-Object -First 1; if ($b) { [double]$b.TotalMilliseconds } else { $null } }
    $p50 = & $p 50; $p99 = & $p 99
    if ($null -eq $p50) { Add-Skipped 'disk.randrw4k' 'could not read latency percentiles from the DiskSpd XML'; return }
    Add-Samples 'disk.randrw4k.p50Ms' 'ms' $p50 'DiskSpd 4 KiB random, 30 % writes, 2 threads x 8 outstanding, caching off, 1 GiB file in %TEMP%'
    Add-Samples 'disk.randrw4k.p99Ms' 'ms' $p99 'DiskSpd 4 KiB random, 30 % writes, 2 threads x 8 outstanding, caching off, 1 GiB file in %TEMP%'
  } finally { Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
}

function Invoke-IdleWorkload([int]$run, [double]$minutes) {
  if ($Config -eq 'stock') { Add-Skipped 'idle.core' 'no OpenSweep Core in the stock configuration'; return }
  $svc = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
  if (-not $svc -or -not $svc.ProcessId) { Add-Skipped 'idle.core' 'OpenSweep Core service process not found'; return }
  $p = Get-Process -Id $svc.ProcessId
  $n = [Environment]::ProcessorCount
  $cpu = @(); $ws = @(); $pv = @()
  $end = (Get-Date).AddMinutes($minutes)
  $p.Refresh(); $t0 = $p.TotalProcessorTime.TotalMilliseconds; $w0 = [Diagnostics.Stopwatch]::StartNew()
  while ((Get-Date) -lt $end) {
    Start-Sleep -Seconds 10
    $p.Refresh()
    $t1 = $p.TotalProcessorTime.TotalMilliseconds; $el = $w0.Elapsed.TotalMilliseconds
    $cpu += 100 * ($t1 - $t0) / ($el * $n); $t0 = $t1; $w0.Restart()
    $ws += $p.WorkingSet64 / 1MB; $pv += $p.PrivateMemorySize64 / 1MB
  }
  Add-Samples 'idle.coreCpuPercent' '%' $cpu "OpenSweepCore.exe CPU over 10 s intervals, % of all $n logical CPUs"
  Add-Samples 'idle.coreWorkingSetMB' 'MB' $ws 'OpenSweepCore.exe working set every 10 s'
  Add-Samples 'idle.corePrivateMB' 'MB' $pv 'OpenSweepCore.exe private bytes every 10 s'
}

# ------------------------------------------------------------------ main
if (-not (Test-Admin)) { throw 'Run this from an elevated PowerShell: changing the OpenSweep Core service and mode needs administrator rights.' }
$running = @($LaunchApps | ForEach-Object { Get-Process -Name $_.procs -ErrorAction SilentlyContinue })
if ($Workloads -contains 'launch' -or $Workloads -contains 'memory') {
  if ($running.Count -gt 0) { throw "Close these apps first (the harness never touches windows it did not open): $(($running | ForEach-Object { $_.ProcessName } | Sort-Object -Unique) -join ', ')" }
}
if (-not $Force) {
  Write-Host "OpenSweep benchmark: configuration '$Config', $Runs run(s), workloads: $($Workloads -join ', ')."
  Write-Host 'It will change the OpenSweep Core service state/mode, start and close Notepad/Calculator/Paint, run CPU and memory load generators and (if installed) DiskSpd.'
  Write-Host 'Close other apps, plug in the power, and do not use the PC while it runs.'
  if ((Read-Host 'Type YES to continue') -ne 'YES') { Write-Host 'Cancelled.'; return }
}
$initialState = Get-CoreServiceState
$initialMode = $null
try { $initialMode = (Invoke-CoreCommand 'status' -TimeoutMs 1500).mode } catch { }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$started = (Get-Date).ToUniversalTime().ToString('o')
try {
  Set-BenchConfig $Config
  Start-Sleep -Seconds 10
  $system = Get-SystemInfo
  Write-Host "System: $($system.osCaption) build $($system.osBuild).$($system.osUbr) - $($system.cpu) - $([Math]::Round($system.ramBytes / 1GB, 1)) GB - Core: $($system.coreServiceState) $($system.coreMode)"
  for ($run = 1; $run -le $Runs; $run++) {
    Write-Host "== run $run of $Runs"
    if ($Workloads -contains 'launch') { Write-Host '  app launch'; Invoke-LaunchWorkload $run }
    if ($Workloads -contains 'contention') { Write-Host '  CPU contention + latency probe'; Invoke-ContentionWorkload $run }
    if ($Workloads -contains 'memory') { Write-Host '  memory pressure'; Invoke-MemoryWorkload $run }
    if ($Workloads -contains 'disk') { Write-Host '  disk latency'; Invoke-DiskWorkload $run }
    if ($Workloads -contains 'idle') { Write-Host "  idle overhead ($([Math]::Round($IdleMinutes / $Runs, 1)) min)"; Invoke-IdleWorkload $run ($IdleMinutes / $Runs) }
  }
} finally {
  # Leave the PC as it was: service state and (if it was answering) its mode.
  try {
    if ($initialState -eq 'Running' -and (Get-CoreServiceState) -ne 'Running') { Start-Service -Name $ServiceName; [void](Wait-CoreServiceState 'Running') }
    if ($initialState -ne 'Running' -and (Get-CoreServiceState) -eq 'Running') { Stop-Service -Name $ServiceName }
    if ($initialMode -and (Get-CoreServiceState) -eq 'Running') { Start-Sleep -Seconds 2; [void](Invoke-CoreCommand 'setMode' @{ mode = $initialMode }) }
  } catch { Write-Warning "Could not restore the OpenSweep Core state ($initialState, mode $initialMode): $_" }
}

$workloads = @(foreach ($k in $script:Results.Keys) { $r = $script:Results[$k]; [ordered]@{ id = $k; unit = $r.unit; better = $r.better; note = $r.note; runs = $r.runs; samples = $r.samples.ToArray() } })
$result = [ordered]@{
  schema = 1; tool = 'opensweep-bench'; config = $Config; runs = $Runs; startedAt = $started; finishedAt = (Get-Date).ToUniversalTime().ToString('o')
  parameters = [ordered]@{ workloads = $Workloads; contentionSeconds = $ContentionSeconds; memoryTargetPercent = $MemoryTargetPercent; idleMinutes = $IdleMinutes }
  system = $system; workloads = $workloads; skipped = @($script:Skipped)
}
$file = Join-Path $OutDir ("{0}-{1}.json" -f $Config, (Get-Date -Format 'yyyyMMdd-HHmmss'))
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $file -Encoding UTF8
Write-Host "Results written to $file"
Write-Host "Analyse with: node bench\analyze.js $OutDir\*.json --out $OutDir\report.md"
