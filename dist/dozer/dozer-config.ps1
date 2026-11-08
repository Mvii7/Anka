# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Dozer launchers: set or check keys of the [Renderer] section of an dozer-cli .ini.
# The start-*.bat launchers next to this file call it on the first start
# (powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File dozer-config.ps1) to
# prepare config\sdl2-config.ini and config\sdl2-config-reactive.ini. It only touches the one file
# named in NX2_INI, which the launcher has just made itself, or only reads it (check).
# Environment:
#   NX2_INI   the .ini file
#   NX2_SET   keys to set, "key=value|key=value" (mode set): a key's line in [Renderer] is
#             replaced, a missing key is added at the end of [Renderer] (the section is added
#             when missing). Nothing else changes: same encoding (UTF-8, with a BOM only if the
#             file had one), same line endings.
#   NX2_MODE  "set" or "check": check exits 42 when reactive flushing is on (the default, or
#             use_reactive_flushing=true), 0 when it is off.
$ErrorActionPreference = 'Stop'
$path = $env:NX2_INI
$bytes = [IO.File]::ReadAllBytes($path)
$bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
$skip = 0
if ($bom) { $skip = 3 }
$text = (New-Object Text.UTF8Encoding $false).GetString($bytes, $skip, $bytes.Length - $skip)
$nl = "`n"
if ($text.Contains("`r`n")) { $nl = "`r`n" }
$endsNl = $text.EndsWith("`n")
$lines = New-Object Collections.Generic.List[string]
foreach ($l in ($text -split "`r?`n")) { $lines.Add($l) }
if ($endsNl) { $lines.RemoveAt($lines.Count - 1) }

# [Renderer]: first line after its header and the line after its last line
$start = -1
$end = $lines.Count
for ($i = 0; $i -lt $lines.Count; $i++) {
    $t = $lines[$i].Trim()
    if ($start -lt 0) {
        if ($t -eq '[Renderer]') { $start = $i + 1 }
    } elseif ($t.StartsWith('[')) {
        $end = $i
        break
    }
}

function Get-Value([string]$key) {
    if ($start -lt 0) { return $null }
    for ($i = $start; $i -lt $end; $i++) {
        if ($lines[$i].StartsWith($key + '=')) { return $lines[$i].Substring($key.Length + 1).Trim() }
    }
    return $null
}

if ($env:NX2_MODE -eq 'check') {
    $def = Get-Value 'use_reactive_flushing\default'
    $val = Get-Value 'use_reactive_flushing'
    if ($def -eq 'false' -and $val -eq 'false') { exit 0 }
    exit 42
}

if ($start -lt 0) {
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -ne '') { $lines.Add('') }
    $lines.Add('[Renderer]')
    $start = $lines.Count
    $end = $lines.Count
}
foreach ($pair in $env:NX2_SET.Split('|')) {
    $k = $pair.Substring(0, $pair.IndexOf('='))
    $found = $false
    for ($i = $start; $i -lt $end; $i++) {
        if ($lines[$i].StartsWith($k + '=')) {
            $lines[$i] = $pair
            $found = $true
        }
    }
    if (-not $found) {
        # after the section's last non-empty line
        $at = $end
        while ($at -gt $start -and $lines[$at - 1].Trim() -eq '') { $at-- }
        $lines.Insert($at, $pair)
        $end++
    }
}
$out = [string]::Join($nl, $lines)
if ($endsNl) { $out += $nl }
$enc = New-Object Text.UTF8Encoding $bom
# written next to it first and then moved over it, so that a stop half-way leaves the file whole
$tmp = $path + '.nx2tmp'
[IO.File]::WriteAllText($tmp, $out, $enc)
Move-Item -LiteralPath $tmp -Destination $path -Force
exit 0
