#!/usr/bin/env pwsh
<#
.SYNOPSIS
  After a Firehose flash+reset, prove the tablet actually booted.

.DESCRIPTION
  edl_flash_b.ps1 ends by sending Firehose `<power value="reset"/>` and
  printing the ACK, then exits. It never checks that the device came back.

  That gap is exactly what round 13 hit: pl100 was flashed at 03:46:41,
  the reset was ACKed, and then the tablet never enumerated anything for
  26+ minutes -- no COM5, no 9008, no unknown USB device. The flash script
  reported success because it only cares about writes.

  This script asserts the real outcome:

    9008 present            -> reset did not take; device still in EDL
    COM5 appears            -> kernel is running, CDC enumerated  (GOOD)
    nothing for N seconds   -> device did not boot; physical check needed

  It does NOT flash or write anything.

.PARAMETER TimeoutSeconds
  How long to wait for COM5. Default 180. pl93-class boots took ~60-90 s.

.PARAMETER Port
  CDC port name to wait for. Default COM5.
#>
param(
    [int]$TimeoutSeconds = 180,
    [string]$Port = 'COM5'
)

function Edl-Present {
    try {
        return [bool](Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_05C6&PID_9008' })
    } catch { return $false }
}

function Port-Present([string]$name) {
    try { return ([System.IO.Ports.SerialPort]::GetPortNames() -contains $name) } catch { return $false }
}

function AnyTabletUsb {
    try {
        return @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_05C6|VID_1209|VID_18D1' })
    } catch { return @() }
}

Write-Host "waiting up to ${TimeoutSeconds}s for the tablet to boot..."

$start = Get-Date
$sawEdlStill = $false
while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSeconds) {

    if (Port-Present $Port) {
        $elapsed = [int]((Get-Date) - $start).TotalSeconds
        Write-Host ""
        Write-Host "RESULT: BOOTED - $Port appeared after ${elapsed}s"
        Write-Host "kernel is running and the CDC enumerated."
        exit 0
    }

    if (Edl-Present) {
        if (-not $sawEdlStill) {
            Write-Host "  still in EDL (9008); reset has not taken effect yet"
            $sawEdlStill = $true
        }
    } else {
        $usb = AnyTabletUsb
        if ($usb.Count -gt 0) {
            $names = ($usb | ForEach-Object { $_.FriendlyName }) -join '; '
            Write-Host "  tablet visible as: $names"
        }
    }

    Start-Sleep -Seconds 2
}

Write-Host ""
if (Edl-Present) {
    Write-Host "RESULT: STILL IN EDL - the power reset did not boot the device."
    Write-Host "Action: re-issue the flash (it will reset again), or hold power to restart."
    exit 2
}

$usb = AnyTabletUsb
if ($usb.Count -eq 0) {
    Write-Host "RESULT: NO USB AT ALL after ${TimeoutSeconds}s."
    Write-Host "Not EDL, not CDC, not even an unknown device."
    Write-Host ""
    Write-Host "This is the state that needs a human:"
    Write-Host "  - screen black with no backlight -> hold power ~10 s, then power on"
    Write-Host "  - stuck in UEFI menu             -> select the xnu entry"
    Write-Host "  - battery flat                   -> charge, then power on"
    exit 3
}

Write-Host "RESULT: USB PRESENT BUT NO COM5"
$usb | Select-Object FriendlyName, Status, InstanceId | Format-Table -AutoSize
Write-Host "Device enumerated but the CDC driver did not come up."
exit 4
