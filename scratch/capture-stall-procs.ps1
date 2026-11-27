#!/usr/bin/env pwsh
<#
.SYNOPSIS
  When a boot stalls, capture the kernel's process table over CDC and save it.

.DESCRIPTION
  Key observation from hardware (scratch/com5_watch_20261128-032323.log):

    [03:23:31] ps: answered, no WindowServer

  The `ps` verb answers even while launchd is stuck. That means proc_find()
  and proc_name() still work, so the process table is readable in the stalled
  state. This is the only way to see WHAT launchd became without a debugger.

  The earlier watcher threw that output away and only recorded "answered".
  This tool keeps every byte.

  Reads: sends `ps`, then `disp`, then `help` (to see which verbs are alive),
  and writes each reply to its own file plus one combined transcript.

.PARAMETER Port
  Serial port, default COM5.

.PARAMETER OutDir
  Where to write. Defaults to scratch/stall-<timestamp>.
#>
param(
    [string]$Port = 'COM5',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Continue'
if (-not $OutDir) {
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) "scratch\stall-$stamp"
}
if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

function Say([string]$m) { Write-Host $m }

if (-not ([System.IO.Ports.SerialPort]::GetPortNames() -contains $Port)) {
    Say "port $Port not present; nothing to capture"
    exit 2
}

$sp = $null
for ($i = 1; $i -le 6; $i++) {
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
        $sp.ReadTimeout = 250
        $sp.WriteTimeout = 600
        $sp.DtrEnable = $true
        $sp.RtsEnable = $true
        $sp.Open()
        break
    } catch {
        if ($sp) { try { $sp.Dispose() } catch {}; $sp = $null }
        Start-Sleep -Milliseconds 700
    }
}
if (-not $sp) { Say "could not open $Port"; exit 2 }

function Drain([int]$ms, [System.Text.StringBuilder]$acc) {
    $deadline = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $deadline) {
        try {
            $c = $sp.ReadExisting()
            if ($c.Length -gt 0) { [void]$acc.Append($c) }
        } catch [TimeoutException] { }
        catch [InvalidOperationException] { break }
        Start-Sleep -Milliseconds 40
    }
}

$all = New-Object System.Text.StringBuilder

try {
    # Ring replay first.
    $boot = New-Object System.Text.StringBuilder
    Drain 4000 $boot
    [void]$all.Append("=== BOOT RING REPLAY ===`n").Append($boot.ToString()).Append("`n")
    [System.IO.File]::WriteAllText((Join-Path $OutDir '00-ring-replay.log'), $boot.ToString())

    foreach ($verb in @('ps', 'disp', 'help')) {
        $reply = New-Object System.Text.StringBuilder
        try {
            $sp.DiscardInBuffer()
            $sp.Write("$verb`r`n")
        } catch { continue }
        Start-Sleep -Milliseconds 200
        # ps dumps many lines; give it longer.
        $ms = if ($verb -eq 'ps') { 6000 } else { 2500 }
        Drain $ms $reply
        $text = $reply.ToString()
        [void]$all.Append("=== $($verb.ToUpper()) ($($text.Length) bytes) ===`n").Append($text).Append("`n")
        [System.IO.File]::WriteAllText((Join-Path $OutDir "$verb.log"), $text)
        Say "$verb -> $($text.Length) bytes"
    }
}
finally {
    try { $sp.Close() } catch {}
    try { $sp.Dispose() } catch {}
}

$combined = Join-Path $OutDir 'transcript.log'
[System.IO.File]::WriteAllText($combined, $all.ToString())

# Summarise what the process table showed.
$psText = if (Test-Path (Join-Path $OutDir 'ps.log')) { Get-Content (Join-Path $OutDir 'ps.log') -Raw } else { '' }
$procs = [regex]::Matches($psText, 'name=(\S+)\s+thr=(-?\d+)\s+fp=(\d+)')
Say ""
Say "process entries: $($procs.Count)"
$interesting = @('launchd', 'WindowServer', 'loginwindow', 'logd', 'cfprefsd', 'opendirectoryd', 'seputil')
foreach ($n in $interesting) {
    $m = $procs | Where-Object { $_.Groups[1].Value -eq $n } | Select-Object -First 1
    if ($m) { Say ("  {0,-18} thr={1} fp={2}" -f $n, $m.Groups[2].Value, $m.Groups[3].Value) }
    else { Say ("  {0,-18} ABSENT" -f $n) }
}

Say ""
Say "wrote $OutDir"
