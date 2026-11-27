#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Decide, from a raw TB710FU serial capture, whether the boot reached macOS
  userspace, and where it stopped if it did not.

.DESCRIPTION
  Marks verified against every capture on hand (5 good, 4 stalled):

    good     : "unnest of range ... DYLD shared region"     28..50 lines
               "dyld cache mapped system-wide"               1 line
    stalled  : neither                                          0 lines

  So those two strings are the reliable boundary, not "check_np errno 12"
  (which appears in good boots too and means nothing).

  Prints one summary line per file plus an overall verdict. Read-only.
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Paths
)

function Classify([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) {
        return [pscustomobject]@{ File = (Split-Path $path -Leaf); Verdict = 'MISSING'; Unnest = 0; Mapped = 0; Lines = 0; LastInteresting = '' }
    }
    $lines = Get-Content -LiteralPath $path -Encoding UTF8
    $unnest = 0; $mapped = 0; $last = ''
    foreach ($l in $lines) {
        if ($l -match 'unnest of range') { $unnest++; continue }
        if ($l -match 'dyld cache mapped system-wide') { $mapped++; continue }
        # Keep the last line that says something about where we are.
        if ($l -match 'AMFI pointer initialized|check_np|launchd|GopDisplay: registered|WindowServer|exit: pid 1') {
            $last = $l.Trim()
        }
    }
    $verdict = if ($mapped -gt 0 -or $unnest -gt 0) { 'USERSPACE' } else { 'STALLED' }
    return [pscustomobject]@{
        File            = (Split-Path $path -Leaf)
        Verdict         = $verdict
        Unnest          = $unnest
        Mapped          = $mapped
        Lines           = $lines.Count
        LastInteresting = if ($last.Length -gt 72) { $last.Substring(0, 72) } else { $last }
    }
}

if (-not $Paths -or $Paths.Count -eq 0) {
    $root = Split-Path -Parent $PSScriptRoot
    $Paths = @(Get-ChildItem -LiteralPath (Join-Path $root 'scratch') -Filter 'com5_*.log' -File |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 20 -ExpandProperty FullName)
}

$rows = @()
foreach ($p in $Paths) { $rows += Classify $p }

$rows | Format-Table File, Verdict, Unnest, Mapped, Lines, LastInteresting -AutoSize

$user = ($rows | Where-Object { $_.Verdict -eq 'USERSPACE' }).Count
$stall = ($rows | Where-Object { $_.Verdict -eq 'STALLED' }).Count
Write-Host ""
Write-Host "USERSPACE=$user STALLED=$stall"
if ($user -gt 0) { Write-Host "at least one capture reached userspace" } else { Write-Host "no capture reached userspace" }
