#!/usr/bin/env pwsh
# Decode the FAT header of /usr/lib/dyld captured from the device over CDC cat.
#
# Purpose: dyld is itself a FAT binary. If the arm64e slice's segment
# alignment (2^14 = 16 KB on Apple silicon) does not match what a 4 KB-page
# kernel can map, dyld could fail to map the shared cache in a way that
# produces the observed openat->..->mmap->munmap retry loop.
#
# This prints every arch slice and then walks the arm64e slice's load commands
# to show segment alignments.
param(
    [string]$LogFile
)

if (-not $LogFile) {
    $LogFile = (Get-ChildItem "$PSScriptRoot" -Filter 'dyld_header_*.log' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $LogFile -or -not (Test-Path -LiteralPath $LogFile)) {
    Write-Error "no dyld_header log found"
    exit 1
}
Write-Host "source: $LogFile"

$text = Get-Content -LiteralPath $LogFile -Raw

# cut out the CAT region
$i = $text.IndexOf('CAT: begin')
if ($i -lt 0) { Write-Error "CAT: begin not found"; exit 1 }
$seg = $text.Substring($i)
$e = $seg.IndexOf('TB710FU SHELL')
if ($e -gt 0) { $seg = $seg.Substring(0, $e) }

# find the FAT magic in its escaped form
$j = $seg.IndexOf('\xca\xfe')
if ($j -lt 0) { Write-Error "FAT magic not found in output"; exit 1 }
$seg = $seg.Substring($j)

# unescape \xNN
$sb = New-Object System.Text.StringBuilder
for ($k = 0; $k -lt $seg.Length; $k++) {
    $ch = $seg[$k]
    if ($ch -eq '\' -and ($k + 3) -lt $seg.Length -and $seg[$k + 1] -eq 'x') {
        $hex = $seg.Substring($k + 2, 2)
        [void]$sb.Append([char][Convert]::ToInt32($hex, 16))
        $k += 3
    } else {
        [void]$sb.Append($ch)
    }
}
$bytes = [System.Text.Encoding]::GetEncoding(28591).GetBytes($sb.ToString())
Write-Host ("unescaped bytes: {0}" -f $bytes.Length)

function BE32([int]$o) {
    return ([uint32]$bytes[$o] -shl 24) -bor ([uint32]$bytes[$o+1] -shl 16) -bor ([uint32]$bytes[$o+2] -shl 8) -bor [uint32]$bytes[$o+3]
}
function LE32([int]$o) {
    return ([uint32]$bytes[$o+3] -shl 24) -bor ([uint32]$bytes[$o+2] -shl 16) -bor ([uint32]$bytes[$o+1] -shl 8) -bor [uint32]$bytes[$o]
}
function LE64([int]$o) {
    $lo = [uint64](LE32 $o)
    $hi = [uint64](LE32 ($o + 4))
    return ($hi -shl 32) -bor $lo
}

$magic = BE32 0
Write-Host ("fat magic: 0x{0:x8}  (cafebabe = FAT32 big-endian)" -f $magic)
$nfat = BE32 4
Write-Host ("nfat_arch: {0}" -f $nfat)

$arm64eOff = -1
$arm64eSize = 0
for ($a = 0; $a -lt $nfat; $a++) {
    $o = 8 + $a * 20
    if (($o + 20) -gt $bytes.Length) { break }
    $cpu = BE32 $o
    $sub = BE32 ($o + 4)
    $off = BE32 ($o + 8)
    $sz  = BE32 ($o + 12)
    $al  = BE32 ($o + 16)
    Write-Host ("  arch{0}: cputype=0x{1:x8} subtype=0x{2:x8} offset=0x{3:x} size=0x{4:x} align=2^{5} ({6} bytes)" -f $a, $cpu, $sub, $off, $sz, $al, ([math]::Pow(2, $al)))
    if ($cpu -eq 0x0100000c) { $arm64eOff = $off; $arm64eSize = $sz }
}

if ($arm64eOff -lt 0) {
    Write-Host ""
    Write-Host "arm64e slice is beyond the 4096 bytes CDC cat returned; header only."
    exit 0
}

Write-Host ""
Write-Host ("=== arm64e slice at file offset 0x{0:x} ===" -f $arm64eOff)
$m = LE32 $arm64eOff
Write-Host ("mach magic: 0x{0:x8}  (feedfacf = MH_MAGIC_64)" -f $m)
$cputype = LE32 ($arm64eOff + 4)
$ncmds = LE32 ($arm64eOff + 16)
$sizeofcmds = LE32 ($arm64eOff + 20)
Write-Host ("cputype=0x{0:x8} ncmds={1} sizeofcmds={2}" -f $cputype, $ncmds, $sizeofcmds)

$off2 = $arm64eOff + 32
for ($c = 0; $c -lt $ncmds; $c++) {
    if (($off2 + 8) -gt $bytes.Length) { break }
    $cmd = LE32 $off2
    $cmdsize = LE32 ($off2 + 4)
    if ($cmd -eq 0x19) {
        # LC_SEGMENT_64
        $segname = [System.Text.Encoding]::ASCII.GetString($bytes[$off2+8..$off2+23]).TrimEnd([char]0)
        $vmsize = LE64 ($off2 + 32)
        $fileoff = LE64 ($off2 + 40)
        $filesize = LE64 ($off2 + 48)
        Write-Host ("  seg {0,-16} vmaddr=0x{1:x} vmsize=0x{2:x} fileoff=0x{3:x} filesize=0x{4:x}" -f $segname, (LE64 ($off2+24)), $vmsize, $fileoff, $filesize)
    }
    $off2 += $cmdsize
    if ($cmdsize -le 0) { break }
}
