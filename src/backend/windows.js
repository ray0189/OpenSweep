'use strict';
// Windows backend: every system operation is a PowerShell 5.1 script (built into
// Windows 10/11). Scripts are written to a temp .ps1 and run with -File, so there is
// no command-line length limit and no shell-quoting of user data: arguments are
// embedded as a JSON here-string and parsed with ConvertFrom-Json.

const { execFile } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const C = require('./catalog');

const PS = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
const PRELUDE = `$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.Encoding]::UTF8
function Out-Json($x){ ConvertTo-Json -InputObject $x -Depth 6 -Compress }
`;

function tmpFile(ext) {
  return path.join(os.tmpdir(), `opensweep-${crypto.randomBytes(8).toString('hex')}${ext}`);
}
function withArgs(script, args) {
  if (args === undefined) return PRELUDE + script;
  const json = JSON.stringify(args).replace(/'@/g, "'\u0040"); // cannot terminate the here-string
  return `${PRELUDE}$a = @'\n${json}\n'@ | ConvertFrom-Json\n${script}`;
}
function writeScript(text) {
  const f = tmpFile('.ps1');
  fs.writeFileSync(f, '\ufeff' + text, 'utf8'); // BOM so PS 5.1 reads UTF-8
  return f;
}

// Run a script as the current user; resolves with parsed JSON (or raw text).
function ps(script, args, { timeout = 120000, json = true } = {}) {
  const file = writeScript(withArgs(script, args));
  return new Promise((resolve, reject) => {
    execFile(PS, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', file],
      { timeout, maxBuffer: 64 * 1024 * 1024, windowsHide: true },
      (err, stdout, stderr) => {
        fs.rm(file, { force: true }, () => {});
        if (err && !stdout) return reject(new Error((stderr || err.message).trim().split('\n').slice(-3).join(' ')));
        const out = stdout.trim();
        if (!json) return resolve(out);
        if (!out) return resolve(null);
        try { resolve(JSON.parse(out.slice(out.search(/[[{"0-9tfn-]/)))); }
        catch { reject(new Error('Could not parse PowerShell output: ' + out.slice(0, 200))); }
      });
  });
}

// Is this process already elevated? (The installed app asks for admin at launch, so
// normally yes; then admin work runs directly, errors come back, and there is no UAC prompt.)
let adminP = null;
function isAdmin() {
  if (!adminP) adminP = ps('Out-Json ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)')
    .then(Boolean).catch(() => false);
  return adminP;
}

// Run a script with admin rights. Scripts report their result with Out-Result.
// Already admin: runs in-process and returns the result (or throws the real error).
// Not admin: UAC prompt; with wait, resolves with the script's result.
// visible=true opens a console the user can watch (not awaited).
async function elevated(script, args, { wait = true, visible = false, timeout = 30 * 60 * 1000 } = {}) {
  const admin = await isAdmin();
  if (admin && !visible) {
    const r = await ps(`function Out-Result($x){ Out-Json $x }\n${script}`, args, { timeout });
    return r === null || r === undefined ? { ok: true } : r;
  }
  const resultFile = tmpFile('.json');
  const body = withArgs(`function Out-Result($x){ Out-Json $x | Out-File -LiteralPath '${resultFile}' -Encoding utf8 }\n${script}`, args);
  const inner = writeScript(visible ? `${body}\nWrite-Host ''\nWrite-Host 'Finished. You can close this window.' -ForegroundColor Green` : body);
  // Start-Process joins -ArgumentList with spaces, so the file path must be quoted (user names can contain spaces).
  const argList = ['-NoProfile', '-ExecutionPolicy', 'Bypass'].concat(visible ? ['-NoExit'] : []).concat(['-File', `"${inner}"`]);
  const launcher = `$p = Start-Process -FilePath '${PS}' ${admin ? '' : '-Verb RunAs'} -WindowStyle ${visible ? 'Normal' : 'Hidden'} -PassThru ${wait && !visible ? '-Wait' : ''} -ArgumentList @(${argList.map(x => `'${x.replace(/'/g, "''")}'`).join(',')})
Out-Json @{ ok = [bool]$p }`;
  try {
    await ps(launcher, undefined, { timeout });
  } catch (e) {
    fs.rm(inner, { force: true }, () => {});
    if (/canceled|cancelled/i.test(e.message)) throw new Error('Administrator permission was declined.');
    throw e;
  }
  if (!wait || visible) { setTimeout(() => fs.rm(inner, { force: true }, () => {}), 60000); return { ok: true }; }
  fs.rm(inner, { force: true }, () => {});
  let text = '';
  try { text = fs.readFileSync(resultFile, 'utf8').replace(/^\ufeff/, '').trim(); fs.rm(resultFile, { force: true }, () => {}); }
  catch { throw new Error('The admin step did not finish (no result was written). Try running OpenSweep as administrator.'); }
  return text ? JSON.parse(text) : { ok: true };
}

// ---------------------------------------------------------------- system
async function systemInfo() {
  return ps(`
$os = Get-CimInstance Win32_OperatingSystem
$cs = Get-CimInstance Win32_ComputerSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name })
$disks = @(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' | ForEach-Object { [pscustomobject]@{ name=$_.DeviceID; label=$_.VolumeName; total=[double]$_.Size; free=[double]$_.FreeSpace; fs=$_.FileSystem } })
$phys = @(Get-PhysicalDisk -ErrorAction SilentlyContinue | ForEach-Object { [pscustomobject]@{ name=$_.FriendlyName; media=[string]$_.MediaType; bus=[string]$_.BusType; health=[string]$_.HealthStatus; size=[double]$_.Size } })
$bat = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1
$battery = $null; if ($bat) { $battery = [pscustomobject]@{ charge=$bat.EstimatedChargeRemaining; status=$bat.BatteryStatus } }
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$dv = (Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion' -ErrorAction SilentlyContinue).DisplayVersion
$bios = Get-CimInstance Win32_BIOS -ErrorAction SilentlyContinue
$board = Get-CimInstance Win32_BaseBoard -ErrorAction SilentlyContinue
Out-Json ([pscustomobject]@{ os=$os.Caption; displayVersion=$dv; version=$os.Version; build=$os.BuildNumber; arch=$os.OSArchitecture;
  installDate=$os.InstallDate.ToString('s'); lastBoot=$os.LastBootUpTime.ToString('s'); manufacturer=$cs.Manufacturer; model=$cs.Model;
  computerName=$env:COMPUTERNAME; user=$env:USERNAME; cpu=$cpu.Name.Trim(); cores=$cpu.NumberOfCores; threads=$cpu.NumberOfLogicalProcessors; cpuMHz=$cpu.MaxClockSpeed;
  ramTotal=[double]$cs.TotalPhysicalMemory; gpu=$gpu; disks=$disks; physicalDisks=$phys; battery=$battery; isAdmin=$admin;
  bios=("{0} {1}" -f $bios.Manufacturer, $bios.SMBIOSBIOSVersion); board=("{0} {1}" -f $board.Manufacturer, $board.Product) })`);
}

// ---------------------------------------------------------------- processes
async function processes() {
  return ps(`
$cores = [Environment]::ProcessorCount
$perf = @{}
Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -ErrorAction SilentlyContinue | ForEach-Object { $perf[[int]$_.IDProcess] = [double]$_.PercentProcessorTime }
Out-Json @(Get-Process | Where-Object { $_.Id -ne 0 } | ForEach-Object {
  [pscustomobject]@{ pid=$_.Id; name=$_.ProcessName; memMB=[math]::Round($_.WorkingSet64/1MB,1); cpu=[math]::Round(($perf[$_.Id])/$cores,1); title=$_.MainWindowTitle; path=$_.Path; company=$_.Company }
})`);
}
async function killProcess({ pid }) {
  return ps(`Stop-Process -Id ([int]$a.pid) -Force -ErrorAction Stop; Out-Json @{ ok=$true }`, { pid });
}

// ---------------------------------------------------------------- boost
async function boost() {
  return ps(`
Add-Type -Namespace OpenSweep -Name Mem -MemberDefinition '[DllImport("psapi.dll")] public static extern bool EmptyWorkingSet(System.IntPtr h);' -ErrorAction SilentlyContinue
$before = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory
$n = 0
Get-Process | ForEach-Object { try { if ([OpenSweep.Mem]::EmptyWorkingSet($_.Handle)) { $n++ } } catch {} }
Start-Sleep -Milliseconds 700
$after = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory
# temp files older than a day
$freed = 0
Get-ChildItem -LiteralPath $env:TEMP -Recurse -Force -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-1) } | ForEach-Object { $l=$_.Length; try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop; $freed += $l } catch {} }
Clear-DnsClientCache -ErrorAction SilentlyContinue
Out-Json ([pscustomobject]@{ memFreedMB=[math]::Max(0,[math]::Round(($after-$before)/1024)); processesTrimmed=$n; tempFreed=[double]$freed })`);
}

// ---------------------------------------------------------------- cleanup
const CLEAN_SCAN = `
function Get-Targets($paths) {
  foreach ($p in $paths) { $x = [Environment]::ExpandEnvironmentVariables($p); Get-Item -Path $x -Force -ErrorAction SilentlyContinue }
}
function Measure-Target($item) {
  if ($item.PSIsContainer) { $s=0; $c=0; Get-ChildItem -LiteralPath $item.FullName -Recurse -Force -File -ErrorAction SilentlyContinue | ForEach-Object { $s += $_.Length; $c++ }; return @($s,$c) }
  return @($item.Length, 1)
}
function Get-RecycleSize { $s=0; $c=0; try { (New-Object -ComObject Shell.Application).NameSpace(10).Items() | ForEach-Object { $s += [double]$_.ExtendedProperty('Size'); $c++ } } catch {}; return @($s,$c) }
`;
async function cleanupScan({ ids } = {}) {
  const cats = C.CLEANUP.filter(c => !ids || ids.includes(c.id));
  return ps(`${CLEAN_SCAN}
Out-Json @(foreach ($c in $a) {
  if ($c.special -eq 'recycle') { $m = Get-RecycleSize } else { $s=0; $n=0; foreach ($t in (Get-Targets $c.paths)) { $r = Measure-Target $t; $s += $r[0]; $n += $r[1] }; $m=@($s,$n) }
  [pscustomobject]@{ id=$c.id; size=[double]$m[0]; files=[int]$m[1] }
})`, cats.map(c => ({ id: c.id, paths: c.paths || [], special: c.special || null })), { timeout: 10 * 60 * 1000 });
}
const CLEAN_RUN = `${CLEAN_SCAN}
$res = @(foreach ($c in $a) {
  $freed = 0; $failed = 0
  if ($c.special -eq 'recycle') { $m = Get-RecycleSize; try { Clear-RecycleBin -Force -ErrorAction Stop; $freed = $m[0] } catch { $failed++ } }
  else {
    foreach ($t in (Get-Targets $c.paths)) {
      if ($t.PSIsContainer) {
        Get-ChildItem -LiteralPath $t.FullName -Recurse -Force -File -ErrorAction SilentlyContinue | ForEach-Object { $l=$_.Length; try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop; $freed += $l } catch { $failed++ } }
        Get-ChildItem -LiteralPath $t.FullName -Recurse -Force -Directory -ErrorAction SilentlyContinue | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
      } else { $l=$t.Length; try { Remove-Item -LiteralPath $t.FullName -Force -ErrorAction Stop; $freed += $l } catch { $failed++ } }
    }
  }
  [pscustomobject]@{ id=$c.id; freed=[double]$freed; skipped=$failed }
})`;
async function cleanupRun({ ids, elevate }) {
  const cats = C.CLEANUP.filter(c => ids.includes(c.id)).map(c => ({ id: c.id, paths: c.paths || [], special: c.special || null, admin: !!c.admin }));
  const user = cats.filter(c => !c.admin || !elevate);
  const admin = elevate ? cats.filter(c => c.admin) : [];
  const out = [];
  if (user.length) out.push(...(await ps(`${CLEAN_RUN}\nOut-Json $res`, user, { timeout: 30 * 60 * 1000 })) || []);
  if (admin.length) out.push(...(await elevated(`${CLEAN_RUN}\nOut-Result $res`, admin)) || []);
  return out;
}

// ---------------------------------------------------------------- storage
async function largeFiles({ root, minMB = 100, limit = 200 }) {
  return ps(`
Out-Json @(Get-ChildItem -LiteralPath $a.root -Recurse -File -Force -ErrorAction SilentlyContinue |
  Where-Object { $_.Length -ge ($a.minMB * 1MB) -and -not ($_.Attributes -band [IO.FileAttributes]::System) } |
  Sort-Object Length -Descending | Select-Object -First $a.limit |
  ForEach-Object { [pscustomobject]@{ path=$_.FullName; size=[double]$_.Length; modified=$_.LastWriteTime.ToString('s'); accessed=$_.LastAccessTime.ToString('s'); ext=$_.Extension.ToLower() } })`,
  { root, minMB, limit }, { timeout: 30 * 60 * 1000 });
}
async function duplicates({ root, minKB = 256 }) {
  return ps(`
$files = Get-ChildItem -LiteralPath $a.root -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Length -ge ($a.minKB * 1KB) }
$groups = $files | Group-Object Length | Where-Object Count -gt 1
Out-Json @(foreach ($g in $groups) {
  $g.Group | ForEach-Object { $h = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash; if ($h) { [pscustomobject]@{ f=$_; h=$h } } } |
    Group-Object h | Where-Object Count -gt 1 | ForEach-Object {
      [pscustomobject]@{ hash=$_.Name; size=[double]$_.Group[0].f.Length; files=@($_.Group | Sort-Object { $_.f.LastWriteTime } | ForEach-Object { [pscustomobject]@{ path=$_.f.FullName; modified=$_.f.LastWriteTime.ToString('s') } }) }
    }
})`, { root, minKB }, { timeout: 60 * 60 * 1000 });
}
async function folderSizes({ root }) {
  return ps(`
Out-Json @(Get-ChildItem -LiteralPath $a.root -Force -Directory -ErrorAction SilentlyContinue | Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | ForEach-Object {
  $s = (Get-ChildItem -LiteralPath $_.FullName -Recurse -Force -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
  [pscustomobject]@{ path=$_.FullName; name=$_.Name; size=[double]$s }
} | Sort-Object size -Descending)`, { root }, { timeout: 30 * 60 * 1000 });
}

// ---------------------------------------------------------------- apps
async function installedApps() {
  return ps(`
$keys = 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*','HKLM:\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*','HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*'
$win32 = @(Get-ItemProperty $keys -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -and $_.UninstallString -and -not $_.SystemComponent -and -not $_.ParentKeyName -and -not $_.ReleaseType } | ForEach-Object {
  [pscustomobject]@{ id=('w:'+$_.PSPath); kind='desktop'; name=$_.DisplayName; publisher=$_.Publisher; version=$_.DisplayVersion; sizeKB=[double]$_.EstimatedSize; installDate=$_.InstallDate; uninstall=$_.UninstallString; quiet=$_.QuietUninstallString; location=$_.InstallLocation } })
$store = @(Get-AppxPackage -ErrorAction SilentlyContinue | Where-Object { -not $_.IsFramework -and $_.SignatureKind -ne 'System' -and -not $_.NonRemovable } | ForEach-Object {
  [pscustomobject]@{ id=('s:'+$_.PackageFullName); kind='store'; pkg=$_.Name; name=$_.Name; publisher=$_.Publisher; version=[string]$_.Version; sizeKB=$null; installDate=$null; location=$_.InstallLocation } })
Out-Json ($win32 + $store)`);
}
async function uninstallApp(app) {
  if (app.kind === 'store') {
    return ps(`Get-AppxPackage -Name $a.pkg | Remove-AppxPackage -ErrorAction Stop; Out-Json @{ ok=$true }`, { pkg: app.pkg });
  }
  // Launch the vendor's own uninstaller (interactive). Uninstall strings come from the
  // registry entry the main process cached — never from the renderer.
  return ps(`
$u = $a.cmd
if ($u -match '(?i)msiexec' -and $u -match '(\\{[0-9A-F-]+\\})') { Start-Process msiexec.exe -ArgumentList '/x', $Matches[1] }
else { Start-Process cmd.exe -ArgumentList '/c', ('"' + $u + '"') -WindowStyle Hidden }
Out-Json @{ ok=$true; interactive=$true }`, { cmd: app.uninstall });
}

// ---------------------------------------------------------------- startup
const SA = 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved';
const SAM = 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved';
async function startupItems() {
  return ps(`
function Is-Enabled($ap, $name) { $v = (Get-ItemProperty -LiteralPath $ap -Name $name -ErrorAction SilentlyContinue).$name; if ($v -and ($v[0] -band 1)) { return $false }; return $true }
$items = @()
$runs = @(
  @{ src='HKCU'; path='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run'; ap='${SA}\\Run' },
  @{ src='HKLM'; path='HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run'; ap='${SAM}\\Run' },
  @{ src='HKLM32'; path='HKLM:\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run'; ap='${SAM}\\Run32' })
foreach ($r in $runs) {
  $p = Get-ItemProperty -LiteralPath $r.path -ErrorAction SilentlyContinue
  if ($p) { $p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object {
    $items += [pscustomobject]@{ id=($r.src+'|'+$_.Name); source=$r.src; name=$_.Name; command=[string]$_.Value; enabled=(Is-Enabled $r.ap $_.Name); ap=$r.ap; admin=($r.src -ne 'HKCU') } } }
}
$folders = @(@{ src='UserFolder'; dir=[Environment]::GetFolderPath('Startup'); ap='${SA}\\StartupFolder' }, @{ src='CommonFolder'; dir=[Environment]::GetFolderPath('CommonStartup'); ap='${SAM}\\StartupFolder' })
foreach ($f in $folders) {
  Get-ChildItem -LiteralPath $f.dir -File -ErrorAction SilentlyContinue | Where-Object Name -ne 'desktop.ini' | ForEach-Object {
    $items += [pscustomobject]@{ id=($f.src+'|'+$_.Name); source=$f.src; name=$_.BaseName; valueName=$_.Name; command=$_.FullName; enabled=(Is-Enabled $f.ap $_.Name); ap=$f.ap; admin=($f.src -eq 'CommonFolder') } }
}
$st = @(Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskPath -notlike '\\Microsoft\\*' -and ($_.Triggers | Where-Object { $_.CimClass.CimClassName -match 'Logon|Boot' }) } | ForEach-Object {
  [pscustomobject]@{ id=('Task|'+$_.TaskPath+$_.TaskName); source='Task'; name=$_.TaskName; taskPath=$_.TaskPath; command=(($_.Actions | ForEach-Object { $_.Execute }) -join '; '); enabled=($_.State -ne 'Disabled'); admin=$true } })
Out-Json ($items + $st)`);
}
async function setStartup(item, enabled) {
  if (item.source === 'Task') {
    const s = `${enabled ? 'Enable' : 'Disable'}-ScheduledTask -TaskPath $a.p -TaskName $a.n | Out-Null; Out-Result @{ ok=$true }`;
    return elevated(s, { p: item.taskPath, n: item.name });
  }
  const s = `
$name = $a.name
if (-not (Test-Path -LiteralPath $a.ap)) { New-Item -Path $a.ap -Force | Out-Null }
$bytes = [byte[]](@([byte]$(if ($a.en) { 2 } else { 3 })) + (,[byte]0 * 11))
New-ItemProperty -LiteralPath $a.ap -Name $name -Value $bytes -PropertyType Binary -Force | Out-Null`;
  const args = { ap: item.ap, name: item.valueName || item.name, en: enabled };
  if (item.admin) return elevated(s + '\nOut-Result @{ ok=$true }', args);
  return ps(s + '\nOut-Json @{ ok=$true }', args);
}

// ---------------------------------------------------------------- security
async function security() {
  return ps(`
$mp = Get-MpComputerStatus -ErrorAction SilentlyContinue
$def = $null
if ($mp) { $def = [pscustomobject]@{ realtime=$mp.RealTimeProtectionEnabled; antivirus=$mp.AntivirusEnabled; signatureAge=$mp.AntivirusSignatureAge; signatureVersion=$mp.AntivirusSignatureVersion; quickScanAge=$mp.QuickScanAge; fullScanAge=$mp.FullScanAge; tamper=$mp.IsTamperProtected; behavior=$mp.BehaviorMonitorEnabled; network=$mp.NISEnabled } }
$fw = @(Get-NetFirewallProfile -ErrorAction SilentlyContinue | ForEach-Object { [pscustomobject]@{ name=$_.Name; enabled=[bool]$_.Enabled } })
$uac = (Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System' -ErrorAction SilentlyContinue).EnableLUA
$hf = Get-HotFix -ErrorAction SilentlyContinue | Where-Object InstalledOn | Sort-Object InstalledOn -Descending | Select-Object -First 1
$av = @(Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction SilentlyContinue | ForEach-Object { $_.displayName })
$ss = (Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer' -ErrorAction SilentlyContinue).SmartScreenEnabled
$bl = $null; try { $bl = [string](Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction Stop).ProtectionStatus } catch {}
$rdp = (Get-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server' -ErrorAction SilentlyContinue).fDenyTSConnections
$smb1 = $null; try { $smb1 = (Get-SmbServerConfiguration -ErrorAction Stop).EnableSMB1Protocol } catch {}
Out-Json ([pscustomobject]@{ defender=$def; firewall=$fw; uac=($uac -eq 1); lastUpdate=$(if ($hf) { $hf.InstalledOn.ToString('s') } else { $null }); antivirus=$av; smartscreen=$ss; bitlocker=$bl; rdpEnabled=($rdp -eq 0); smb1=$smb1 })`);
}
async function defenderAction({ action }) {
  const map = {
    quick: 'Start-MpScan -ScanType QuickScan',
    full: 'Start-MpScan -ScanType FullScan',
    update: 'Update-MpSignature',
  };
  if (action === 'firewall-on') return elevated('Set-NetFirewallProfile -Profile Domain,Public,Private -Enabled True; Out-Result @{ ok=$true }');
  if (action === 'smb1-off') return elevated('Set-SmbServerConfiguration -EnableSMB1Protocol $false -Force; Out-Result @{ ok=$true }');
  if (action === 'rdp-off') return elevated("Set-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server' -Name fDenyTSConnections -Value 1; Out-Result @{ ok=$true }");
  if (!map[action]) throw new Error('Unknown action');
  return ps(`${map[action]} -ErrorAction Stop; Out-Json @{ ok=$true }`, undefined, { timeout: 4 * 60 * 60 * 1000 });
}
async function threats() {
  return ps(`Out-Json @(Get-MpThreatDetection -ErrorAction SilentlyContinue | Sort-Object InitialDetectionTime -Descending | Select-Object -First 50 | ForEach-Object {
  $t = Get-MpThreat -ThreatID $_.ThreatID -ErrorAction SilentlyContinue
  [pscustomobject]@{ name=$t.ThreatName; severity=$t.SeverityID; when=$_.InitialDetectionTime.ToString('s'); resources=@($_.Resources); action=$_.ActionSuccess } })`);
}

// ---------------------------------------------------------------- network
async function network() {
  return ps(`
Out-Json @(Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object { $_.NetAdapter.Status -eq 'Up' } | ForEach-Object {
  [pscustomobject]@{ index=$_.InterfaceIndex; name=$_.InterfaceAlias; desc=$_.InterfaceDescription; ipv4=@($_.IPv4Address | ForEach-Object IPAddress); gateway=@($_.IPv4DefaultGateway | ForEach-Object NextHop);
    dns=@($_.DNSServer | Where-Object AddressFamily -eq 2 | ForEach-Object ServerAddresses); speed=[string]$_.NetAdapter.LinkSpeed; mac=$_.NetAdapter.MacAddress } })`);
}
async function networkAction({ action, index, servers }) {
  switch (action) {
    case 'flushdns': return ps('Clear-DnsClientCache; Out-Json @{ ok=$true }');
    case 'renew': return ps('ipconfig /release | Out-Null; ipconfig /renew | Out-Null; Out-Json @{ ok=$true }', undefined, { timeout: 120000 });
    case 'hosts': return ps(`Start-Process notepad.exe -Verb RunAs -ArgumentList "$env:WINDIR\\System32\\drivers\\etc\\hosts"; Out-Json @{ ok=$true }`);
    case 'dns':
      return elevated(`if ($a.servers.Count -eq 0) { Set-DnsClientServerAddress -InterfaceIndex $a.index -ResetServerAddresses } else { Set-DnsClientServerAddress -InterfaceIndex $a.index -ServerAddresses $a.servers }
Clear-DnsClientCache; Out-Result @{ ok=$true }`, { index, servers });
    default: throw new Error('Unknown network action');
  }
}

// ---------------------------------------------------------------- tweaks (registry)
const toU32 = v => (typeof v === 'number' ? v >>> 0 : v);
const toS32 = v => (typeof v === 'number' && v > 0x7fffffff ? v - 0x100000000 : v);

async function tweakStates() {
  const reads = [];
  C.TWEAKS.forEach(t => t.values.forEach(v => reads.push({ i: reads.length, key: v.key, name: v.name || '', kind: v.kind || 'value' })));
  const res = await ps(`
Out-Json @(foreach ($q in $a) { $p = 'Registry::' + $q.key
  if ($q.kind -eq 'key' -or $q.kind -eq 'absent') { [pscustomobject]@{ i=$q.i; e=(Test-Path -LiteralPath $p) } }
  else { $v = $null; try { $v = (Get-ItemProperty -LiteralPath $p -Name $q.name -ErrorAction Stop).($q.name) } catch {}; [pscustomobject]@{ i=$q.i; v=$v } } })`, reads);
  const byI = new Map((res || []).map(r => [r.i, r]));
  const states = {};
  let i = 0;
  for (const t of C.TWEAKS) {
    states[t.id] = t.values.every(v => {
      const r = byI.get(i++) || {};
      if (v.kind === 'key') return !!r.e;
      if (v.kind === 'absent') return !r.e;
      if (r.v === null || r.v === undefined) return v.on === null;
      return String(toU32(r.v)) === String(toU32(v.on));
    });
  }
  return states;
}
const TWEAK_SET = `
$errs = @()
foreach ($o in $a) { $p = 'Registry::' + $o.key
  try {
    if ($o.kind -eq 'absent') { if ($o.on -and (Test-Path -LiteralPath $p)) { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction Stop } }
    elseif ($o.kind -eq 'key') {
      if ($o.on) { if (-not (Test-Path -LiteralPath $p)) { New-Item -Path $p -Force -ErrorAction Stop | Out-Null }; Set-Item -LiteralPath $p -Value '' -ErrorAction Stop }
      else { $rk = 'Registry::' + $o.removeKey; if (Test-Path -LiteralPath $rk) { Remove-Item -LiteralPath $rk -Recurse -Force -ErrorAction Stop } }
    } else {
      $val = $(if ($o.on) { $o.onValue } else { $o.offValue })
      if ($null -eq $val) { if (Test-Path -LiteralPath $p) { Remove-ItemProperty -LiteralPath $p -Name $o.name -Force -ErrorAction SilentlyContinue } }
      else {
        if (-not (Test-Path -LiteralPath $p)) { New-Item -Path $p -Force -ErrorAction Stop | Out-Null }
        if ($o.type -eq 'DWord') { $val = [int]$val } else { $val = [string]$val }
        New-ItemProperty -LiteralPath $p -Name $o.name -Value $val -PropertyType $o.type -Force -ErrorAction Stop | Out-Null
      }
    }
  } catch { $errs += [pscustomobject]@{ id=$o.id; error=$_.Exception.Message } }
}`;
const asList = x => (Array.isArray(x) ? x : x && typeof x === 'object' && x.id ? [x] : []);

// Applies the changes, then reads every value back so the UI reports what really applied.
async function setTweaks(changes) {
  const ops = [];
  let restartExplorer = false;
  for (const { id, on } of changes) {
    const t = C.TWEAKS.find(x => x.id === id);
    if (!t) continue;
    if (t.explorer) restartExplorer = true;
    for (const v of t.values) ops.push({ id, key: v.key, name: v.name, type: v.type, kind: v.kind || 'value', removeKey: v.removeKey, on: !!on, onValue: toS32(v.on), offValue: toS32(v.off) });
  }
  const user = ops.filter(o => !o.key.startsWith('HKEY_LOCAL_MACHINE'));
  const machine = ops.filter(o => o.key.startsWith('HKEY_LOCAL_MACHINE'));
  const errors = [];
  if (machine.length) {
    try { errors.push(...asList(await elevated(`${TWEAK_SET}\nOut-Result @($errs)`, machine))); }
    catch (e) { [...new Set(machine.map(o => o.id))].forEach(id => errors.push({ id, error: e.message })); }
  }
  if (user.length) errors.push(...asList(await ps(`${TWEAK_SET}\nOut-Json @($errs)`, user)));
  if (restartExplorer) await ps('Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 1500; if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }; Out-Json @{ ok=$true }');
  return verifyTweaks(changes, errors);
}
async function verifyTweaks(changes, errors = []) {
  const states = await tweakStates();
  const failed = [];
  for (const { id, on } of changes) {
    if (!(id in states) || states[id] === !!on) continue;
    const e = errors.find(x => x.id === id);
    failed.push({ id, title: (C.TWEAKS.find(t => t.id === id) || {}).title || id, error: e ? e.error : 'Value did not stick (Windows or a policy may be overriding it).' });
  }
  return { ok: !failed.length, applied: changes.length - failed.length, failed, states };
}

// ---------------------------------------------------------------- debloat / services / tasks
async function bloatStatus() {
  return ps(`Out-Json @(Get-AppxPackage -ErrorAction SilentlyContinue | ForEach-Object Name | Sort-Object -Unique)`);
}
async function removeApps({ pkgs, allUsers }) {
  const s = `
$res = @(foreach ($n in $a.pkgs) {
  $ok = $true
  try {
    if ($a.all) { Get-AppxPackage -AllUsers -Name $n | Remove-AppxPackage -AllUsers -ErrorAction Stop
      Get-AppxProvisionedPackage -Online | Where-Object DisplayName -eq $n | Remove-AppxProvisionedPackage -Online -ErrorAction SilentlyContinue | Out-Null }
    else { Get-AppxPackage -Name $n | Remove-AppxPackage -ErrorAction Stop }
  } catch { $ok = $false }
  [pscustomobject]@{ pkg=$n; ok=$ok } })`;
  if (allUsers) return elevated(s + '\nOut-Result $res', { pkgs, all: true });
  return ps(s + '\nOut-Json $res', { pkgs, all: false }, { timeout: 15 * 60 * 1000 });
}
async function servicesStatus() {
  return ps(`Out-Json @(foreach ($n in $a) { $s = Get-Service -Name $n -ErrorAction SilentlyContinue
  if ($s) { [pscustomobject]@{ name=$n; status=[string]$s.Status; startType=[string]$s.StartType } } else { [pscustomobject]@{ name=$n; status='Missing'; startType=$null } } })`, C.SERVICES.map(s => s.name));
}
async function setServices(changes) {
  let res = [];
  try {
    res = await elevated(`
$res = @(foreach ($c in $a) { $ok=$true; $err=$null
  try { Set-Service -Name $c.name -StartupType $c.type -ErrorAction Stop; if ($c.type -eq 'Disabled') { Stop-Service -Name $c.name -Force -ErrorAction SilentlyContinue } }
  catch {
    # Some services refuse Set-Service; sc.exe / the registry Start value work for most of those.
    $start = @{ Automatic=2; Manual=3; Disabled=4 }[$c.type]
    try { Set-ItemProperty -LiteralPath ('HKLM:\\SYSTEM\\CurrentControlSet\\Services\\' + $c.name) -Name Start -Value $start -ErrorAction Stop } catch { $ok=$false; $err=$_.Exception.Message }
  }
  [pscustomobject]@{ name=$c.name; ok=$ok; error=$err } })
Out-Result $res`, changes);
  } catch (e) { res = changes.map(c => ({ name: c.name, ok: false, error: e.message })); }
  res = Array.isArray(res) ? res : [res];
  // Verify by reading the start type back.
  const now = await servicesStatus();
  return changes.map(c => {
    const r = res.find(x => x && x.name === c.name) || {};
    const st = (now || []).find(x => x.name === c.name) || {};
    const ok = st.startType === c.type;
    return { name: c.name, ok, error: ok ? null : (r.error || `Still ${st.startType || 'unknown'} (Windows may protect this service).`) };
  });
}
async function tasksStatus() {
  return ps(`Out-Json @(foreach ($t in $a) { $x = Get-ScheduledTask -TaskPath $t.path -TaskName $t.name -ErrorAction SilentlyContinue
  [pscustomobject]@{ id=($t.path+$t.name); state=$(if ($x) { [string]$x.State } else { 'Missing' }) } })`, C.SCHEDULED_TASKS.map(t => ({ path: t.path, name: t.name })));
}
async function setTasks(changes) {
  let err = null;
  try {
    await elevated(`
foreach ($c in $a) { if ($c.enable) { Enable-ScheduledTask -TaskPath $c.path -TaskName $c.name -ErrorAction SilentlyContinue | Out-Null } else { Disable-ScheduledTask -TaskPath $c.path -TaskName $c.name -ErrorAction SilentlyContinue | Out-Null } }
Out-Result @{ ok=$true }`, changes);
  } catch (e) { err = e.message; }
  const now = await ps(`Out-Json @(foreach ($t in $a) { $x = Get-ScheduledTask -TaskPath $t.path -TaskName $t.name -ErrorAction SilentlyContinue
  [pscustomobject]@{ id=($t.path+$t.name); state=$(if ($x) { [string]$x.State } else { 'Missing' }) } })`, changes.map(c => ({ path: c.path, name: c.name })));
  const results = changes.map(c => {
    const st = ((Array.isArray(now) ? now : [now]).find(x => x && x.id === c.path + c.name) || {}).state;
    const ok = st === 'Missing' || (c.enable ? st !== 'Disabled' : st === 'Disabled');
    return { id: c.path + c.name, ok, error: ok ? null : (err || `Task is still ${st}.`) };
  });
  return { ok: results.every(r => r.ok), results };
}

// ---------------------------------------------------------------- drivers & stability
// Low-cost check meant to run in the background: problem devices from PnP, plus the
// System event log entries that matter for stability (blue screens, unexpected
// shutdowns, hardware/WHEA errors, display driver resets, disk errors).
async function stabilityCheck({ days = 7 } = {}) {
  return ps(`
try { (Get-Process -Id $PID).PriorityClass = 'Idle' } catch {}
$codes = @{ 1='Not configured correctly'; 3='Driver may be corrupted'; 10='Device cannot start'; 12='Not enough resources'; 14='Needs a restart'; 18='Reinstall the driver'; 19='Registry problem'; 21='Being removed'; 22='Disabled'; 24='Not present or not working'; 28='Driver not installed'; 31='Not working properly'; 32='Driver disabled'; 37='Driver failed to load'; 39='Driver missing or corrupted'; 41='Driver loaded but device not found'; 43='Device reported a problem'; 45='Not connected'; 48='Driver blocked'; 52='Driver signature problem' }
$devices = @(Get-CimInstance Win32_PnPEntity -Filter 'ConfigManagerErrorCode <> 0' -ErrorAction SilentlyContinue | Where-Object { $_.ConfigManagerErrorCode -notin 22,45 } | ForEach-Object {
  $c = [int]$_.ConfigManagerErrorCode
  [pscustomobject]@{ id=[string]$_.PNPDeviceID; name=$(if ($_.Name) { $_.Name } else { $_.PNPDeviceID }); cls=[string]$_.PNPClass; code=$c; problem=$(if ($codes[$c]) { $codes[$c] } else { "Error code $c" }) } })
$since = (Get-Date).AddDays(-[int]$a.days)
$filters = @(
  @{ LogName='System'; ProviderName='Microsoft-Windows-WER-SystemErrorReporting'; Id=1001 },
  @{ LogName='System'; ProviderName='Microsoft-Windows-Kernel-Power'; Id=41 },
  @{ LogName='System'; ProviderName='Microsoft-Windows-WHEA-Logger'; Level=1,2,3 },
  @{ LogName='System'; ProviderName='Display'; Id=4101 },
  @{ LogName='System'; ProviderName='disk'; Id=7,51,153 },
  @{ LogName='System'; ProviderName='Ntfs'; Id=55 })
$kinds = @{ 'Microsoft-Windows-WER-SystemErrorReporting'='Blue screen'; 'Microsoft-Windows-Kernel-Power'='Unexpected shutdown'; 'Microsoft-Windows-WHEA-Logger'='Hardware error'; 'Display'='Display driver reset'; 'disk'='Disk error'; 'Ntfs'='File system error' }
$events = @(foreach ($f in $filters) { $f.StartTime = $since
  Get-WinEvent -FilterHashtable $f -MaxEvents 20 -ErrorAction SilentlyContinue | ForEach-Object {
    $m = [string]$_.Message; if ($m.Length -gt 300) { $m = $m.Substring(0,300) + '...' }
    [pscustomobject]@{ key=("{0}|{1}|{2}" -f $_.ProviderName, $_.Id, $_.RecordId); when=$_.TimeCreated.ToString('s'); kind=$kinds[$f.ProviderName]; source=$_.ProviderName; eventId=$_.Id; message=$m } } })
$events = @($events | Sort-Object when -Descending)
$drivers = @(Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.DeviceName -and $_.DriverProviderName -and $_.DriverProviderName -ne 'Microsoft' })
$old = @($drivers | Where-Object { $_.DriverDate -and $_.DriverDate -lt (Get-Date).AddYears(-5) } | Sort-Object DriverDate | Select-Object -First 30 | ForEach-Object { [pscustomobject]@{ name=$_.DeviceName; provider=$_.DriverProviderName; version=$_.DriverVersion; date=$_.DriverDate.ToString('yyyy-MM-dd') } })
$unsigned = @($drivers | Where-Object { $_.IsSigned -eq $false } | ForEach-Object { [pscustomobject]@{ name=$_.DeviceName; provider=$_.DriverProviderName; version=$_.DriverVersion } })
Out-Json ([pscustomobject]@{ checkedAt=(Get-Date).ToString('s'); days=[int]$a.days; devices=$devices; events=$events; oldDrivers=$old; unsignedDrivers=$unsigned; thirdPartyDrivers=$drivers.Count })`, { days }, { timeout: 5 * 60 * 1000 });
}
async function scanDevices() {
  return elevated('pnputil /scan-devices | Out-Null; Out-Result @{ ok=$true }');
}

// ---------------------------------------------------------------- start with Windows (elevated app)
// Windows will not start an app that needs admin from the Run key, so a logon task
// with "highest privileges" is used instead.
async function setAutoStart({ enabled, exe }) {
  return elevated(`
$name = 'OpenSweep'
Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
if ($a.enabled) {
  $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
  $act = New-ScheduledTaskAction -Execute $a.exe -Argument '--hidden'
  $trg = New-ScheduledTaskTrigger -AtLogOn -User $user
  $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
  $pr = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
  Register-ScheduledTask -TaskName $name -Action $act -Trigger $trg -Settings $set -Principal $pr -Force -ErrorAction Stop | Out-Null
}
Out-Result @{ ok=$true; enabled=[bool](Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) }`, { enabled: !!enabled, exe });
}

// ---------------------------------------------------------------- Windows Update control
const UX = 'HKLM:\\SOFTWARE\\Microsoft\\WindowsUpdate\\UX\\Settings';
const WUPOL = 'HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate';
async function updatesStatus() {
  return ps(`
$u = Get-ItemProperty '${UX}' -ErrorAction SilentlyContinue
$pol = Get-ItemProperty '${WUPOL}' -ErrorAction SilentlyContinue
$cv = Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion'
$hf = @(Get-HotFix -ErrorAction SilentlyContinue | Where-Object InstalledOn | Sort-Object InstalledOn -Descending | Select-Object -First 15 | ForEach-Object { [pscustomobject]@{ kb=$_.HotFixID; desc=$_.Description; installed=$_.InstalledOn.ToString('s') } })
$reboot = Test-Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired'
Out-Json ([pscustomobject]@{ pausedUntil=$u.PauseUpdatesExpiryTime; activeStart=$u.ActiveHoursStart; activeEnd=$u.ActiveHoursEnd;
  lockedVersion=$(if ($pol.TargetReleaseVersion -eq 1) { $pol.TargetReleaseVersionInfo } else { $null });
  displayVersion=$cv.DisplayVersion; product=$(if ([int]$cv.CurrentBuild -ge 22000) { 'Windows 11' } else { 'Windows 10' }); hotfixes=$hf; rebootPending=$reboot })`);
}
async function updatesAction({ action, days, start, end, kb, product, version }) {
  const ensure = `if (-not (Test-Path '${UX}')) { New-Item -Path '${UX}' -Force | Out-Null }`;
  switch (action) {
    case 'pause': return elevated(`${ensure}
$now = (Get-Date).ToUniversalTime(); $end = $now.AddDays([int]$a.days); $f = 'yyyy-MM-ddTHH:mm:ssZ'
if ([int]$a.days -gt 35) { New-ItemProperty '${UX}' -Name FlightSettingsMaxPauseDays -Value ([int]$a.days) -PropertyType DWord -Force | Out-Null }
foreach ($n in 'PauseFeatureUpdatesStartTime','PauseQualityUpdatesStartTime','PauseUpdatesStartTime') { New-ItemProperty '${UX}' -Name $n -Value $now.ToString($f) -PropertyType String -Force | Out-Null }
foreach ($n in 'PauseFeatureUpdatesEndTime','PauseQualityUpdatesEndTime','PauseUpdatesExpiryTime') { New-ItemProperty '${UX}' -Name $n -Value $end.ToString($f) -PropertyType String -Force | Out-Null }
Out-Result @{ ok=$true }`, { days });
    case 'resume': return elevated(`foreach ($n in 'PauseFeatureUpdatesStartTime','PauseQualityUpdatesStartTime','PauseUpdatesStartTime','PauseFeatureUpdatesEndTime','PauseQualityUpdatesEndTime','PauseUpdatesExpiryTime') { Remove-ItemProperty '${UX}' -Name $n -ErrorAction SilentlyContinue }
Out-Result @{ ok=$true }`);
    case 'active-hours': return elevated(`${ensure}
New-ItemProperty '${UX}' -Name ActiveHoursStart -Value ([int]$a.start) -PropertyType DWord -Force | Out-Null
New-ItemProperty '${UX}' -Name ActiveHoursEnd -Value ([int]$a.end) -PropertyType DWord -Force | Out-Null
New-ItemProperty '${UX}' -Name SmartActiveHoursState -Value 0 -PropertyType DWord -Force | Out-Null
Out-Result @{ ok=$true }`, { start, end });
    case 'lock': return elevated(`if (-not (Test-Path '${WUPOL}')) { New-Item -Path '${WUPOL}' -Force | Out-Null }
New-ItemProperty '${WUPOL}' -Name TargetReleaseVersion -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty '${WUPOL}' -Name ProductVersion -Value $a.product -PropertyType String -Force | Out-Null
New-ItemProperty '${WUPOL}' -Name TargetReleaseVersionInfo -Value $a.version -PropertyType String -Force | Out-Null
Out-Result @{ ok=$true }`, { product, version });
    case 'unlock': return elevated(`foreach ($n in 'TargetReleaseVersion','ProductVersion','TargetReleaseVersionInfo') { Remove-ItemProperty '${WUPOL}' -Name $n -ErrorAction SilentlyContinue }
Out-Result @{ ok=$true }`);
    case 'uninstall': return ps(`Start-Process wusa.exe -ArgumentList '/uninstall', ('/kb:' + $a.kb.Substring(2)); Out-Json @{ ok=$true }`, { kb });
    case 'scan': return ps(`Start-Process 'ms-settings:windowsupdate-action'; Out-Json @{ ok=$true }`);
    default: throw new Error('Unknown update action');
  }
}

// ---------------------------------------------------------------- power
async function powerPlans() {
  const out = await ps('powercfg /list', undefined, { json: false });
  return out.split(/\r?\n/).map(l => l.match(/([0-9a-f-]{36})\s+\((.+?)\)\s*(\*)?/i)).filter(Boolean).map(m => ({ guid: m[1], name: m[2], active: !!m[3] }));
}
async function setPowerPlan({ guid }) {
  return ps('powercfg /setactive $a.guid; Out-Json @{ ok=$true }', { guid });
}

// ---------------------------------------------------------------- repair / launch
async function runRepair(r) {
  if (r.admin) return elevated(r.script, undefined, { wait: false, visible: true });
  return ps(`${r.script}\nOut-Json @{ ok=$true }`);
}
async function launchTool(t) {
  if (t.uri) return ps('Start-Process $a.uri; Out-Json @{ ok=$true }', { uri: t.uri });
  return ps('if ($a.args.Count) { Start-Process $a.cmd -ArgumentList $a.args } else { Start-Process $a.cmd }; Out-Json @{ ok=$true }', { cmd: t.cmd, args: t.args || [] });
}

module.exports = {
  demo: false,
  systemInfo, processes, killProcess, boost, cleanupScan, cleanupRun, largeFiles, duplicates, folderSizes,
  installedApps, uninstallApp, startupItems, setStartup, security, defenderAction, threats, network, networkAction,
  tweakStates, setTweaks, bloatStatus, removeApps, servicesStatus, setServices, tasksStatus, setTasks,
  powerPlans, setPowerPlan, runRepair, launchTool, updatesStatus, updatesAction,
  isAdmin, verifyTweaks, _ps: ps, stabilityCheck, scanDevices, setAutoStart,
};
