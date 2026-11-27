#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Unattended boot monitor for TB710FU that keeps the serial link alive.

.DESCRIPTION
  Two hard rules learned from hardware:

  1. A desktop left running with the current renderer eventually loses COM5
     (backlight goes black, port still enumerates but never answers), which
     makes the tablet unreachable for the rest of the night.
  2. Restarts are only visible as the port going away and coming back.

  So this script never leaves the tablet booted longer than it must. After a
  short observation window it always sends `edl`, and it records the FULL raw
  serial text of every boot generation (the previous watcher only logged that
  it saw a marker, and threw the evidence away).

  Verdicts per generation:
    DESKTOP  WindowServer in ps, or disp transfers > 0
    BSD      ps answered but no WindowServer
    STUCK    port opened but the shell never answered
    NOSHELL  no buffered kernel text at all

.PARAMETER WatchMinutes      Total wall time to keep cycling. Default 20.
.PARAMETER ObserveSeconds    How long to watch one boot before forcing EDL. Default 45.
.PARAMETER MaxBoots          Stop after this many boot generations. Default 6.
#>
param(
    [int]$WatchMinutes = 20,
    [int]$ObserveSeconds = 45,
    [int]$MaxBoots = 6,
    [string]$Port = 'COM5',
    [string]$Out = ''
)

$ErrorActionPreference = 'Continue'
if (-not $Out) {
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $Out = "D:\unixnu-tb710fu-clean\scratch\boot_monitor_$stamp.log"
}
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
$rawDir = [System.IO.Path]::ChangeExtension($Out, $null) + '-raw'
if (-not (Test-Path -LiteralPath $rawDir)) { New-Item -ItemType Directory -Path $rawDir -Force | Out-Null }

function Log([string]$line) {
    $ts = (Get-Date).ToString('HH:mm:ss.fff')
    $entry = "[$ts] $line"
    Write-Host $entry
    Add-Content -LiteralPath $Out -Value $entry -Encoding UTF8
}

function Port-Present([string]$name) {
    try { return ([System.IO.Ports.SerialPort]::GetPortNames() -contains $name) } catch { return $false }
}

function Edl-Present {
    try {
        return [bool](Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_05C6&PID_9008' })
    } catch { return $false }
}

function Open-Port([string]$name, [int]$tries = 8) {
    for ($i = 1; $i -le $tries; $i++) {
        $sp = $null
        try {
            $sp = New-Object System.IO.Ports.SerialPort $name, 115200, 'None', 8, 'One'
            $sp.ReadTimeout = 250
            $sp.WriteTimeout = 500
            # DWC3 CDC only drains once the host raises DTR (docs/USB_CDC_PORT.md).
            $sp.DtrEnable = $true
            $sp.RtsEnable = $true
            $sp.Open()
            return $sp
        } catch {
            if ($sp) { try { $sp.Dispose() } catch {} }
            Start-Sleep -Milliseconds 700
        }
    }
    return $null
}

function Drain([System.IO.Ports.SerialPort]$sp, [int]$ms, [System.Text.StringBuilder]$acc) {
    $deadline = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $deadline) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk.Length -gt 0) { [void]$acc.Append($chunk) }
        } catch [TimeoutException] { }
        catch [InvalidOperationException] { break }
        Start-Sleep -Milliseconds 40
    }
}

function Send-Verb([System.IO.Ports.SerialPort]$sp, [string]$verb, [int]$ms, [System.Text.StringBuilder]$acc) {
    try {
        $sp.DiscardInBuffer()
        $sp.Write("$verb`r`n")
    } catch { return }
    Start-Sleep -Milliseconds 150
    Drain $sp $ms $acc
}

# Force the tablet back to EDL. Returns $true when 9008 shows up.
function Force-Edl([string]$name, [int]$tries = 5) {
    for ($i = 1; $i -le $tries; $i++) {
        if (Edl-Present) { return $true }
        if (-not (Port-Present $name)) { Start-Sleep -Milliseconds 800; continue }
        $sp = Open-Port $name 2
        if (-not $sp) { Start-Sleep -Milliseconds 800; continue }
        try {
            $sp.Write("edl`r`n")
            Start-Sleep -Milliseconds 900
        } catch { }
        finally { try { $sp.Close() } catch {}; try { $sp.Dispose() } catch {} }
        $deadline = (Get-Date).AddSeconds(25)
        while ((Get-Date) -lt $deadline) {
            if (Edl-Present) { return $true }
            Start-Sleep -Milliseconds 600
        }
    }
    return (Edl-Present)
}

Log "=== boot monitor start: out=$Out raw=$rawDir ==="
Log "params: WatchMinutes=$WatchMinutes ObserveSeconds=$ObserveSeconds MaxBoots=$MaxBoots"

$deadline = (Get-Date).AddMinutes($WatchMinutes)
$boots = 0
$generation = 0
$results = @()

while ((Get-Date) -lt $deadline -and $boots -lt $MaxBoots) {

    if (Edl-Present) {
        Log "device is in EDL; waiting for a boot (it only leaves EDL on a flash)"
        Start-Sleep -Seconds 3
        continue
    }

    if (-not (Port-Present $Port)) {
        Start-Sleep -Milliseconds 700
        continue
    }

    $sp = Open-Port $Port
    if (-not $sp) { Start-Sleep -Seconds 1; continue }

    $generation++
    $boots++
    $raw = New-Object System.Text.StringBuilder
    Log "BOOT #$boots open"

    $verdict = 'UNKNOWN'
    try {
        # Kernel ring replay plus a couple of seconds of live output.
        Drain $sp 4000 $raw

        $sawLaunchd = $raw.ToString() -match 'launchd'
        $sawAmfi = $raw.ToString() -match 'AMFI pointer initialized'
        $sawKernel = $raw.ToString() -match 'Darwin Kernel Version'
        Log "  kernel=$sawKernel launchd=$sawLaunchd amfi=$sawAmfi"

        # Observe until the window ends, probing periodically, but force EDL
        # the moment a desktop appears: a long desktop session is what loses
        # COM5 on this board.
        $observeUntil = (Get-Date).AddSeconds($ObserveSeconds)
        $desktop = $false
        $psAnswered = $false
        while ((Get-Date) -lt $observeUntil) {
            if (-not (Port-Present $Port)) { break }
            Send-Verb $sp 'ps' 2200 $raw
            if ($raw.ToString() -match 'WindowServer') { $desktop = $true; Log "  ps: WindowServer PRESENT"; break }
            if ($raw.ToString() -match 'TB710FU SHELL: ps unavailable' -or $raw.ToString() -match '^.*\bps\b.*pid.*') { }
            if ($raw.ToString() -match 'uid\s+pid' -or $raw.ToString() -match 'PID\s+COMMAND') { $psAnswered = $true }
            Send-Verb $sp 'disp' 1600 $raw
            $m = [regex]::Match($raw.ToString(), 'transfers=(\d+)')
            if ($m.Success -and [int]$m.Groups[1].Value -gt 0) {
                $desktop = $true
                Log "  disp: transfers=$($m.Groups[1].Value)"
                break
            }
            Start-Sleep -Milliseconds 400
        }

        $rawText = $raw.ToString()
        if (-not $psAnswered -and $rawText -match '\bps\b') { $psAnswered = $true }
        if ($desktop) { $verdict = 'DESKTOP' }
        elseif ($psAnswered) { $verdict = 'BSD' }
        elseif ($rawText.Length -eq 0) { $verdict = 'NOSHELL' }
        elseif ($sawKernel) { $verdict = 'STUCK' }
        else { $verdict = 'NOSHELL' }
    }
    finally {
        try { $sp.Close() } catch {}
        try { $sp.Dispose() } catch {}
    }

    $rawPath = Join-Path $rawDir ("gen{0:D2}-{1}.log" -f $generation, $verdict)
    try { [System.IO.File]::WriteAllText($rawPath, $raw.ToString()) } catch {}
    Log "  VERDICT $verdict  raw=$rawPath  bytes=$($raw.Length)"
    $results += "$verdict"

    # Always return to EDL: never leave a booted desktop running unattended.
    $ok = Force-Edl $Port
    if ($ok) { Log "  back in EDL" } else { Log "  EDL NOT reached - port may be lost" }
}

Log "=== monitor end ==="
Log "boots=$boots results=$($results -join ',') edl=$(Edl-Present)"
Write-Host "`nlog: $Out"
