'use strict';
// Advanced uninstaller: programs list with last-used info, silent/batch uninstall,
// leftover scanning (files, registry, services, tasks, startup, shortcuts, firewall),
// leftover removal with backups, orphan finder, browser extensions, Windows optional
// features, install tracker (snapshot + diff), hunter mode and winget updates.
const fs = require('fs');
const os = require('os');
const path = require('path');
const { ps, elevated } = require('./pscore');

const list = (x) => (Array.isArray(x) ? x : x ? [x] : []);
const BACKUP_ROOT = () => path.join(process.env.ProgramData || 'C:\\ProgramData', 'OpenSweep', 'backups');
const TRACK_ROOT = () => path.join(process.env.ProgramData || 'C:\\ProgramData', 'OpenSweep', 'tracks');

// ---------------------------------------------------------------- names
const NEVER = new Set(['microsoft', 'windows', 'common files', 'intel', 'nvidia', 'amd', 'realtek', 'internet explorer', 'windowsapps', 'packages',
  'temp', 'programs', 'system32', 'microsoft shared', 'windows defender', 'windows nt', 'windows mail', 'windows media player', 'windows photo viewer',
  'reference assemblies', 'msbuild', 'dotnet', 'installer', 'package cache', 'microsoft.net', 'google', 'mozilla', 'apple', 'adobe', 'oracle', 'java',
  'crashdumps', 'comms', 'connecteddevicesplatform', 'd3dscache', 'publishers', 'virtualstore', 'history', 'cache', 'logs', 'update', 'updater', 'app', 'apps', 'data', 'tools', 'software', 'classes', 'policies', 'wow6432node']);
function cleanName(s) {
  return String(s || '').replace(/\(.*?\)|\[.*?\]/g, ' ').replace(/\b(v?\d+(\.\d+)+[a-z]?|x64|x86|64-bit|32-bit|amd64|arm64|version|update|setup)\b/gi, ' ')
    .replace(/[™®©]/g, '').replace(/\s+/g, ' ').trim();
}
function publisherKey(p) {
  return String(p || '').replace(/,?\s*\b(inc|llc|ltd|limited|corp|corporation|co|gmbh|s\.?a|b\.?v|ab|ag|software|technologies|plc)\b\.?/gi, '').replace(/[.,]+$/, '').replace(/\s+/g, ' ').trim();
}
function nameKeys(app) {
  const n = cleanName(app.name);
  const pub = publisherKey(app.publisher);
  const keys = new Set([n, n.replace(/\s+/g, '')]);
  if (pub && n.toLowerCase().startsWith(pub.toLowerCase() + ' ')) keys.add(n.slice(pub.length + 1));
  if (app.location) keys.add(app.location.replace(/[\\/]+$/, '').split(/[\\/]/).pop());
  return [...keys].filter(k => k && k.length >= 3 && !NEVER.has(k.toLowerCase()));
}

// ---------------------------------------------------------------- programs
async function programs() {
  const r = await ps(`
$pf = @{}
Get-ChildItem -LiteralPath (Join-Path $env:SystemRoot 'Prefetch') -Filter *.pf -ErrorAction SilentlyContinue | ForEach-Object {
  $n = ($_.Name -replace '-[0-9A-F]{8}\\.pf$','').ToLower(); if (-not $pf.ContainsKey($n) -or $pf[$n] -lt $_.LastWriteTime) { $pf[$n] = $_.LastWriteTime } }
function Last-Used($loc, $icon) {
  $best = $null; $exes = @()
  if ($loc -and (Test-Path -LiteralPath $loc)) { $exes = @(Get-ChildItem -LiteralPath $loc -Filter *.exe -Recurse -Depth 1 -ErrorAction SilentlyContinue | Select-Object -First 40 | ForEach-Object Name) }
  if ($icon) { $i = ($icon -split ',')[0].Trim('"'); if ($i -like '*.exe') { $exes += (Split-Path $i -Leaf) } }
  foreach ($e in $exes) { $k = $e.ToLower(); if ($k -match '^(unins|uninst|uninstall|update|helper|crash)') { continue }; if ($pf.ContainsKey($k) -and ($best -eq $null -or $pf[$k] -gt $best)) { $best = $pf[$k] } }
  if ($best) { return $best.ToString('s') } else { return $null }
}
$keys = 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*','HKLM:\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*','HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*'
$win32 = @(Get-ItemProperty $keys -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -and $_.UninstallString -and -not $_.SystemComponent -and -not $_.ParentKeyName -and -not $_.ReleaseType } | ForEach-Object {
  $rk = ($_.PSPath -replace '^Microsoft.PowerShell.Core\\\\Registry::','')
  [pscustomobject]@{ id=('w:'+$rk); kind='desktop'; name=$_.DisplayName; publisher=$_.Publisher; version=$_.DisplayVersion; sizeKB=[double]$_.EstimatedSize; installDate=$_.InstallDate;
    uninstall=$_.UninstallString; quiet=$_.QuietUninstallString; location=$_.InstallLocation; icon=$_.DisplayIcon; regKey=$rk; msi=[bool]$_.WindowsInstaller; lastUsed=(Last-Used $_.InstallLocation $_.DisplayIcon) } })
$store = @(Get-AppxPackage -ErrorAction SilentlyContinue | Where-Object { -not $_.IsFramework -and $_.SignatureKind -ne 'System' -and -not $_.NonRemovable } | ForEach-Object {
  $dn = $_.Name; try { $m = [xml](Get-Content -LiteralPath (Join-Path $_.InstallLocation 'AppxManifest.xml') -Raw -ErrorAction Stop); $d = $m.Package.Properties.DisplayName; if ($d -and $d -notlike 'ms-resource*') { $dn = $d } } catch {}
  [pscustomobject]@{ id=('s:'+$_.PackageFullName); kind='store'; pkg=$_.Name; name=$dn; publisher=($_.Publisher -replace '^CN=([^,]+).*','$1'); version=[string]$_.Version; sizeKB=$null; installDate=$null; location=$_.InstallLocation; lastUsed=$null } })
Out-Json ($win32 + $store)`, undefined, { timeout: 5 * 60 * 1000 });
  return list(r);
}

// Best silent uninstall command for a desktop app, or null when we can't tell.
function silentCommand(app) {
  if (app.quiet) return app.quiet;
  const u = String(app.uninstall || '');
  const guid = (u.match(/\{[0-9A-F-]{36}\}/i) || [])[0];
  if (/msiexec/i.test(u) && guid) return `MsiExec.exe /X${guid} /qn /norestart`;
  if (/unins\d{3}\.exe/i.test(u)) return `${u} /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`; // Inno Setup
  if (/uninst(all)?\.exe/i.test(u)) return `${u} /S`; // NSIS
  return null;
}

const RUN_UNINSTALL = `
function Split-Cmd($u) {
  if ($u -match '^\\s*"([^"]+)"\\s*(.*)$') { return @($Matches[1], $Matches[2]) }
  if ($u -match '^\\s*(.+?\\.exe)\\s*(.*)$') { return @($Matches[1], $Matches[2]) }
  return @($u, '')
}
$c = Split-Cmd $a.cmd
$sp = @{ FilePath=$c[0]; PassThru=$true }
if ($c[1]) { $sp.ArgumentList = $c[1] }
if ($a.silent) { $sp.WindowStyle = 'Hidden' }
$p = Start-Process @sp -ErrorAction Stop
if ($a.wait) {
  $p.WaitForExit(20 * 60 * 1000) | Out-Null
  # Many uninstallers hand over to a temp copy and exit; wait for the app to disappear.
  $t0 = Get-Date
  while ($a.regKey -and (Test-Path -LiteralPath ('Registry::' + $a.regKey)) -and ((Get-Date) - $t0).TotalSeconds -lt $a.grace) { Start-Sleep -Seconds 2 }
}
Out-Result @{ ok=$true; gone=$(if ($a.regKey) { -not (Test-Path -LiteralPath ('Registry::' + $a.regKey)) } else { $null }) }`;

async function runUninstall(app, { silent = false } = {}) {
  if (app.kind === 'store') {
    return ps('Get-AppxPackage -Name $a.pkg | Remove-AppxPackage -ErrorAction Stop; Out-Json @{ ok=$true; gone=$true }', { pkg: app.pkg });
  }
  const cmd = silent ? silentCommand(app) : (app.uninstall || '').replace(/MsiExec\.exe\s+\/I/i, 'MsiExec.exe /X');
  if (!cmd) throw new Error(`${app.name} has no silent uninstall option. Use a normal uninstall.`);
  return elevated(RUN_UNINSTALL, { cmd, silent, wait: silent, regKey: app.regKey || null, grace: silent ? 120 : 0 }, { timeout: 25 * 60 * 1000 });
}
async function stillInstalled(app) {
  if (app.kind === 'store') return ps('Out-Json ([bool](Get-AppxPackage -Name $a.pkg))', { pkg: app.pkg });
  return ps("Out-Json (Test-Path -LiteralPath ('Registry::' + $a.k))", { k: app.regKey });
}

// ---------------------------------------------------------------- leftovers
const LEFTOVER_SCAN = `
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
$items = New-Object System.Collections.ArrayList
$seen = @{}
function Add($type, $p, $level, $detail) { if (-not $p -or $seen.ContainsKey($type + $p)) { return }; $seen[$type + $p] = 1
  $size = 0; if ($type -eq 'dir') { $size = Get-DirSize $p 3000 } elseif ($type -eq 'file') { $size = [double](Get-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue).Length }
  [void]$items.Add([pscustomobject]@{ type=$type; path=$p; level=$level; detail=$detail; size=$size }) }
$names = @($a.keys | ForEach-Object { $_.ToLower() })
$pub = $(if ($a.pub) { $a.pub.ToLower() } else { $null })
$loc = $(if ($a.location) { $a.location.TrimEnd('\\') } else { $null })
$prot = @($env:SystemRoot, $env:ProgramFiles, \${env:ProgramFiles(x86)}, $env:ProgramData, $env:APPDATA, $env:LOCALAPPDATA, $env:USERPROFILE, (Join-Path $env:LOCALAPPDATA 'Programs')) | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\\').ToLower() }
function Safe-Path($p) { $l = $p.TrimEnd('\\').ToLower(); return ($l.Length -gt 6 -and $prot -notcontains $l -and -not $l.StartsWith($env:SystemRoot.ToLower())) }

# Files and folders
if ($loc -and (Test-Path -LiteralPath $loc) -and (Safe-Path $loc)) { Add 'dir' $loc 'safe' 'Install folder' }
$roots = @($env:ProgramFiles, \${env:ProgramFiles(x86)}, $env:ProgramData, $env:APPDATA, $env:LOCALAPPDATA, (Join-Path $env:LOCALAPPDATA 'Programs'), (Join-Path $env:USERPROFILE 'AppData\\LocalLow'),
  (Join-Path $env:APPDATA 'Microsoft\\Windows\\Start Menu\\Programs'), (Join-Path $env:ProgramData 'Microsoft\\Windows\\Start Menu\\Programs')) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
foreach ($r in $roots) {
  foreach ($d in (Get-ChildItem -LiteralPath $r -Directory -Force -ErrorAction SilentlyContinue)) {
    $n = $d.Name.ToLower()
    if ($names -contains $n) { Add 'dir' $d.FullName 'safe' 'Named after the app' }
    elseif ($pub -and $n -eq $pub) {
      $kids = @(Get-ChildItem -LiteralPath $d.FullName -Force -ErrorAction SilentlyContinue)
      $match = @($kids | Where-Object { $names -contains $_.Name.ToLower() })
      foreach ($m in $match) { Add 'dir' $m.FullName 'safe' "Inside the publisher's folder" }
      if ($match.Count -gt 0 -and $match.Count -eq $kids.Count) { Add 'dir' $d.FullName 'likely' "Publisher folder (only this app's files)" }
      elseif ($match.Count -eq 0 -and $kids.Count -le 3) { Add 'dir' $d.FullName 'careful' 'Publisher folder — may be shared with other apps' }
    }
  }
  foreach ($f in (Get-ChildItem -LiteralPath $r -Filter *.lnk -File -Force -ErrorAction SilentlyContinue)) { if ($names -contains $f.BaseName.ToLower()) { Add 'file' $f.FullName 'safe' 'Start menu shortcut' } }
}
# Desktop shortcuts pointing into the install folder or named after the app
$sh = New-Object -ComObject WScript.Shell
foreach ($dt in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('CommonDesktopDirectory'))) {
  foreach ($f in (Get-ChildItem -LiteralPath $dt -Filter *.lnk -File -Force -ErrorAction SilentlyContinue)) {
    $t = $null; try { $t = $sh.CreateShortcut($f.FullName).TargetPath } catch {}
    if (($loc -and $t -and $t.ToLower().StartsWith($loc.ToLower())) -or ($names -contains $f.BaseName.ToLower())) { Add 'file' $f.FullName 'safe' 'Desktop shortcut' } } }
# Registry
$regRoots = 'HKEY_CURRENT_USER\\Software','HKEY_LOCAL_MACHINE\\SOFTWARE','HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node'
foreach ($rr in $regRoots) {
  foreach ($k in (Get-ChildItem -LiteralPath ('Registry::' + $rr) -ErrorAction SilentlyContinue)) {
    $n = $k.PSChildName.ToLower(); $full = $rr + '\\' + $k.PSChildName
    if ($names -contains $n) { Add 'reg' $full 'safe' 'Settings key named after the app' }
    elseif ($pub -and $n -eq $pub) {
      $kids = @(Get-ChildItem -LiteralPath $k.PSPath -ErrorAction SilentlyContinue)
      $match = @($kids | Where-Object { $names -contains $_.PSChildName.ToLower() })
      foreach ($m in $match) { Add 'reg' ($full + '\\' + $m.PSChildName) 'safe' "Settings under the publisher's key" }
      if ($match.Count -gt 0 -and $match.Count -eq $kids.Count) { Add 'reg' $full 'likely' "Publisher key (only this app)" }
    }
  }
}
if ($a.regKey -and (Test-Path -LiteralPath ('Registry::' + $a.regKey))) { Add 'reg' $a.regKey 'safe' 'Uninstall entry left behind' }
# Startup entries
foreach ($rk in 'HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run','HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run','HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run') {
  $p = Get-ItemProperty -LiteralPath ('Registry::' + $rk) -ErrorAction SilentlyContinue
  if ($p) { foreach ($pp in ($p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' })) { $v = [string]$pp.Value
    if (($loc -and $v.ToLower().Contains($loc.ToLower())) -or ($names -contains $pp.Name.ToLower())) { Add 'run' ($rk + '|' + $pp.Name) 'safe' ('Starts with Windows: ' + $v) } } } }
# Services
foreach ($s in (Get-CimInstance Win32_Service -ErrorAction SilentlyContinue)) { $pn = [string]$s.PathName
  if (($loc -and $pn.ToLower().Contains($loc.ToLower())) -or ($names -contains $s.Name.ToLower())) { Add 'service' $s.Name 'likely' ("Service: " + $s.DisplayName) } }
# Scheduled tasks
foreach ($t in (Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskPath -notlike '\\Microsoft\\*' })) {
  $ex = (($t.Actions | ForEach-Object { $_.Execute }) -join ' ').ToLower()
  $hit = ($loc -and $ex.Contains($loc.ToLower()))
  if (-not $hit) { foreach ($n in $names) { if ($t.TaskName.ToLower().Contains($n)) { $hit = $true } } }
  if ($hit) { Add 'task' ($t.TaskPath + $t.TaskName) 'likely' ('Scheduled task: ' + $t.TaskName) } }
# Firewall rules for programs in the install folder
if ($loc) { foreach ($f in (Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue | Where-Object { $_.Program -and $_.Program.ToLower().StartsWith($loc.ToLower()) })) {
  $rule = $f | Get-NetFirewallRule -ErrorAction SilentlyContinue; if ($rule) { Add 'firewall' $rule.Name 'likely' ('Firewall rule: ' + $rule.DisplayName) } } }
Out-Json @($items)`;

async function leftoverScan(app) {
  const args = { keys: nameKeys(app), pub: publisherKey(app.publisher) || null, location: app.location || null, regKey: app.regKey || null };
  if (!args.keys.length && !args.location) return [];
  const r = await ps(LEFTOVER_SCAN, args, { timeout: 10 * 60 * 1000 });
  return list(r).map((x, i) => ({ ...x, id: `${x.type}:${i}:${x.path}` }));
}

// Remove non-file leftovers (registry, startup, services, tasks, firewall) with backups.
// Files and folders are moved to the Recycle Bin by the main process.
async function removeLeftovers(items, label) {
  const dir = path.join(BACKUP_ROOT(), `${new Date().toISOString().replace(/[:.]/g, '-')}-${String(label || 'leftovers').replace(/[^\w.-]+/g, '_').slice(0, 40)}`);
  const r = await elevated(`
New-Item -ItemType Directory -Path $a.dir -Force | Out-Null
$n = 0
$res = @(foreach ($i in $a.items) { $n++; $ok = $true; $err = $null
  try {
    switch ($i.type) {
      'reg' { & reg.exe export $i.path (Join-Path $a.dir "reg-$n.reg") /y | Out-Null; Remove-Item -LiteralPath ('Registry::' + $i.path) -Recurse -Force -ErrorAction Stop }
      'run' { $k, $v = $i.path -split '\\|', 2; & reg.exe export $k (Join-Path $a.dir "run-$n.reg") /y | Out-Null; Remove-ItemProperty -LiteralPath ('Registry::' + $k) -Name $v -Force -ErrorAction Stop }
      'service' { & reg.exe export ('HKLM\\SYSTEM\\CurrentControlSet\\Services\\' + $i.path) (Join-Path $a.dir "svc-$n.reg") /y | Out-Null; Stop-Service -Name $i.path -Force -ErrorAction SilentlyContinue; & sc.exe delete $i.path | Out-Null; if ($LASTEXITCODE -ne 0) { throw "sc delete failed ($LASTEXITCODE)" } }
      'task' { $tp = $i.path.Substring(0, $i.path.LastIndexOf('\\') + 1); $tn = $i.path.Substring($i.path.LastIndexOf('\\') + 1)
               Export-ScheduledTask -TaskPath $tp -TaskName $tn -ErrorAction SilentlyContinue | Out-File (Join-Path $a.dir "task-$n.xml") -Encoding unicode
               Unregister-ScheduledTask -TaskPath $tp -TaskName $tn -Confirm:$false -ErrorAction Stop }
      'firewall' { Get-NetFirewallRule -Name $i.path -ErrorAction SilentlyContinue | Select-Object Name,DisplayName,Direction,Action,Enabled | ConvertTo-Json | Out-File (Join-Path $a.dir "fw-$n.json") -Encoding utf8; Remove-NetFirewallRule -Name $i.path -ErrorAction Stop }
      default { throw 'Unsupported item' }
    }
  } catch { $ok = $false; $err = $_.Exception.Message }
  [pscustomobject]@{ id=$i.id; ok=$ok; error=$err } })
Out-Result @{ backup=$a.dir; results=$res }`, { dir, items: items.map(i => ({ id: i.id, type: i.type, path: i.path })) }, { timeout: 10 * 60 * 1000 });
  return { backup: r.backup || dir, results: list(r.results) };
}
async function restoreBackup(dir) {
  return elevated(`$res = @(Get-ChildItem -LiteralPath $a.dir -Filter *.reg -ErrorAction Stop | ForEach-Object { & reg.exe import $_.FullName 2>$null; [pscustomobject]@{ file=$_.Name; ok=($LASTEXITCODE -eq 0) } })
Get-ChildItem -LiteralPath $a.dir -Filter task-*.xml -ErrorAction SilentlyContinue | ForEach-Object { try { Register-ScheduledTask -Xml (Get-Content -LiteralPath $_.FullName -Raw) -TaskName ($_.BaseName) -ErrorAction Stop | Out-Null } catch {} }
Out-Result @{ ok=$true; restored=$res.Count }`, { dir });
}

// ---------------------------------------------------------------- orphans (leftovers of apps removed in the past)
const ORPHAN_ALLOW = ['microsoft', 'windows', 'packages', 'temp', 'programs', 'common files', 'internet explorer', 'windowsapps', 'modifiableswindowsapps', 'package cache', 'microsoft.net',
  'reference assemblies', 'windows defender', 'windows mail', 'windows media player', 'windows nt', 'windows photo viewer', 'windows sidebar', 'windowspowershell', 'uninstall information',
  'msbuild', 'dotnet', 'installer', 'crashdumps', 'comms', 'connecteddevicesplatform', 'd3dscache', 'publishers', 'virtualstore', 'ssh', 'regid.1991-06.com.microsoft', 'usoshared',
  'usoprivate', 'softwaredistribution', 'desktop.ini', 'application data', 'history', 'temporary internet files', 'microsoft help', 'nvidia', 'nvidia corporation', 'intel', 'amd', 'realtek',
  'opensweep', 'peernetworking', 'placeholdertilelogofolder', 'iconcache.db', 'fontcache', 'mozilla', 'google', 'packagemanagement', 'chocolatey', 'squirreltemp', 'deliveryoptimization'];
async function orphanScan(installed) {
  const tokens = new Set();
  for (const a of installed || []) {
    for (const k of nameKeys(a)) tokens.add(k.toLowerCase());
    const p = publisherKey(a.publisher).toLowerCase(); if (p) tokens.add(p);
    for (const w of cleanName(a.name).toLowerCase().split(' ')) if (w.length >= 4) tokens.add(w);
    if (a.location) for (const seg of a.location.toLowerCase().split(/[\\/]/)) if (seg.length >= 3) tokens.add(seg);
  }
  const r = await ps(`
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
$tok = $a.tokens; $allow = $a.allow
$roots = @($env:ProgramFiles, \${env:ProgramFiles(x86)}, $env:ProgramData, $env:APPDATA, $env:LOCALAPPDATA, (Join-Path $env:LOCALAPPDATA 'Programs')) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
Out-Json @(foreach ($r in $roots) { foreach ($d in (Get-ChildItem -LiteralPath $r -Directory -Force -ErrorAction SilentlyContinue)) {
  $n = $d.Name.ToLower()
  if ($allow -contains $n -or $n.StartsWith('{') -or $n.StartsWith('.')) { continue }
  $hit = $false; foreach ($t in $tok) { if ($n.Contains($t) -or $t.Contains($n)) { $hit = $true; break } }
  if ($hit) { continue }
  $last = $d.LastWriteTime
  try { foreach ($c in (Get-ChildItem -LiteralPath $d.FullName -Force -ErrorAction SilentlyContinue | Select-Object -First 200)) { if ($c.LastWriteTime -gt $last) { $last = $c.LastWriteTime } } } catch {}
  if (((Get-Date) - $last).TotalDays -lt 30) { continue }
  $s = Get-DirSize $d.FullName 1500
  if ($s -lt 1MB) { continue }
  [pscustomobject]@{ type='dir'; path=$d.FullName; size=[double]$s; level='careful'; detail=$(if ($last) { 'Last changed ' + $last.ToString('yyyy-MM-dd') } else { 'Empty' }); modified=$(if ($last) { $last.ToString('s') } else { $null }) } } })`,
  { tokens: [...tokens].filter(t => t.length >= 3), allow: ORPHAN_ALLOW }, { timeout: 15 * 60 * 1000 });
  return list(r).filter(x => !x.modified || Date.now() - new Date(x.modified).getTime() > 30 * 86400e3)
    .sort((a, b) => b.size - a.size).map((x, i) => ({ ...x, id: `orphan:${i}:${x.path}` }));
}

// ---------------------------------------------------------------- browser extensions
function readJson(p) { try { return JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\ufeff/, '')); } catch { return null; } }
function chromiumName(dir, manifest) {
  let n = manifest.name || '';
  const m = n.match(/^__MSG_(.+)__$/);
  if (m) {
    for (const loc of [manifest.default_locale, 'en', 'en_US', 'en_GB'].filter(Boolean)) {
      const msgs = readJson(path.join(dir, '_locales', loc, 'messages.json'));
      const hit = msgs && Object.entries(msgs).find(([k]) => k.toLowerCase() === m[1].toLowerCase());
      if (hit) { n = hit[1].message; break; }
    }
  }
  return n || '(unnamed extension)';
}
async function browserExtensions() {
  const out = [];
  const local = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  const roaming = process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming');
  const browsers = [['Chrome', path.join(local, 'Google', 'Chrome', 'User Data')], ['Edge', path.join(local, 'Microsoft', 'Edge', 'User Data')],
    ['Brave', path.join(local, 'BraveSoftware', 'Brave-Browser', 'User Data')], ['Vivaldi', path.join(local, 'Vivaldi', 'User Data')], ['Opera', path.join(roaming, 'Opera Software', 'Opera Stable')]];
  for (const [browser, base] of browsers) {
    let profiles = [];
    try { profiles = fs.readdirSync(base).filter(p => p === 'Default' || /^Profile \d+$/.test(p)); } catch { continue; }
    if (browser === 'Opera') profiles = ['.'];
    for (const prof of profiles) {
      const extDir = path.join(base, prof, 'Extensions');
      let ids = []; try { ids = fs.readdirSync(extDir); } catch { continue; }
      for (const id of ids) {
        if (id === 'Temp') continue;
        let vers = []; try { vers = fs.readdirSync(path.join(extDir, id)); } catch { continue; }
        const ver = vers.sort().pop(); if (!ver) continue;
        const dir = path.join(extDir, id, ver);
        const man = readJson(path.join(dir, 'manifest.json')); if (!man) continue;
        if (man.theme) continue;
        out.push({ id: `${browser}|${prof}|${id}`, browser, profile: prof === '.' ? 'Default' : prof, extId: id, name: chromiumName(dir, man), version: man.version, desc: (man.description || '').startsWith('__MSG_') ? '' : (man.description || '').slice(0, 160),
          permissions: [...(man.permissions || []), ...(man.host_permissions || [])].filter(p => typeof p === 'string').slice(0, 12), path: path.join(extDir, id), forced: false });
      }
    }
  }
  // Firefox
  const ffBase = path.join(roaming, 'Mozilla', 'Firefox', 'Profiles');
  let ffProfiles = []; try { ffProfiles = fs.readdirSync(ffBase); } catch { /* none */ }
  for (const prof of ffProfiles) {
    const j = readJson(path.join(ffBase, prof, 'extensions.json'));
    for (const a of (j && j.addons) || []) {
      if (a.type !== 'extension' || (a.location && a.location !== 'app-profile')) continue;
      out.push({ id: `Firefox|${prof}|${a.id}`, browser: 'Firefox', profile: prof, extId: a.id, name: (a.defaultLocale && a.defaultLocale.name) || a.id, version: a.version, desc: ((a.defaultLocale && a.defaultLocale.description) || '').slice(0, 160),
        permissions: ((a.userPermissions && a.userPermissions.permissions) || []).slice(0, 12), path: a.path || null, forced: false });
    }
  }
  // Extensions forced by policy (a common adware trick)
  if (process.platform === 'win32') {
    const forced = await ps(`$out = @()
foreach ($p in @(@('Chrome','HKLM:\\SOFTWARE\\Policies\\Google\\Chrome\\ExtensionInstallForcelist'), @('Edge','HKLM:\\SOFTWARE\\Policies\\Microsoft\\Edge\\ExtensionInstallForcelist'), @('Brave','HKLM:\\SOFTWARE\\Policies\\BraveSoftware\\Brave\\ExtensionInstallForcelist'), @('Chrome','HKCU:\\SOFTWARE\\Policies\\Google\\Chrome\\ExtensionInstallForcelist'), @('Edge','HKCU:\\SOFTWARE\\Policies\\Microsoft\\Edge\\ExtensionInstallForcelist'))) {
  $k = Get-ItemProperty -LiteralPath $p[1] -ErrorAction SilentlyContinue
  if ($k) { foreach ($pp in ($k.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' })) { $out += [pscustomobject]@{ browser=$p[0]; key=$p[1]; value=$pp.Name; ext=(([string]$pp.Value) -split ';')[0] } } } }
Out-Json @($out)`).catch(() => []);
    for (const f of list(forced)) {
      const e = out.find(x => x.browser === f.browser && x.extId === f.ext);
      if (e) Object.assign(e, { forced: true, policyKey: f.key, policyValue: f.value });
      else out.push({ id: `${f.browser}|policy|${f.ext}`, browser: f.browser, profile: 'All profiles', extId: f.ext, name: f.ext, version: '', desc: 'Installed by a policy on this PC', permissions: [], path: null, forced: true, policyKey: f.key, policyValue: f.value });
    }
  }
  return out;
}
async function removeForcedExtension(e) {
  return elevated('Remove-ItemProperty -LiteralPath $a.k -Name $a.v -Force -ErrorAction Stop; Out-Result @{ ok=$true }', { k: e.policyKey, v: e.policyValue });
}

// ---------------------------------------------------------------- Windows optional features
async function optionalFeatures() {
  const r = await ps(`Out-Json @(Get-WindowsOptionalFeature -Online -ErrorAction Stop | ForEach-Object { [pscustomobject]@{ name=$_.FeatureName; state=[string]$_.State } })`, undefined, { timeout: 5 * 60 * 1000 });
  return list(r);
}
async function setFeature({ name, enable }) {
  return elevated(`if ($a.enable) { $r = Enable-WindowsOptionalFeature -Online -FeatureName $a.name -NoRestart -All -ErrorAction Stop } else { $r = Disable-WindowsOptionalFeature -Online -FeatureName $a.name -NoRestart -ErrorAction Stop }
$s = [string](Get-WindowsOptionalFeature -Online -FeatureName $a.name).State
Out-Result @{ ok=$true; state=$s; restart=[bool]$r.RestartNeeded }`, { name, enable: !!enable }, { timeout: 30 * 60 * 1000 });
}

// ---------------------------------------------------------------- install tracker
const SNAPSHOT = `
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
$dirs = New-Object System.Collections.Generic.List[string]
foreach ($r in @($env:ProgramFiles, \${env:ProgramFiles(x86)}, $env:ProgramData, $env:APPDATA, $env:LOCALAPPDATA, (Join-Path $env:USERPROFILE 'AppData\\LocalLow'))) {
  if ($r -and (Test-Path -LiteralPath $r)) { Get-ChildItem -LiteralPath $r -Directory -Force -Depth 1 -ErrorAction SilentlyContinue | ForEach-Object { $dirs.Add($_.FullName) } } }
$files = New-Object System.Collections.Generic.List[string]
foreach ($r in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('CommonDesktopDirectory'), (Join-Path $env:APPDATA 'Microsoft\\Windows\\Start Menu\\Programs'), (Join-Path $env:ProgramData 'Microsoft\\Windows\\Start Menu\\Programs'))) {
  if ($r -and (Test-Path -LiteralPath $r)) { Get-ChildItem -LiteralPath $r -Recurse -Filter *.lnk -Force -ErrorAction SilentlyContinue | ForEach-Object { $files.Add($_.FullName) } } }
$reg = New-Object System.Collections.Generic.List[string]
foreach ($rr in 'HKEY_CURRENT_USER\\Software','HKEY_LOCAL_MACHINE\\SOFTWARE','HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node') {
  Get-ChildItem -LiteralPath ('Registry::' + $rr) -ErrorAction SilentlyContinue | ForEach-Object { $reg.Add($rr + '\\' + $_.PSChildName)
    Get-ChildItem -LiteralPath $_.PSPath -ErrorAction SilentlyContinue | ForEach-Object { $reg.Add($rr + '\\' + (Split-Path $_.PSParentPath -Leaf) + '\\' + $_.PSChildName) } } }
$run = @(); foreach ($rk in 'HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run','HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run','HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run') {
  $p = Get-ItemProperty -LiteralPath ('Registry::' + $rk) -ErrorAction SilentlyContinue; if ($p) { $p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { $run += ($rk + '|' + $_.Name) } } }
$svc = @(Get-Service -ErrorAction SilentlyContinue | ForEach-Object Name)
$tasks = @(Get-ScheduledTask -ErrorAction SilentlyContinue | ForEach-Object { $_.TaskPath + $_.TaskName })
$fw = @(Get-NetFirewallRule -ErrorAction SilentlyContinue | ForEach-Object Name)
Out-Json ([pscustomobject]@{ dirs=$dirs; files=$files; reg=$reg; run=$run; services=$svc; tasks=$tasks; firewall=$fw })`;

async function trackStart(label) {
  const snap = await elevated(`${SNAPSHOT.replace(/Out-Json \(\[pscustomobject\]/, 'Out-Result ([pscustomobject]')}`, undefined, { timeout: 10 * 60 * 1000 });
  const id = new Date().toISOString().replace(/[:.]/g, '-');
  fs.mkdirSync(TRACK_ROOT(), { recursive: true });
  fs.writeFileSync(path.join(TRACK_ROOT(), `${id}.json`), JSON.stringify({ id, label: String(label || 'Install').slice(0, 80), started: new Date().toISOString(), before: snap }));
  return { id, label };
}
async function trackFinish(id) {
  const file = path.join(TRACK_ROOT(), `${id.replace(/[^\w-]/g, '')}.json`);
  const t = JSON.parse(fs.readFileSync(file, 'utf8'));
  const after = await elevated(`${SNAPSHOT.replace(/Out-Json \(\[pscustomobject\]/, 'Out-Result ([pscustomobject]')}`, undefined, { timeout: 10 * 60 * 1000 });
  const diff = {};
  for (const k of ['dirs', 'files', 'reg', 'run', 'services', 'tasks', 'firewall']) {
    const before = new Set(list(t.before[k]).map(x => x.toLowerCase()));
    let added = list(after[k]).filter(x => !before.has(x.toLowerCase()));
    if (k === 'dirs' || k === 'reg') { const s = new Set(added.map(x => x.toLowerCase())); added = added.filter(x => !s.has(x.toLowerCase().replace(/\\[^\\]+$/, ''))); } // top-most only
    diff[k] = added;
  }
  t.finished = new Date().toISOString(); t.diff = diff; delete t.before;
  fs.writeFileSync(file, JSON.stringify(t));
  return t;
}
function tracks() {
  let files = []; try { files = fs.readdirSync(TRACK_ROOT()).filter(f => f.endsWith('.json')); } catch { return []; }
  return files.map(f => { try { const t = JSON.parse(fs.readFileSync(path.join(TRACK_ROOT(), f), 'utf8')); return { id: t.id, label: t.label, started: t.started, finished: t.finished || null, diff: t.diff || null }; } catch { return null; } })
    .filter(Boolean).sort((a, b) => (a.started < b.started ? 1 : -1));
}
function trackItems(t) {
  const d = t.diff || {};
  const items = [];
  (d.dirs || []).forEach(p => items.push({ type: 'dir', path: p, level: 'safe', detail: 'Folder created by this install' }));
  (d.files || []).forEach(p => items.push({ type: 'file', path: p, level: 'safe', detail: 'Shortcut created by this install' }));
  (d.reg || []).forEach(p => items.push({ type: 'reg', path: p, level: 'likely', detail: 'Registry key created by this install' }));
  (d.run || []).forEach(p => items.push({ type: 'run', path: p, level: 'safe', detail: 'Startup entry created by this install' }));
  (d.services || []).forEach(p => items.push({ type: 'service', path: p, level: 'likely', detail: 'Service created by this install' }));
  (d.tasks || []).forEach(p => items.push({ type: 'task', path: p, level: 'likely', detail: 'Scheduled task created by this install' }));
  (d.firewall || []).forEach(p => items.push({ type: 'firewall', path: p, level: 'likely', detail: 'Firewall rule created by this install' }));
  return items.map((x, i) => ({ ...x, size: 0, id: `track:${i}:${x.path}` }));
}

// ---------------------------------------------------------------- hunter mode
async function hunt({ seconds = 20 } = {}) {
  return ps(`
Add-Type -Namespace OpenSweep -Name Hunt -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int key);
'@ -ErrorAction SilentlyContinue
$t0 = Get-Date; $down = $false
while (((Get-Date) - $t0).TotalSeconds -lt $a.s) {
  $pressed = ([OpenSweep.Hunt]::GetAsyncKeyState(1) -band 0x8000) -ne 0
  if ($pressed) { $down = $true } elseif ($down) {
    $pt = New-Object OpenSweep.Hunt+POINT; [void][OpenSweep.Hunt]::GetCursorPos([ref]$pt)
    $h = [OpenSweep.Hunt]::GetAncestor([OpenSweep.Hunt]::WindowFromPoint($pt), 2); $procId = 0; [void][OpenSweep.Hunt]::GetWindowThreadProcessId($h, [ref]$procId)
    $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
    $path = $null; try { $path = $p.Path } catch {}
    $company = $null; if ($path) { try { $company = [Diagnostics.FileVersionInfo]::GetVersionInfo($path).CompanyName } catch {} }
    Out-Json ([pscustomobject]@{ pid=[int]$procId; name=$p.ProcessName; path=$path; title=$p.MainWindowTitle; company=$company }); exit }
  Start-Sleep -Milliseconds 40 }
Out-Json @{ timeout=$true }`, { s: seconds }, { timeout: (seconds + 15) * 1000 });
}

// ---------------------------------------------------------------- winget updates
async function wingetUpgrades() {
  const out = await ps('$o = & winget.exe upgrade --include-unknown --accept-source-agreements --disable-interactivity 2>$null | Out-String -Width 400; Out-Json @{ text=$o; code=$LASTEXITCODE }', undefined, { timeout: 3 * 60 * 1000 }).catch(e => ({ text: '', error: e.message }));
  return parseWinget(out && out.text || '');
}
function parseWinget(text) {
  const lines = text.split(/\r?\n/).map(l => l.replace(/^.*\r/, '')).filter(l => l.trim());
  const hi = lines.findIndex(l => /\bId\b/.test(l) && /\bVersion\b/.test(l) && /\bAvailable\b/.test(l));
  if (hi < 0) return [];
  const head = lines[hi];
  const cols = ['Id', 'Version', 'Available', 'Source'].map(c => head.indexOf(c));
  const res = [];
  for (const l of lines.slice(hi + 1)) {
    if (/^-+$/.test(l.trim()) || /upgrades? available|package\(s\)/i.test(l) || l.length < cols[2]) continue;
    const name = l.slice(0, cols[0]).trim(), id = l.slice(cols[0], cols[1]).trim(), version = l.slice(cols[1], cols[2]).trim(), available = l.slice(cols[2], cols[3] > 0 ? cols[3] : undefined).trim();
    if (id && /^[\w.+-]+$/.test(id) && available) res.push({ name, id, version, available });
  }
  return res;
}
async function wingetUpgrade(id) {
  if (!/^[\w.+-]{2,128}$/.test(id)) throw new Error('Invalid package id');
  return elevated('$o = & winget.exe upgrade --id $a.id --exact --silent --accept-source-agreements --accept-package-agreements --disable-interactivity 2>&1 | Out-String; Out-Result @{ ok=($LASTEXITCODE -eq 0); code=$LASTEXITCODE; text=$o.Substring([math]::Max(0,$o.Length-400)) }', { id }, { timeout: 30 * 60 * 1000 });
}

module.exports = { programs, silentCommand, runUninstall, stillInstalled, leftoverScan, removeLeftovers, restoreBackup, orphanScan, browserExtensions, removeForcedExtension,
  optionalFeatures, setFeature, trackStart, trackFinish, tracks, trackItems, hunt, wingetUpgrades, wingetUpgrade, parseWinget, nameKeys, cleanName, publisherKey, BACKUP_ROOT };
