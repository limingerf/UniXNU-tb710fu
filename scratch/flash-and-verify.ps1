#!/usr/bin/env pwsh
<#
.SYNOPSIS
  One command: flash a known-good image to slot_b and prove the tablet boots.

.DESCRIPTION
  Combines the two steps that were always run by hand and, more importantly,
  fixes the gap that cost 30+ minutes in round 13: edl_flash_b.ps1 ACKs the
  Firehose power reset and exits without checking that the device came back.

  Presets cover the two images worth returning to:

    pl93  last image confirmed to reach the macOS desktop on hardware
          (first swap accepted reason=0, 583 frames, 0 rejected).
          vbmeta da1d7555..., vendor_boot 0b1725a1...

    pl100 baseline with patch 0052's SRMAP trace, so a boot attempt tells us
          whether shared_region_map_and_slide_2_np is even called.
          vbmeta ecac2cc7..., vendor_boot 5bf81eab...

  boot_b is NEVER touched: it carries the 5G UEFI (988dace74...), and
  edl_flash_b.ps1 only flashes boot_b when -BootPath is passed.

.PARAMETER Preset
  pl93 | pl100  (default pl93 -- the safe recovery target)

.PARAMETER SkipBootCheck
  Flash only; do not wait for COM5 afterwards.
#>
param(
    [ValidateSet('pl93', 'pl100')]
    [string]$Preset = 'pl93',
    [switch]$SkipBootCheck,
    [int]$BootTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'

$FLASH = 'C:\Users\baiti\.gemini\antigravity\brain\e6e35059-f62d-4cac-979b-5de0846d0f50\scratch\edl_flash_b.ps1'
$VERIFY = 'D:\unixnu-tb710fu-clean\scratch\verify-boot-after-flash.ps1'

$presets = @{
    pl93  = @{
        Dir    = 'D:\unixnu-tb710fu-clean\out\pl16-46021085431-pl93-5g-srgb-signed'
        Note   = 'last image confirmed to reach the macOS desktop'
        Vbmeta = 'da1d75552de764b0a96455d8af1b7a04ad118e24f4ab724fe9f8dbe2726d1416'
        Vendor = '0b1725a12ea02d16e16525c2dd4eb91c1b465a45bc0a1eab6f010e778ab58d9d'
    }
    pl100 = @{
        Dir    = 'D:\unixnu-tb710fu-clean\out\pl16-46058104010-pl100-srmap-signed'
        Note   = 'baseline + SRMAP trace (patch 0052)'
        Vbmeta = 'ecac2cc7e0c54c657000db531d4e04efb04aede07ccc41b0fb5aa82ef2de6631'
        Vendor = '5bf81eab8de968a4407717451162475c1335ff9cd912a33556cc7bb456403cb1'
    }
}

$p = $presets[$Preset]
$vbmeta = Join-Path $p.Dir 'vbmeta.img'
$vendor = Join-Path $p.Dir 'vendor_boot.img'

Write-Host "preset : $Preset  ($($p.Note))"
Write-Host "dir    : $($p.Dir)"

foreach ($pair in @(@('vbmeta.img', $vbmeta, $p.Vbmeta), @('vendor_boot.img', $vendor, $p.Vendor))) {
    $name, $path, $want = $pair
    if (-not (Test-Path -LiteralPath $path)) { throw "$name missing at $path" }
    $have = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
    if ($have -ne $want) {
        throw ("{0} hash mismatch`n  have {1}`n  want {2}`nRefusing to flash a modified image." -f $name, $have, $want)
    }
    Write-Host "  $name ok  $($have.Substring(0,16))..."
}

if (-not (Test-Path -LiteralPath $FLASH)) { throw "flash script missing: $FLASH" }

Write-Host ""
Write-Host "flashing (boot_b is NOT touched)..."
& $FLASH -VbmetaPath $vbmeta -VendorBootPath $vendor

Write-Host ""
Write-Host "flash finished. NOTE: the flash script ACKs the power reset but does"
Write-Host "not verify the device came back -- that check is the next step."

if ($SkipBootCheck) {
    Write-Host "skipping boot verification (-SkipBootCheck)"
    exit 0
}

Write-Host ""
& $VERIFY -TimeoutSeconds $BootTimeoutSeconds
exit $LASTEXITCODE
