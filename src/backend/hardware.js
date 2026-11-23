'use strict';
// Hardware check (0.4.0-alpha.4, docs/core/alpha4.md section 1).
// Read-only: one PowerShell 5.1 script collects raw facts (every part in its own try/catch, so one
// failing source never fails the whole check). analyze() turns the raw facts into plain-language
// checks; the demo feeds it simulated raw data so both paths share the same logic.
// Fixes run only when the user presses a button, only from a fixed set of ids, and only for
// targets seen in the last check (kept in memory here, never taken from the renderer as-is).

const { ps, elevated } = require('./pscore');

const STATUSES = ['ok', 'warn', 'bad', 'unknown'];
const FIX_KINDS = ['button', 'steps', 'link'];
const FIX_IDS = ['refresh_rate', 'refresh_revert', 'ssd_retrim', 'power_plan'];
const CHECK_IDS = ['ram_speed', 'refresh_rate', 'pcie_gpu', 'ssd', 'power', 'thermal', 'drivers_age', 'game_mode', 'hags'];

// In-memory P/Invoke (same Reflection.Emit pattern as main.js FG_SCRIPT; nothing is written to disk).
// Structs are passed as byte arrays (blittable, pinned during the call) and read at fixed offsets:
//   DISPLAY_DEVICEW (840 bytes): cb@0, DeviceName@4 (32 wchar), DeviceString@68 (128), StateFlags@324, DeviceID@328 (128)
//   DEVMODEW (220 bytes): dmSize@68, dmFields@72, dmDisplayOrientation@84, dmBitsPerPel@168,
//                         dmPelsWidth@172, dmPelsHeight@176, dmDisplayFlags@180, dmDisplayFrequency@184
//   SYSTEM_POWER_STATUS (12 bytes): ACLineStatus@0, BatteryFlag@1, BatteryLifePercent@2, SystemStatusFlag@3
const NATIVE = String.raw`
$OsNative = $null; $nativeErr = $null
try {
  $asm = [Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly((New-Object Reflection.AssemblyName 'OpenSweepHw'), [Reflection.Emit.AssemblyBuilderAccess]::Run)
  $tb = $asm.DefineDynamicModule('OpenSweepHw').DefineType('OpenSweepHw.N', 'Public, Class')
  $attr = [Reflection.MethodAttributes]'Public, Static, PinvokeImpl'
  $defs = @(
    @('EnumDisplayDevices', 'user32.dll', [Int32], [Type[]]@([String], [UInt32], [Byte[]], [UInt32])),
    @('EnumDisplaySettings', 'user32.dll', [Int32], [Type[]]@([String], [Int32], [Byte[]])),
    @('ChangeDisplaySettingsEx', 'user32.dll', [Int32], [Type[]]@([String], [Byte[]], [IntPtr], [UInt32], [IntPtr])),
    @('GetSystemPowerStatus', 'kernel32.dll', [Int32], [Type[]]@([Byte[]])))
  foreach ($d in $defs) {
    $m = $tb.DefinePInvokeMethod($d[0], $d[1], $attr, [Reflection.CallingConventions]::Standard, $d[2], $d[3], [Runtime.InteropServices.CallingConvention]::Winapi, [Runtime.InteropServices.CharSet]::Unicode)
    $m.SetImplementationFlags('PreserveSig')
  }
  $OsNative = $tb.CreateType()
} catch { $nativeErr = $_.Exception.Message }
function Need-Native { if (-not $OsNative) { throw ('Windows display/power functions unavailable: ' + $nativeErr) } }
function U32($b, $o) { [BitConverter]::ToUInt32($b, $o) }
function WS($b, $o, $n) { [Text.Encoding]::Unicode.GetString($b, $o, $n).Split([char]0)[0] }
function New-DD { $d = New-Object byte[] 840; [BitConverter]::GetBytes([UInt32]840).CopyTo($d, 0); ,$d }
function New-DM { $d = New-Object byte[] 220; [BitConverter]::GetBytes([UInt16]220).CopyTo($d, 68); ,$d }
# All refresh rates offered at the given resolution/colour depth (either orientation), progressive modes only.
function Get-Rates($dev, $w, $h, $bpp) {
  $rates = New-Object 'System.Collections.Generic.List[int]'
  for ($j = 0; $j -lt 5000; $j++) {
    $m = New-DM
    if ($OsNative::EnumDisplaySettings($dev, $j, $m) -eq 0) { break }
    $mw = U32 $m 172; $mh = U32 $m 176
    if (((($mw -eq $w) -and ($mh -eq $h)) -or (($mw -eq $h) -and ($mh -eq $w))) -and ((U32 $m 168) -eq $bpp) -and -not ((U32 $m 180) -band 2)) {
      $f = [int](U32 $m 184); if ($f -gt 1 -and -not $rates.Contains($f)) { $rates.Add($f) }
    }
  }
  ,@($rates | Sort-Object)
}
`;

const CHECK_SCRIPT = String.raw`$ErrorActionPreference = 'Stop'
` + NATIVE + String.raw`
$R = [ordered]@{}
function Try-Part($name, [scriptblock]$sb) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  try { $d = & $sb; $R[$name] = @{ ok = $true; data = $d; ms = $sw.ElapsedMilliseconds } }
  catch { $R[$name] = @{ ok = $false; error = $_.Exception.Message; ms = $sw.ElapsedMilliseconds } }
}

Try-Part 'system' {
  $cs = Get-CimInstance Win32_ComputerSystem
  $en = Get-CimInstance Win32_SystemEnclosure | Select-Object -First 1
  @{ maker = [string]$cs.Manufacturer; model = [string]$cs.Model; chassis = @($en.ChassisTypes); battery = [bool](Get-CimInstance Win32_Battery);
     virtual = [bool](([string]$cs.Model -match 'Virtual|VMware|KVM|HVM domU|Standard PC') -or ([string]$cs.Manufacturer -match 'QEMU|Xen|innotek|Parallels|VMware')) }
}

Try-Part 'ram' {
  @(Get-CimInstance Win32_PhysicalMemory | ForEach-Object {
    [pscustomobject]@{ speed = $_.Speed; configured = $_.ConfiguredClockSpeed; capacity = [double]$_.Capacity; bank = [string]$_.BankLabel; slot = [string]$_.DeviceLocator;
      type = $_.SMBIOSMemoryType; formFactor = $_.FormFactor; maker = [string]$_.Manufacturer; part = ([string]$_.PartNumber).Trim() } })
}

Try-Part 'display' {
  Need-Native
  $names = @{}
  try {
    foreach ($w in (Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction Stop)) {
      $n = -join ($w.UserFriendlyName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
      $p = ([string]$w.InstanceName) -split '\\'
      if ($p.Count -ge 2 -and $n) { $names[$p[1].ToUpper()] = $n }
    }
  } catch {}
  $out = @()
  for ($i = 0; $i -lt 32; $i++) {
    $dd = New-DD
    if ($OsNative::EnumDisplayDevices([NullString]::Value, [UInt32]$i, $dd, [UInt32]0) -eq 0) { break }
    $flags = U32 $dd 324
    if (-not ($flags -band 1)) { continue }   # DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
    $dev = WS $dd 4 64
    $md = New-DD; $mon = ''; $monId = ''
    if ($OsNative::EnumDisplayDevices($dev, [UInt32]0, $md, [UInt32]0) -ne 0) { $mon = WS $md 68 256; $monId = WS $md 328 256 }
    $hw = ''; $p = $monId -split '\\'; if ($p.Count -ge 2) { $hw = $p[1].ToUpper() }
    $cur = New-DM
    if ($OsNative::EnumDisplaySettings($dev, -1, $cur) -eq 0) { continue }   # ENUM_CURRENT_SETTINGS
    $w = U32 $cur 172; $h = U32 $cur 176; $bpp = U32 $cur 168
    $out += [pscustomobject]@{ device = $dev; adapter = (WS $dd 68 256); monitor = $(if ($hw -and $names[$hw]) { $names[$hw] } else { $mon }); primary = [bool]($flags -band 4);
      width = $w; height = $h; bpp = $bpp; hz = [int](U32 $cur 184); rates = (Get-Rates $dev $w $h $bpp) }
  }
  ,$out
}

Try-Part 'gpu' {
  $keys = @('DEVPKEY_PciDevice_CurrentLinkSpeed', 'DEVPKEY_PciDevice_CurrentLinkWidth', 'DEVPKEY_PciDevice_MaxLinkSpeed', 'DEVPKEY_PciDevice_MaxLinkWidth')
  @(Get-PnpDevice -Class Display -PresentOnly | ForEach-Object {
    $o = [ordered]@{ name = [string]$_.FriendlyName; id = [string]$_.InstanceId; status = [string]$_.Status }
    try { foreach ($p in (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName $keys)) { if ($p.Data -ne $null) { $o[($p.KeyName -replace '^DEVPKEY_PciDevice_', '')] = [int]$p.Data } } } catch {}
    [pscustomobject]$o })
}

Try-Part 'ssd' {
  $trim = $null
  try { $t = (& fsutil.exe behavior query DisableDeleteNotify) -join ' '; if ($t -match 'NTFS\s+DisableDeleteNotify\s*=\s*(\d)') { $trim = [int]$Matches[1] } elseif ($t -match 'DisableDeleteNotify\s*=\s*(\d)') { $trim = [int]$Matches[1] } } catch {}
  $pd = @{}
  try { foreach ($d in (Get-PhysicalDisk)) { $pd[[string]$d.DeviceId] = @{ media = [string]$d.MediaType; bus = [string]$d.BusType; name = [string]$d.FriendlyName } } } catch {}
  $vols = @(Get-Volume | Where-Object { $_.DriveType -eq 'Fixed' -and $_.DriveLetter -and $_.Size -gt 0 } | ForEach-Object {
    $dn = $null; try { $dn = [string](Get-Partition -DriveLetter $_.DriveLetter).DiskNumber } catch {}
    $info = if ($dn -and $pd.ContainsKey($dn)) { $pd[$dn] } else { @{} }
    [pscustomobject]@{ letter = [string]$_.DriveLetter; label = [string]$_.FileSystemLabel; fs = [string]$_.FileSystem; size = [double]$_.Size; free = [double]$_.SizeRemaining;
      media = $info.media; bus = $info.bus; disk = $info.name } })
  @{ disableDeleteNotify = $trim; volumes = $vols }
}

Try-Part 'power' {
  $plans = @()
  $list = (& powercfg.exe /list) -join ' '
  foreach ($m in [regex]::Matches($list, '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\s+\(([^)]+)\)\s*(\*)?')) {
    $plans += [pscustomobject]@{ guid = $m.Groups[1].Value.ToLower(); name = $m.Groups[2].Value; active = $m.Groups[3].Success }
  }
  $ac = $null; $saver = $null; $pctB = $null
  if ($OsNative) { $s = New-Object byte[] 12; if ($OsNative::GetSystemPowerStatus($s) -ne 0) { $ac = [int]$s[0]; $pctB = [int]$s[2]; $saver = [int]$s[3] } }
  $ov = $null
  try { $k = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes'; $ov = @{ ac = [string]$k.ActiveOverlayAcPowerScheme; dc = [string]$k.ActiveOverlayDcPowerScheme } } catch {}
  @{ plans = $plans; ac = $ac; saver = $saver; batteryPercent = $pctB; overlay = $ov }
}

Try-Part 'thermal' {
  $since = (Get-Date).AddDays(-30)
  $ev = @()
  try { $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-Power'; Id = 86, 125; StartTime = $since } -MaxEvents 100 | ForEach-Object { @{ id = $_.Id; level = [int]$_.Level; time = $_.TimeCreated.ToString('o') } }) }
  catch { if ($_.FullyQualifiedErrorId -notmatch 'NoMatchingEventsFound') { throw } }
  $fw = 0
  try { $fw = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-Processor-Power'; Id = 37; StartTime = $since } -MaxEvents 500).Count } catch {}
  $cpu = @(Get-CimInstance Win32_Processor | ForEach-Object { @{ name = ([string]$_.Name).Trim(); load = $_.LoadPercentage; cur = $_.CurrentClockSpeed; max = $_.MaxClockSpeed } })
  @{ events = $ev; firmwareLimited = $fw; cpu = $cpu }
}

Try-Part 'drivers' {
  @(Get-CimInstance -Query "SELECT DeviceName, DeviceClass, DriverDate, DriverVersion, Manufacturer, DriverProviderName FROM Win32_PnPSignedDriver WHERE DeviceClass='DISPLAY' OR DeviceClass='SCSIADAPTER' OR DeviceClass='HDC' OR DeviceClass='SYSTEM'" |
    Where-Object { $_.DeviceName -and $_.DriverDate } | ForEach-Object {
      [pscustomobject]@{ name = [string]$_.DeviceName; cls = [string]$_.DeviceClass; date = $_.DriverDate.ToString('yyyy-MM-dd'); version = [string]$_.DriverVersion; maker = [string]$_.Manufacturer; provider = [string]$_.DriverProviderName } })
}

Try-Part 'gaming' {
  $gm = $null; try { $gm = (Get-ItemProperty 'HKCU:\Software\Microsoft\GameBar' -Name AutoGameModeEnabled).AutoGameModeEnabled } catch {}
  $hs = $null; try { $hs = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -Name HwSchMode).HwSchMode } catch {}
  @{ gameMode = $gm; hwSchMode = $hs }
}

[Console]::Out.Write((ConvertTo-Json -InputObject $R -Depth 8 -Compress))
`;

// ------------------------------------------------------------------ fixes (scripts)
const REFRESH_SCRIPT = String.raw`$ErrorActionPreference = 'Stop'
` + NATIVE + String.raw`
Need-Native
$dev = [string]$a.device; $rate = [int]$a.rate
$cur = New-DM
if ($OsNative::EnumDisplaySettings($dev, -1, $cur) -eq 0) { throw 'Could not read the current display mode.' }
$before = [int](U32 $cur 184)
if (-not ((Get-Rates $dev (U32 $cur 172) (U32 $cur 176) (U32 $cur 168)) -contains $rate)) { throw ("$rate Hz is not offered at the current resolution.") }
[BitConverter]::GetBytes([UInt32]$rate).CopyTo($cur, 184)
[BitConverter]::GetBytes([UInt32]0x400000).CopyTo($cur, 72)   # dmFields = DM_DISPLAYFREQUENCY only
$t = $OsNative::ChangeDisplaySettingsEx($dev, $cur, [IntPtr]::Zero, [UInt32]2, [IntPtr]::Zero)   # CDS_TEST
if ($t -ne 0) { throw ("Windows refused $rate Hz for this display (code $t).") }
$r = $OsNative::ChangeDisplaySettingsEx($dev, $cur, [IntPtr]::Zero, [UInt32]1, [IntPtr]::Zero)   # CDS_UPDATEREGISTRY
$after = New-DM; [void]$OsNative::EnumDisplaySettings($dev, -1, $after)
[Console]::Out.Write((ConvertTo-Json -InputObject @{ code = $r; before = $before; after = [int](U32 $after 184) } -Compress))
`;
const RETRIM_SCRIPT = String.raw`Optimize-Volume -DriveLetter $a.letter -ReTrim -ErrorAction Stop
Out-Result @{ ok = $true }`;
const POWER_SCRIPT = String.raw`& powercfg.exe /setactive $a.guid | Out-Null
$now = ((& powercfg.exe /getactivescheme) -join ' ')
Out-Result @{ ok = ($now -match $a.guid); active = $now }`;

// ------------------------------------------------------------------ analysis
const arr = (x) => (x === null || x === undefined ? [] : Array.isArray(x) ? x : [x]);
const num = (x) => (typeof x === 'number' && isFinite(x) ? x : Number(x) || 0);
const GB = (b) => `${Math.round(b / 1073741824)} GB`;
const size = (b) => (b >= 1099511627776 ? `${(b / 1099511627776).toFixed(1)} TB` : `${Math.round(b / 1073741824)} GB`);
function part(raw, name) {
  const p = raw && raw[name];
  if (!p) throw new Error('This part of the check did not run.');
  if (!p.ok) throw new Error(p.error || 'Could not be read.');
  return p.data;
}
const worst = (list) => ['bad', 'warn', 'ok', 'unknown'].find(s => list.includes(s)) || 'unknown';

const BIOS_XMP_STEPS = [
  'Restart the PC and press Del or F2 while it starts (the key is shown on the first screen) to open the BIOS/UEFI setup.',
  'Find the memory profile setting. It is called XMP on Intel boards, EXPO on AMD boards (ASUS also uses "D.O.C.P.", MSI "A-XMP").',
  'Set it to "Profile 1" (or Enabled). Leave the other memory settings on Auto.',
  'Save and exit (usually F10). The first start can take a minute while the memory is trained.',
  'If the PC does not start, it normally returns to safe settings after a few attempts (or clear CMOS); then leave the profile off.',
];
const DUAL_CHANNEL_STEPS = [
  'Check the motherboard manual for which slots to use with two sticks (usually A2 and B2, the 2nd and 4th slot counted from the CPU).',
  'With only one stick, adding a second identical stick turns on dual channel and roughly doubles memory bandwidth.',
  'Laptops: some have one soldered module and one free slot; check the maker\'s manual before buying.',
];

function checkRam(raw) {
  const base = { id: 'ram_speed', title: 'Memory speed & channels' };
  const sys = (raw.system && raw.system.ok && raw.system.data) || {};
  const sticks = arr(part(raw, 'ram'));
  if (sys.virtual) return { ...base, status: 'unknown', found: 'Virtual machine', expected: '—', why: 'A virtual machine does not report real memory speeds.', fix: null };
  if (!sticks.length) return { ...base, status: 'unknown', found: 'No memory modules reported', expected: '—', why: 'Windows did not list any memory modules (Win32_PhysicalMemory).', fix: null };
  const rated = Math.max(...sticks.map(s => num(s.speed)));
  const runningList = sticks.map(s => num(s.configured)).filter(Boolean);
  const running = runningList.length ? Math.min(...runningList) : 0;
  const type = { 20: 'DDR', 21: 'DDR2', 24: 'DDR3', 26: 'DDR4', 34: 'DDR5', 35: 'LPDDR5' }[num(sticks[0].type)] || '';
  const total = sticks.reduce((a, s) => a + num(s.capacity), 0);
  // Channel from the slot/bank names when the firmware names them ("ChannelA-DIMM0", "DIMM_A1", "P0 CHANNEL B", "A2").
  const chan = (s) => { const t = `${s.bank} ${s.slot}`; const m = t.match(/channel\s*([A-H])\b/i) || t.match(/DIMM[_\s-]?([A-H])\d/i) || String(s.slot || '').match(/^([A-H])\d$/i); return m ? m[1].toUpperCase() : null; };
  const chans = sticks.map(chan);
  const single = sticks.length === 1 || (chans.every(Boolean) && new Set(chans).size === 1);
  // Some firmware reports ConfiguredClockSpeed in MHz (half the MT/s figure): treat exactly half as "same speed".
  const slow = rated && running && running < rated * 0.97 && Math.abs(running * 2 - rated) > 2;
  if (!rated || !running) return { ...base, status: 'unknown', found: `${sticks.length} stick(s), ${GB(total)}${type ? ' ' + type : ''}`, expected: '—', why: 'The memory modules do not report their speed to Windows.', fix: null };
  const found = `${running} MT/s running · ${rated} MT/s rated · ${sticks.length} stick${sticks.length > 1 ? 's' : ''}, ${GB(total)}${type ? ' ' + type : ''}${single ? ' · single channel' : ''}`;
  const why = [];
  if (slow) why.push(`Your memory runs at ${running} MT/s but is rated for ${rated} MT/s. XMP/EXPO is likely off in the BIOS, so the memory uses a slow safe default. Turning it on is free speed, especially in games.`);
  if (single) why.push(sticks.length === 1 ? 'Only one memory stick is installed, so the CPU uses a single memory channel and gets about half the memory bandwidth.' : 'All sticks appear to be in the same memory channel, so the CPU gets about half the memory bandwidth.');
  const status = slow || single ? 'warn' : 'ok';
  if (!why.length) why.push('Memory runs at its rated speed in dual (or more) channel mode.');
  const steps = [...(slow ? BIOS_XMP_STEPS : []), ...(single ? DUAL_CHANNEL_STEPS : [])];
  return {
    ...base, status, found, expected: `${rated} MT/s in dual channel (2 or 4 matching sticks)`, why: why.join(' '),
    note: status === 'ok' ? 'Windows reports the speed stored on the module itself. If your RAM box advertises a higher speed (for example 3600 or 6000), XMP/EXPO may still be off.' : undefined,
    fix: steps.length ? { kind: 'steps', label: slow ? 'Turn on XMP/EXPO in the BIOS' : 'Use dual channel', steps } : null,
  };
}

function checkRefresh(raw) {
  const base = { id: 'refresh_rate', title: 'Monitor refresh rate' };
  const mons = arr(part(raw, 'display'));
  if (!mons.length) return { ...base, status: 'unknown', found: 'No active display found', expected: '—', why: 'Windows did not report an active display.', fix: null };
  const items = mons.map(m => {
    const rates = arr(m.rates).map(num).filter(r => r > 1);
    const max = Math.max(num(m.hz), ...rates);
    const low = max > num(m.hz) + 2;
    const label = `${m.monitor || 'Display'}${m.primary && mons.length > 1 ? ' (main)' : ''}`;
    return {
      target: String(m.device), label, current: num(m.hz), max, status: low ? 'warn' : 'ok',
      found: `${num(m.hz)} Hz at ${m.width}×${m.height}`, expected: `${max} Hz`,
      fix: low ? { kind: 'button', id: 'refresh_rate', target: String(m.device), label: `Switch to ${max} Hz`, confirm: `Set ${label} to ${max} Hz?\nThe screen may go black for a moment. If you do not confirm within 15 seconds, the old ${num(m.hz)} Hz comes back automatically.` } : null,
    };
  });
  const bad = items.filter(i => i.status === 'warn');
  const one = items.length === 1 ? items[0] : null;
  return {
    ...base, status: bad.length ? 'warn' : 'ok', items,
    found: one ? `${one.current} Hz (${one.label})` : items.map(i => `${i.label}: ${i.current} Hz`).join(' · '),
    expected: one ? `${one.max} Hz (highest this monitor offers at its current resolution)` : items.map(i => `${i.label}: ${i.max} Hz`).join(' · '),
    why: bad.length
      ? `${bad.map(i => i.label).join(', ')} can show ${bad.map(i => `${i.max} Hz`).join(' / ')} but Windows uses ${bad.map(i => `${i.current} Hz`).join(' / ')}. You paid for a smoother screen: higher refresh makes motion, scrolling and games look smoother. Windows often leaves new monitors at 60 Hz.`
      : 'Every monitor runs at the highest refresh rate it offers at its current resolution.',
    fix: bad.length ? { ...bad[0].fix } : null,
    altFix: bad.length ? { kind: 'link', label: 'Advanced display settings', url: 'ms-settings:display-advanced' } : null,
  };
}

const PCIE_GEN = { 1: 'PCIe 1.0 (2.5 GT/s)', 2: 'PCIe 2.0 (5 GT/s)', 3: 'PCIe 3.0 (8 GT/s)', 4: 'PCIe 4.0 (16 GT/s)', 5: 'PCIe 5.0 (32 GT/s)', 6: 'PCIe 6.0 (64 GT/s)' };
function checkPcie(raw) {
  const base = { id: 'pcie_gpu', title: 'Graphics card slot (PCIe link)' };
  const gpus = arr(part(raw, 'gpu'));
  const linked = gpus.filter(g => num(g.MaxLinkWidth) > 0 && num(g.CurrentLinkWidth) > 0);
  if (!linked.length) return { ...base, status: 'unknown', found: gpus.length ? gpus.map(g => g.name).join(', ') : 'No graphics adapter found', expected: '—', why: 'No PCIe link information: this is normal for graphics built into the CPU or a virtual machine.', fix: null };
  const items = linked.map(g => {
    const narrow = num(g.CurrentLinkWidth) < num(g.MaxLinkWidth);
    const speed = num(g.CurrentLinkSpeed) && num(g.MaxLinkSpeed) ? ` · ${PCIE_GEN[num(g.CurrentLinkSpeed)] || 'Gen ' + g.CurrentLinkSpeed} now, up to ${PCIE_GEN[num(g.MaxLinkSpeed)] || 'Gen ' + g.MaxLinkSpeed}` : '';
    return { target: g.id, label: g.name, status: narrow ? 'warn' : 'ok', found: `x${g.CurrentLinkWidth} of x${g.MaxLinkWidth} lanes${speed}`, expected: `x${g.MaxLinkWidth} lanes` };
  });
  const bad = items.filter(i => i.status === 'warn');
  return {
    ...base, status: bad.length ? 'warn' : 'ok', items,
    found: items.map(i => `${i.label}: ${i.found}`).join(' · '), expected: items.map(i => i.expected).join(' · '),
    why: (bad.length
      ? `${bad.map(i => i.label).join(', ')} uses fewer PCIe lanes than it supports. That usually means it sits in a secondary slot, shares lanes with an M.2 drive, or is not fully seated.`
      : 'The graphics card uses all the PCIe lanes it supports.') + ' The link speed is shown for information only: graphics cards lower it when idle to save power, so a lower speed right now is normal.',
    fix: bad.length ? { kind: 'steps', label: 'Use the full-speed slot', steps: [
      'Shut down, unplug the PC and open the case.',
      'Check the motherboard manual for the full-speed x16 slot (usually the top one, closest to the CPU).',
      'Move the graphics card there and press it in until the slot latch clicks. Re-attach the power cables.',
      'Some boards halve the slot when certain M.2 slots are used; the manual lists which ones.',
      'In the BIOS, set the PCIe slot configuration to Auto/x16 if your board has that option.',
    ] } : null,
  };
}

function checkSsd(raw) {
  const base = { id: 'ssd', title: 'Drives: free space & TRIM' };
  const d = part(raw, 'ssd') || {};
  const vols = arr(d.volumes);
  if (!vols.length) return { ...base, status: 'unknown', found: 'No fixed drives found', expected: '—', why: 'Windows did not list any fixed drives with a letter.', fix: null };
  const trimOff = num(d.disableDeleteNotify) === 1;
  const isSsd = (v) => /ssd/i.test(v.media || '') || /nvme/i.test(v.bus || '');
  const items = vols.map(v => {
    const p = v.size ? Math.round((100 * num(v.free)) / num(v.size)) : 0;
    const lowSpace = p < 15;
    const ssd = isSsd(v);
    return {
      target: String(v.letter).toUpperCase(), label: `${String(v.letter).toUpperCase()}: ${v.label || ''}`.trim(), lowSpace, ssd,
      status: lowSpace || (ssd && trimOff) ? 'warn' : 'ok',
      found: `${size(num(v.free))} free of ${size(num(v.size))} (${p}%) · ${ssd ? (/nvme/i.test(v.bus || '') ? 'NVMe SSD' : 'SSD') : /hdd/i.test(v.media || '') ? 'Hard disk' : 'Unknown type'}${v.disk ? ' · ' + v.disk : ''}`,
      expected: 'At least 15% free',
      fix: ssd ? { kind: 'button', id: 'ssd_retrim', target: String(v.letter).toUpperCase(), label: 'Run TRIM now', confirm: `Tell the SSD behind ${String(v.letter).toUpperCase()}: which blocks are free (Optimize-Volume -ReTrim)?\nThis is what Windows' weekly "Optimize drives" task does. It is safe and takes up to a few minutes. Needs administrator rights.` } : null,
    };
  });
  const low = items.filter(i => i.lowSpace);
  const anySsd = items.some(i => i.ssd);
  const why = [];
  if (low.length) why.push(`${low.map(i => i.target + ':').join(', ')} ${low.length > 1 ? 'are' : 'is'} almost full. Windows needs free space for updates, the page file and temporary files, and SSDs slow down when they are nearly full.`);
  if (anySsd && trimOff) why.push('TRIM is turned off, so your SSD does not learn which blocks are free and gets slower over time.');
  if (!why.length) why.push(`Every drive has enough free space${anySsd ? ' and TRIM is on' : ''}.`);
  return {
    ...base, status: worst(items.map(i => i.status)), items,
    found: items.map(i => `${i.target}: ${i.found.split(' · ')[0]}`).join(' · ') + (anySsd ? ` · TRIM ${d.disableDeleteNotify === null || d.disableDeleteNotify === undefined ? 'unknown' : trimOff ? 'off' : 'on'}` : ''),
    expected: 'At least 15% free on every drive; TRIM on for SSDs',
    why: why.join(' '),
    fix: anySsd && trimOff ? { kind: 'steps', label: 'Turn TRIM back on', steps: ['Open Terminal (Admin) or PowerShell as administrator.', 'Run: fsutil behavior set DisableDeleteNotify 0', 'Restart the PC.'] }
      : low.length ? { kind: 'link', label: 'Open Storage settings', url: 'ms-settings:storagesense' } : null,
  };
}

const PLAN = { balanced: '381b4222-f694-41f0-9685-ff5bb260df2e', high: '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c', saver: 'a1841308-3542-4fab-bc81-f71121e6c3d8', ultimate: 'e9a42b02-d5df-448d-aa00-03f14749eb61' };
const OVERLAY = { '961cc777-2547-4f9d-8174-7d86181b8a7a': 'Best power efficiency', '3af9b8d9-7c97-431d-ad78-34a8bfea439f': 'Balanced (better battery)', '00000000-0000-0000-0000-000000000000': 'Balanced', 'ded574b5-45a0-4f42-8737-46345c09c238': 'Best performance' };
function checkPower(raw) {
  const base = { id: 'power', title: 'Power plan & power mode' };
  const d = part(raw, 'power') || {};
  const plans = arr(d.plans);
  const active = plans.find(p => p.active);
  if (!active) return { ...base, status: 'unknown', found: 'No active power plan reported', expected: '—', why: 'powercfg did not report an active plan.', fix: null, plans };
  const onBattery = d.ac === 0;
  const ovGuid = String((d.overlay && (onBattery ? d.overlay.dc : d.overlay.ac)) || '').toLowerCase();
  const mode = OVERLAY[ovGuid] || null;
  const saverPlan = active.guid === PLAN.saver;
  const batterySaver = d.saver === 1;
  const efficientMode = !onBattery && ovGuid === '961cc777-2547-4f9d-8174-7d86181b8a7a';
  const why = [];
  let fix = null;
  if (saverPlan) {
    const to = plans.find(p => p.guid === PLAN.balanced) || plans.find(p => p.guid === PLAN.high) || plans.find(p => !p.active);
    why.push(`The "${active.name}" plan keeps the CPU slow to save energy${onBattery ? '. That helps on battery but makes everything feel slower' : ', which makes everything feel slower while plugged in'}.`);
    if (to) fix = { kind: 'button', id: 'power_plan', target: to.guid, label: `Switch to ${to.name}`, confirm: `Switch the power plan from "${active.name}" to "${to.name}"?\nYou can switch back any time (Repair & power). Needs administrator rights.` };
  }
  if (batterySaver) { why.push('Battery saver is on: Windows limits background activity and performance.'); fix = fix || { kind: 'link', label: 'Battery saver settings', url: 'ms-settings:batterysaver' }; }
  if (efficientMode) { why.push('Power mode is "Best power efficiency" while plugged in, which limits CPU speed.'); fix = fix || { kind: 'link', label: 'Power mode settings', url: 'ms-settings:powersleep' }; }
  const status = saverPlan || batterySaver || efficientMode ? 'warn' : 'ok';
  if (!why.length) why.push(`The "${active.name}" plan${mode ? ` with power mode "${mode}"` : ''} lets the CPU reach full speed when needed.`);
  return {
    ...base, status, plans,
    found: `${active.name}${mode ? ` · power mode ${mode}` : ''} · ${onBattery ? `on battery${d.batteryPercent >= 0 && d.batteryPercent <= 100 ? ` (${d.batteryPercent}%)` : ''}` : d.ac === 1 ? 'plugged in' : 'power source unknown'}${batterySaver ? ' · battery saver on' : ''}`,
    expected: 'Balanced or High performance; no battery saver while plugged in',
    why: why.join(' '), fix,
  };
}

function checkThermal(raw) {
  const base = { id: 'thermal', title: 'Overheating & throttling' };
  const d = part(raw, 'thermal') || {};
  const events = arr(d.events).filter(e => num(e.level) > 0 && num(e.level) <= 3); // critical, error, warning only
  const cpu = arr(d.cpu)[0] || {};
  const busy = num(cpu.load) >= 60;
  const throttled = busy && num(cpu.max) > 0 && num(cpu.cur) < num(cpu.max) * 0.6;
  const fwNote = num(d.firmwareLimited) ? ` Windows also logged ${d.firmwareLimited} time(s) that the firmware limited the processor speed (Kernel-Processor-Power 37); on laptops this is often normal power management.` : '';
  const clock = num(cpu.max) ? `${cpu.cur} of ${cpu.max} MHz at ${num(cpu.load)}% load` : '';
  const steps = ['Clean dust from the fans, heatsinks and air filters (compressed air, with the PC off and unplugged).',
    'Make sure the vents are free: laptops on a hard, flat surface, desktops not pushed against a wall.',
    'Check that all fans spin (BIOS hardware monitor, or the maker\'s app).',
    'If the PC is several years old, new thermal paste on the CPU/GPU often helps (or ask a repair shop).'];
  if (events.length || throttled) {
    return {
      ...base, status: 'warn', found: [events.length ? `${events.length} thermal event(s) in the last 30 days` : '', throttled ? `CPU at ${clock}` : ''].filter(Boolean).join(' · '),
      expected: 'No thermal events; full clock speed under load',
      why: `${events.length ? 'Windows logged thermal events (Kernel-Power 86/125): the PC got hot enough for the firmware to step in. ' : ''}${throttled ? 'The processor runs well below its rated clock while busy, a sign of heat or power limits. ' : ''}Hot PCs slow themselves down to protect the hardware.${fwNote}`,
      fix: { kind: 'steps', label: 'Cool it down', steps },
    };
  }
  return {
    ...base, status: 'ok', found: `No thermal events in the last 30 days${clock ? ` · CPU ${clock}` : ''}`, expected: 'No thermal events; full clock speed under load',
    why: `Windows has not logged any overheating events.${busy ? '' : ' The CPU was not busy during the check, so throttling under load could not be measured.'}${fwNote}`, fix: null,
  };
}

const VENDOR = [
  [/nvidia/i, 'NVIDIA', 'https://www.nvidia.com/Download/index.aspx'],
  [/amd|advanced micro|ati\b|radeon/i, 'AMD', 'https://www.amd.com/en/support/download/drivers.html'],
  [/intel/i, 'Intel', 'https://www.intel.com/content/www/us/en/support/detect.html'],
];
function checkDrivers(raw, now = Date.now()) {
  const base = { id: 'drivers_age', title: 'Driver age (graphics, chipset, storage)' };
  const sys = (raw.system && raw.system.ok && raw.system.data) || {};
  const all = arr(part(raw, 'drivers'));
  const notMs = (x) => !/^microsoft/i.test(x.provider || '');
  const months = (x) => Math.floor((now - new Date(x.date).getTime()) / (30.44 * 86400e3));
  const pick = [];
  const display = all.filter(x => /^display$/i.test(x.cls));
  const gpus = display.filter(notMs);
  for (const g of gpus) pick.push({ ...g, group: 'Graphics' });
  const chipset = all.filter(x => /^system$/i.test(x.cls) && notMs(x) && /intel|amd|advanced micro/i.test(`${x.provider} ${x.maker}`)).sort((a, b) => (a.date < b.date ? 1 : -1))[0];
  if (chipset) pick.push({ ...chipset, name: `Chipset (${chipset.provider || chipset.maker})`, group: 'Chipset' });
  for (const s of all.filter(x => /^(scsiadapter|hdc)$/i.test(x.cls) && notMs(x))) pick.push({ ...s, group: 'Storage' });
  if (!gpus.length && display.length) {
    if (sys.virtual) return { ...base, status: 'unknown', found: display.map(x => x.name).join(', '), expected: '—', why: 'Virtual machine: the graphics driver comes from the virtualisation software.', fix: null };
    return { ...base, status: 'warn', found: display.map(x => x.name).join(', '), expected: 'The graphics maker\'s driver', why: 'Windows uses its basic display driver: no proper graphics driver is installed, so games and video run slowly and high refresh rates may be missing.',
      fix: { kind: 'link', label: 'Check for driver updates', url: 'ms-settings:windowsupdate-optionalupdates' } };
  }
  if (!pick.length) return { ...base, status: 'unknown', found: 'Only Windows built-in drivers', expected: '—', why: 'All graphics, chipset and storage drivers come with Windows and are updated by Windows Update.', fix: null };
  const items = pick.map(x => ({ label: `${x.group}: ${x.name}`, status: months(x) >= 12 ? 'warn' : 'ok', found: `${x.date} (${months(x) < 1 ? 'this month' : months(x) + ' month(s) old'}) · version ${x.version}`, expected: 'Less than 12 months old', maker: `${x.provider} ${x.maker}` }));
  const old = items.filter(i => i.status === 'warn');
  const v = old.length ? VENDOR.find(([re]) => re.test(old[0].maker)) : null;
  return {
    ...base, status: old.length ? 'warn' : 'ok', items: items.map(({ maker, ...i }) => i),
    found: items.map(i => `${i.label.split(':')[0]}: ${i.found.split(' · ')[0]}`).join(' · '), expected: 'Drivers less than 12 months old',
    why: old.length ? `${old.map(i => i.label).join(', ')} ${old.length > 1 ? 'are' : 'is'} more than a year old. Newer drivers fix bugs and often improve game performance and stability.` : 'Your graphics, chipset and storage drivers are less than a year old.',
    fix: old.length ? { kind: 'link', label: 'Check Windows Update driver updates', url: 'ms-settings:windowsupdate-optionalupdates', vendor: v ? v[1] : undefined, vendorUrl: v ? v[2] : undefined } : null,
  };
}

function checkGameMode(raw) {
  const base = { id: 'game_mode', title: 'Game Mode' };
  const d = part(raw, 'gaming') || {};
  const off = d.gameMode !== null && d.gameMode !== undefined && num(d.gameMode) === 0;
  return {
    ...base, status: off ? 'warn' : 'ok', found: off ? 'Off' : d.gameMode === null || d.gameMode === undefined ? 'On (Windows default)' : 'On', expected: 'On',
    why: off ? 'Game Mode is off. With it on, Windows gives the game priority and holds back Windows Update installs and notifications while you play.' : 'Windows gives games priority while you play.',
    fix: off ? { kind: 'button', tweak: 'gaming.gamemode', label: 'Turn on Game Mode' } : null,
  };
}

function checkHags(raw) {
  const base = { id: 'hags', title: 'Hardware-accelerated GPU scheduling' };
  const d = part(raw, 'gaming') || {};
  const v = d.hwSchMode;
  const optional = { kind: 'button', tweak: 'gaming.hags', label: 'Turn on (restart needed)', optional: true };
  if (v === null || v === undefined) return { ...base, status: 'unknown', found: 'Not set (driver default)', expected: 'Your choice', why: 'Windows uses the graphics driver\'s default. It needs a recent graphics card and driver; it can lower latency slightly but is not faster on every PC.', fix: null };
  const on = num(v) === 2;
  return {
    ...base, status: 'ok', found: on ? 'On' : 'Off', expected: 'Your choice (info only)',
    why: on ? 'The graphics card manages its own memory scheduling. Needed for DLSS Frame Generation.' : 'Off. Turning it on can lower latency slightly on recent graphics cards and is needed for DLSS Frame Generation, but it is not faster on every PC.',
    fix: on ? null : optional,
  };
}

const CHECKS = [
  ['ram_speed', 'Memory speed & channels', checkRam], ['refresh_rate', 'Monitor refresh rate', checkRefresh], ['pcie_gpu', 'Graphics card slot (PCIe link)', checkPcie],
  ['ssd', 'Drives: free space & TRIM', checkSsd], ['power', 'Power plan & power mode', checkPower], ['thermal', 'Overheating & throttling', checkThermal],
  ['drivers_age', 'Driver age (graphics, chipset, storage)', checkDrivers], ['game_mode', 'Game Mode', checkGameMode], ['hags', 'Hardware-accelerated GPU scheduling', checkHags],
];
function analyze(raw) {
  return CHECKS.map(([id, title, fn]) => {
    try { return fn(raw || {}); }
    catch (e) { return { id, title, status: 'unknown', found: 'Could not be checked', expected: '—', why: `Reason: ${e.message}`, reason: e.message, fix: null }; }
  });
}

// Shape check (used by the smoke test and before results leave this module).
function validateCheck(c) {
  const p = [];
  if (!c || typeof c !== 'object') return ['not an object'];
  if (!CHECK_IDS.includes(c.id)) p.push(`unknown id ${c.id}`);
  for (const k of ['title', 'found', 'expected', 'why']) if (typeof c[k] !== 'string' || (k !== 'found' && k !== 'expected' && !c[k])) p.push(`${k} missing`);
  if (!STATUSES.includes(c.status)) p.push(`bad status ${c.status}`);
  for (const f of [c.fix, c.altFix].concat(arr(c.items).map(i => i.fix))) {
    if (f === null || f === undefined) continue;
    if (!FIX_KINDS.includes(f.kind) || typeof f.label !== 'string') p.push(`bad fix ${JSON.stringify(f)}`);
    if (f.kind === 'steps' && !(Array.isArray(f.steps) && f.steps.length)) p.push('steps fix without steps');
    if (f.kind === 'link' && !/^ms-settings:[a-z-]+$/.test(f.url || '')) p.push(`bad link ${f.url}`);
    if (f.kind === 'button' && !(FIX_IDS.includes(f.id) || f.tweak)) p.push(`button without fix id ${JSON.stringify(f)}`);
  }
  for (const i of arr(c.items)) if (!STATUSES.includes(i.status) || typeof i.label !== 'string') p.push(`bad item ${JSON.stringify(i).slice(0, 80)}`);
  return p;
}

// ------------------------------------------------------------------ state + public API
let last = null;          // targets from the last check: { refresh: Map(device -> {current, max}), ssd: Set(letters), plans: Map(guid -> name) }
let prevRefresh = null;   // { target, rate, label } recorded by the last refresh_rate fix (for refresh_revert)
let fixing = false;

function remember(checks) {
  const get = (id) => checks.find(c => c.id === id) || {};
  const ref = new Map(arr(get('refresh_rate').items).map(i => [i.target, { current: i.current, max: i.max, label: i.label }]));
  const ssd = new Set(arr(get('ssd').items).filter(i => i.ssd).map(i => i.target));
  const plans = new Map(arr(get('power').plans).map(p => [String(p.guid).toLowerCase(), p.name]));
  last = { refresh: ref, ssd, plans };
}

function finish(raw) {
  const checks = analyze(raw);
  for (const c of checks) { const p = validateCheck(c); if (p.length) Object.assign(c, { status: 'unknown', why: `${c.why} (internal check problem: ${p.join('; ')})`, fix: null, items: undefined }); }
  remember(checks);
  return { checks, checkedAt: new Date().toISOString(), ms: Object.fromEntries(Object.entries(raw || {}).map(([k, v]) => [k, v && v.ms])) };
}

let pending = null;
async function hwCheck({ raw } = {}) {
  if (raw) return finish(raw);
  if (!pending) pending = ps(CHECK_SCRIPT, undefined, { timeout: 90000 }).then(finish).finally(() => { pending = null; });
  return pending;
}

// Validate a fix request against the fixed id set and the last check. Returns the exact action to run.
function planFix({ id, target } = {}) {
  if (!FIX_IDS.includes(id)) throw new Error('Unknown hardware fix');
  if (!last) throw new Error('Run the hardware check first.');
  const t = typeof target === 'string' ? target : '';
  if (id === 'refresh_rate') {
    const m = last.refresh.get(t);
    if (!m) throw new Error('That display was not found in the last check. Run the check again.');
    if (!(m.max > m.current)) throw new Error('This display already runs at its highest refresh rate.');
    return { id, device: t, rate: m.max, from: m.current, label: m.label };
  }
  if (id === 'refresh_revert') {
    if (!prevRefresh || prevRefresh.target !== t) throw new Error('There is no refresh rate change to undo for this display.');
    return { id, device: t, rate: prevRefresh.rate, label: prevRefresh.label };
  }
  if (id === 'ssd_retrim') {
    const L = t.toUpperCase();
    if (!/^[A-Z]$/.test(L) || !last.ssd.has(L)) throw new Error('That drive is not an SSD from the last check.');
    return { id, letter: L };
  }
  // power_plan
  const g = t.toLowerCase();
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(g) || !last.plans.has(g)) throw new Error('That power plan was not found in the last check.');
  return { id, guid: g, name: last.plans.get(g) };
}

// Real actions. The demo passes its own runner with the same three functions.
const realRunner = {
  async setRefresh(device, rate) {
    const r = await ps(REFRESH_SCRIPT, { device, rate }, { timeout: 30000 });
    if (!r || (r.code !== 0 && r.code !== 1)) throw new Error(`Windows could not change the refresh rate (code ${r && r.code}).`);
    return { before: r.before, after: r.after, restart: r.code === 1 };
  },
  async retrim(letter) { const r = await elevated(RETRIM_SCRIPT, { letter }, { timeout: 15 * 60e3 }); if (r && r.error) throw new Error(r.error); return { ok: true }; },
  async setPlan(guid) { const r = await elevated(POWER_SCRIPT, { guid }, { timeout: 60e3 }); if (!r || !r.ok) throw new Error('Windows did not switch the power plan.'); return { ok: true }; },
};

async function hwFix(args, runner = realRunner) {
  const p = planFix(args);
  if (fixing) throw new Error('Another hardware fix is still running.');
  fixing = true;
  try {
    if (p.id === 'refresh_rate' || p.id === 'refresh_revert') {
      const r = await runner.setRefresh(p.device, p.rate);
      const now = Number(r.after) || p.rate;
      const entry = last.refresh.get(p.device);
      if (p.id === 'refresh_rate') prevRefresh = { target: p.device, rate: Number(r.before) || p.from, label: p.label };
      else prevRefresh = null;
      if (entry) entry.current = now;
      if (now !== p.rate) throw new Error(`Windows kept ${now} Hz instead of ${p.rate} Hz.`);
      return { ok: true, id: p.id, target: p.device, rate: now, previous: p.id === 'refresh_rate' ? prevRefresh.rate : undefined, restart: !!r.restart,
        summary: p.id === 'refresh_rate' ? `Refresh rate of ${p.label} set to ${now} Hz (was ${prevRefresh.rate} Hz)` : `Refresh rate of ${p.label} set back to ${now} Hz` };
    }
    if (p.id === 'ssd_retrim') { await runner.retrim(p.letter); return { ok: true, id: p.id, target: p.letter, summary: `Ran TRIM on drive ${p.letter}:` }; }
    await runner.setPlan(p.guid);
    return { ok: true, id: p.id, target: p.guid, summary: `Power plan switched to ${p.name}` };
  } finally { fixing = false; }
}

module.exports = { hwCheck, hwFix, analyze, validateCheck, planFix, CHECK_IDS, FIX_IDS, STATUSES, _scripts: { CHECK_SCRIPT, REFRESH_SCRIPT } };
