'use strict';
// Live process list: one long-running PowerShell loop that prints a JSON line every
// ~2 s while the Processes page is open (much cheaper than starting PowerShell each time).
const { ps, stream } = require('./pscore');

const LOOP = `
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
$cores = [Environment]::ProcessorCount
$prevCpu = @{}; $prevIo = @{}; $info = @{}; $sig = @{}
$sw = [Diagnostics.Stopwatch]::StartNew(); $last = 0.0
$win = $env:SystemRoot.ToLower()
while ($true) {
  $now = $sw.Elapsed.TotalSeconds; $dt = [math]::Max(0.2, $now - $last); $last = $now
  $io = @{}
  foreach ($w in (Get-CimInstance Win32_Process -Property ProcessId,ReadTransferCount,WriteTransferCount,ExecutablePath -ErrorAction SilentlyContinue)) {
    $io[[int]$w.ProcessId] = @([double]$w.ReadTransferCount + [double]$w.WriteTransferCount, [string]$w.ExecutablePath) }
  $newSig = 0
  $rows = foreach ($p in (Get-Process -ErrorAction SilentlyContinue)) {
    if ($p.Id -eq 0) { continue }
    $cpu = 0.0; try { $t = $p.TotalProcessorTime.TotalSeconds; if ($prevCpu.ContainsKey($p.Id)) { $cpu = 100 * ($t - $prevCpu[$p.Id]) / $dt / $cores }; $prevCpu[$p.Id] = $t } catch {}
    $x = $io[$p.Id]; $iob = 0.0; $path = $null
    if ($x) { $path = $x[1]; if ($prevIo.ContainsKey($p.Id)) { $iob = ($x[0] - $prevIo[$p.Id]) / $dt }; $prevIo[$p.Id] = $x[0] }
    $company = $null; $desc = $null
    if ($path) {
      if (-not $info.ContainsKey($path)) { try { $vi = [Diagnostics.FileVersionInfo]::GetVersionInfo($path); $info[$path] = @($vi.CompanyName, $vi.FileDescription) } catch { $info[$path] = @($null, $null) } }
      $company = $info[$path][0]; $desc = $info[$path][1]
      if (-not $sig.ContainsKey($path) -and $newSig -lt 6) { $newSig++
        $lp = $path.ToLower()
        if ($lp.StartsWith($win)) { $sig[$path] = 'Valid' } else { try { $sig[$path] = [string](Get-AuthenticodeSignature -LiteralPath $path -ErrorAction Stop).Status } catch { $sig[$path] = 'Unknown' } } }
    }
    $pri = $null; try { $pri = [string]$p.PriorityClass } catch {}
    [pscustomobject]@{ pid=$p.Id; name=$p.ProcessName; cpu=[math]::Round([math]::Max(0,$cpu),1); mem=[math]::Round($p.WorkingSet64/1MB,1); io=[math]::Round([math]::Max(0,$iob)); threads=$p.Threads.Count; path=$path; company=$company; desc=$desc; title=$p.MainWindowTitle; sig=$(if ($path) { $sig[$path] } else { $null }); pri=$pri }
  }
  [Console]::Out.WriteLine((ConvertTo-Json -InputObject @{ t=[DateTime]::Now.ToString('s'); procs=@($rows) } -Depth 4 -Compress))
  [Console]::Out.Flush()
  Start-Sleep -Milliseconds 1800
}`;

function startProcessStream(onData, onExit) { return stream(LOOP, undefined, onData, onExit); }

const PRIORITIES = ['Idle', 'BelowNormal', 'Normal', 'AboveNormal', 'High'];
async function setPriority({ pid, priority }) {
  if (!PRIORITIES.includes(priority)) throw new Error('Invalid priority');
  return ps('(Get-Process -Id ([int]$a.pid) -ErrorAction Stop).PriorityClass = $a.p; Out-Json @{ ok=$true }', { pid, p: priority });
}

module.exports = { startProcessStream, setPriority, PRIORITIES };
