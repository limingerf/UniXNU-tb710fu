#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Watch COM5 across reboots and decide, objectively, whether the tablet
  reached the macOS desktop.

.DESCRIPTION
  The tablet's CDC ACM port disappears whenever the kernel restarts, so the
  port going away and coming back IS the reboot signal. This script:

    * subscribes to serial-port arrival/removal,
    * records every connect/disconnect with a timestamp,
    * probes the device after each (re)connect with the CDC shell verbs
      (ps / disp / ioreg) to see how far the boot got,
    * writes one append-only log and prints a verdict.

  Verdicts:
    DESKTOP   - WindowServer/loginwindow seen in ps AND disp shows transfers
    PARTIAL   - boot advanced but no WindowServer (stalled before desktop)
    SILENT    - port present but the shell never answered (kernel stuck)
    BOOTLOOP  - the port came back more than once (restart detected)

.PARAMETER Minutes
  How long to watch. Default 15.

.PARAMETER Port
  Serial port name. Default COM5.

.PARAMETER Probe
  Send ps/disp after each connect. Default on.
#>
param(
    [int]$Minutes = 15,
    [string]$Port = 'COM5',
    [switch]$NoProbe,
    [string]$Out = ''
)

$ErrorActionPreference = 'Continue'
if (-not $Out) {
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $Out = "D:\unixnu-tb710fu-clean\scratch\com5_watch_$stamp.log"
}
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

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

# Open with DTR/RTS raised: the DWC3 CDC only starts draining when the host
# asserts DTR (see docs/USB_CDC_PORT.md), so opening without them yields
# silence that looks like a hung kernel.
function Open-Port([string]$name, [int]$tries = 6) {
    for ($i = 1; $i -le $tries; $i++) {
        $sp = $null
        try {
            $sp = New-Object System.IO.Ports.SerialPort $name, 115200, 'None', 8, 'One'
            $sp.ReadTimeout = 250
            $sp.WriteTimeout = 500
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

function Drain([System.IO.Ports.SerialPort]$sp, [int]$ms) {
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $deadline) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk.Length -gt 0) { [void]$sb.Append($chunk) }
        } catch [TimeoutException] { }
        catch [InvalidOperationException] { break }
        Start-Sleep -Milliseconds 40
    }
    return $sb.ToString()
}

# Ask the CDC shell for state. Returns the raw reply text.
function Probe([System.IO.Ports.SerialPort]$sp, [string]$verb, [int]$ms = 2500) {
    try {
        $sp.DiscardInBuffer()
        $sp.Write("$verb`r`n")
    } catch { return '' }
    Start-Sleep -Milliseconds 150
    return (Drain $sp $ms)
}

Log "=== watch start: port=$Port minutes=$Minutes out=$Out ==="

$end = (Get-Date).AddMinutes($Minutes)
$reconnects = 0
$sawDesktop = $false
$desktopEvidence = ''
$lastText = ''

# Track the current port generation. A new generation means the kernel
# restarted underneath us.
while ((Get-Date) -lt $end) {
    if (-not (Port-Present $Port)) {
        if (Edl-Present) {
            Log "PORT-GONE; device is in EDL (9008) - not booting"
            Start-Sleep -Seconds 2
            continue
        }
        Log "PORT-GONE (kernel restart or power loss)"
        # Wait for it to come back.
        $waitUntil = (Get-Date).AddSeconds(90)
        $back = $false
        while ((Get-Date) -lt $waitUntil) {
            if (Port-Present $Port) { $back = $true; break }
            if (Edl-Present) { Log "left for EDL while waiting"; break }
            Start-Sleep -Milliseconds 500
        }
        if ($back) {
            $reconnects++
            Log "PORT-BACK (restart #$reconnects)"
        }
        continue
    }

    $sp = Open-Port $Port
    if (-not $sp) {
        Log "open failed; retrying"
        Start-Sleep -Seconds 1
        continue
    }
    Log "PORT-OPEN"

    try {
        # First seconds after open: the kernel ring replays whatever it buffered.
        $bootText = Drain $sp 4000
        if ($bootText.Length -gt 0) {
            $lastText = $bootText
            foreach ($pat in @('Darwin Kernel Version', 'launchd', 'WindowServer', 'AMFI pointer initialized')) {
                if ($bootText -match [regex]::Escape($pat)) { Log "  saw: $pat" }
            }
        } else {
            Log "  (no buffered text)"
        }

        if (-not $NoProbe) {
            $ps = Probe $sp 'ps' 3500
            if ($ps -match 'WindowServer') { $sawDesktop = $true; $desktopEvidence = 'ps:WindowServer' ; Log "  ps: WindowServer PRESENT" }
            elseif ($ps -match 'TB710FU SHELL: ps unavailable') { Log "  ps: unavailable (no BSD yet)" }
            elseif ($ps.Length -gt 0) { Log "  ps: answered, no WindowServer" }
            else { Log "  ps: no answer (kernel stuck or shell not up)" }

            $disp = Probe $sp 'disp' 2500
            if ($disp -match 'transfers=(\d+)') {
                $n = [int]$Matches[1]
                Log "  disp: transfers=$n"
                if ($n -gt 0) { $sawDesktop = $true; if (-not $desktopEvidence) { $desktopEvidence = "disp:transfers=$n" } }
            } elseif ($disp.Length -eq 0) {
                Log "  disp: no answer"
            } else {
                Log "  disp: no transfer counter"
            }
        }
    } finally {
        try { $sp.Close() } catch {}
        try { $sp.Dispose() } catch {}
    }

    # Stay attached to this generation until the port actually disappears.
    while ((Get-Date) -lt $end -and (Port-Present $Port)) {
        Start-Sleep -Milliseconds 800
    }
}

Log "=== watch end ==="
Log "reconnects=$reconnects sawDesktop=$sawDesktop evidence=$desktopEvidence"

if ($sawDesktop) {
    Log "VERDICT: DESKTOP (reached userspace; reconnects=$reconnects)"
} elseif ($reconnects -ge 1) {
    Log "VERDICT: BOOTLOOP (port returned $reconnects time(s) without desktop)"
} elseif ($lastText -match 'launchd') {
    Log "VERDICT: PARTIAL (reached launchd, no WindowServer observed)"
} else {
    Log "VERDICT: SILENT (never got a usable shell)"
}
Write-Host "`nlog: $Out"
