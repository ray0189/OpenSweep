'use strict';
// Shared PowerShell runner used by every Windows backend module.
const { execFile, spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');

const PS = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
const PRELUDE = `$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.Encoding]::UTF8
function Out-Json($x){ ConvertTo-Json -InputObject $x -Depth 6 -Compress }
# Fast folder size (.NET, skips links, never throws). Stops after $budgetMs and returns what it counted.
function Get-DirSize([string]$p, [int]$budgetMs = 4000) {
  $sw = [Diagnostics.Stopwatch]::StartNew(); $total = [double]0; $stack = New-Object System.Collections.Stack; $stack.Push($p)
  while ($stack.Count -gt 0 -and $sw.ElapsedMilliseconds -lt $budgetMs) {
    $d = $stack.Pop()
    try { foreach ($f in [IO.Directory]::EnumerateFiles($d)) { try { $total += (New-Object IO.FileInfo $f).Length } catch {} } } catch {}
    try { foreach ($s in [IO.Directory]::EnumerateDirectories($d)) { try { if (-not ([IO.File]::GetAttributes($s) -band [IO.FileAttributes]::ReparsePoint)) { $stack.Push($s) } } catch {} } } catch {}
  }
  return $total
}
`;

// Scripts that run elevated must live in a folder only administrators can write to (see
// security-audit S2). main.js hardens %ProgramData%\OpenSweep\run at startup and calls setScriptDir.
let scriptDir = null;
function setScriptDir(d) { scriptDir = d; }
function tmpFile(ext) {
  return path.join(scriptDir || os.tmpdir(), `opensweep-${crypto.randomBytes(8).toString('hex')}${ext}`);
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

// Run a script as the built-in SYSTEM account (one-shot scheduled task). Used only as a
// fallback when Windows denies administrators, e.g. policy keys newer Windows 11 builds lock.
async function asSystem(script, args, { timeout = 120000 } = {}) {
  const dir = path.join(process.env.ProgramData || 'C:\\ProgramData', 'OpenSweep');
  fs.mkdirSync(dir, { recursive: true });
  const id = crypto.randomBytes(6).toString('hex');
  const resultFile = path.join(dir, `sys-${id}.json`);
  const file = path.join(dir, `sys-${id}.ps1`);
  fs.writeFileSync(file, '\ufeff' + withArgs(`function Out-Result($x){ Out-Json $x | Out-File -LiteralPath '${resultFile}' -Encoding utf8 }\ntry {\n${script}\n} catch { Out-Result @{ error = $_.Exception.Message } }`, args), 'utf8');
  const name = `OpenSweep-${id}`;
  try {
    await elevated(`
$act = New-ScheduledTaskAction -Execute '${PS}' -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $a.file + '"')
$pr = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName $a.name -Action $act -Principal $pr -Force -ErrorAction Stop | Out-Null
Start-ScheduledTask -TaskName $a.name
$t0 = Get-Date
while (-not (Test-Path -LiteralPath $a.result) -and ((Get-Date) - $t0).TotalSeconds -lt ${Math.round(timeout / 1000)}) { Start-Sleep -Milliseconds 300 }
Unregister-ScheduledTask -TaskName $a.name -Confirm:$false -ErrorAction SilentlyContinue
Out-Result @{ ok = $true }`, { file, name, result: resultFile }, { timeout: timeout + 30000 });
    const text = fs.readFileSync(resultFile, 'utf8').replace(/^\ufeff/, '').trim();
    const r = text ? JSON.parse(text) : null;
    if (r && r.error && Object.keys(r).length === 1) throw new Error(r.error);
    return r;
  } finally {
    fs.rm(file, { force: true }, () => {});
    fs.rm(resultFile, { force: true }, () => {});
  }
}


// Long-running script that prints one JSON object per line (live views). Returns stop().
function stream(script, args, onData, onExit) {
  const file = writeScript(withArgs(script, args));
  const p = spawn(PS, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', file], { windowsHide: true });
  let buf = '', err = '';
  p.stderr.on('data', d => { err = (err + d.toString('utf8')).slice(-4000); }); // always drain stderr so the script can't block
  p.stdout.on('data', d => {
    buf += d.toString('utf8');
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
      if (line.startsWith('{') || line.startsWith('[')) { try { onData(JSON.parse(line)); } catch { /* partial */ } }
    }
  });
  p.on('exit', (code) => { fs.rm(file, { force: true }, () => {}); if (onExit) onExit(code, err); });
  return () => { try { p.kill(); } catch { /* gone */ } };
}

module.exports = { setScriptDir, PS, ps, elevated, isAdmin, asSystem, stream, tmpFile, withArgs, writeScript };
