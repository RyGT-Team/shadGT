# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory = $true)][string]$Probe)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore
Add-Type -AssemblyName WindowsBase

# Build a one-tile PJXR using Windows' lossless JPEG XR encoder. All pixels are synthetic.
$bgr = [byte[]](@(31, 93, 201) * 256)
$bitmap = [Windows.Media.Imaging.BitmapSource]::Create(16, 16, 96, 96,
    [Windows.Media.PixelFormats]::Bgr24, $null, $bgr, 48)
$codec = [Windows.Media.Imaging.WmpBitmapEncoder]::new()
$codec.Lossless = $true
$codec.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
$stream = [IO.MemoryStream]::new()
$codec.Save($stream)
$tile = $stream.ToArray()
$stream.Dispose()
$fixture = [byte[]]::new(80 + $tile.Length)
[Text.Encoding]::ASCII.GetBytes('PJXR').CopyTo($fixture, 0)
foreach ($offset in @(8, 12)) { [BitConverter]::GetBytes([uint32]16).CopyTo($fixture, $offset) }
foreach ($offset in @(16, 18)) { [BitConverter]::GetBytes([uint16]1).CopyTo($fixture, $offset) }
[BitConverter]::GetBytes([uint32]1).CopyTo($fixture, 24)
[BitConverter]::GetBytes([uint32]80).CopyTo($fixture, 32)
$tile.CopyTo($fixture, 80)

function Wait-Until([scriptblock]$Check, [string]$Message) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not (& $Check)) {
        if ($child.HasExited -or $timer.Elapsed.TotalSeconds -gt 40) { throw $Message }
        Start-Sleep -Milliseconds 150
    }
}

$root = [IO.Directory]::CreateDirectory((Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid()))).FullName
$child = $null
try {
    $game = [IO.Directory]::CreateDirectory((Join-Path $root 'game')).FullName
    $legacy = [IO.Directory]::CreateDirectory((Join-Path $game 'download0/APP_DATA/photo')).FullName
    $serial = Join-Path $game 'user/savedata/TEST00001/download0/APP_DATA/photo'
    $photos = Join-Path $game 'Photos'
    [IO.File]::WriteAllText($photos, 'occupied')
    [IO.File]::WriteAllBytes((Join-Path $legacy 'pht__historical.jxr'), $fixture)
    $program = Join-Path $game 'probe.exe'
    Copy-Item -LiteralPath $Probe -Destination $program
    $start = [Diagnostics.ProcessStartInfo]::new($program)
    $start.WorkingDirectory = $root
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['SHADGT_PHOTO_EXPORT'] = '1'
    $child = [Diagnostics.Process]::Start($start)

    # Only final, full-sized photos belong in the output.
    $temp = Join-Path $legacy '001.jxr'
    [IO.File]::WriteAllBytes($temp, $fixture)
    [IO.File]::WriteAllBytes((Join-Path $legacy 'pht__test_t.jxr'), $fixture)
    $first = Join-Path $legacy 'pht__one.jxr'
    $second = Join-Path $serial 'pht__two.jxr'
    $startTime = [DateTime]::UtcNow
    # Change the truncated file's revision until the watcher sees it. This also tests a
    # title-scoped gallery that may not exist when its first polling pass runs.
    [IO.Directory]::CreateDirectory($serial) | Out-Null
    [IO.File]::WriteAllBytes($second, [byte[]](1, 2, 3))
    do {
        $line = $child.StandardError.ReadLineAsync()
        Wait-Until {
            [IO.File]::SetLastWriteTimeUtc($second, [DateTime]::UtcNow)
            $line.IsCompleted
        } 'The watcher did not notice a partial gallery file'
    } until ($line.Result -match 'pht__two\.jxr.*will retry')
    [IO.File]::Move($temp, $first)
    [IO.File]::SetLastWriteTimeUtc($first, $startTime)
    do {
        $line = $child.StandardError.ReadLineAsync()
        Wait-Until { $line.IsCompleted } 'The watcher did not retry when Photos was blocked'
    } until ($line.Result -match 'pht__one\.jxr.*will retry')
    [IO.File]::WriteAllBytes($second, $fixture)
    [IO.File]::SetLastWriteTimeUtc($second, $startTime)
    if ([IO.File]::ReadAllText($photos) -ne 'occupied') {
        throw 'Destination was overwritten'
    }
    [IO.File]::Delete($photos)
    Wait-Until { [IO.Directory]::Exists($photos) -and [IO.Directory]::GetFiles($photos, '*.png').Length -eq 2 } 'Two PNGs were not published'
    Start-Sleep -Seconds 3
    $names = @([IO.Directory]::GetFiles($photos, '*.png') | Sort-Object)
    if (-not ($names -match ' \(2\)\.png$')) { throw 'Missing collision suffix' }
    foreach ($path in $names) {
        $image = [IO.File]::OpenRead($path)
        try {
            $decoder = [Windows.Media.Imaging.PngBitmapDecoder]::new($image, 'None', 'OnLoad')
            $pixels = [Windows.Media.Imaging.FormatConvertedBitmap]::new($decoder.Frames[0],
                [Windows.Media.PixelFormats]::Bgr24, $null, 0)
            if ($pixels.PixelWidth -ne 16 -or $pixels.PixelHeight -ne 16) { throw 'Wrong PNG dimensions' }
            $actual = [byte[]]::new($bgr.Length)
            $pixels.CopyPixels($actual, 48, 0)
            for ($i = 0; $i -lt $actual.Length; $i++) {
                if ($actual[$i] -ne $bgr[$i]) { throw "Wrong RGB value at byte $i" }
            }
        } finally { $image.Dispose() }
    }
    if ([IO.Directory]::GetFiles($photos).Length -ne 2 -or [IO.Directory]::Exists((Join-Path $root 'Photos'))) {
        throw 'Unexpected duplicate, preview, partial file or output relative to cwd'
    }
} finally {
    if ($null -ne $child) {
        $child.StandardInput.Close()
        if (-not $child.WaitForExit(10000)) { $child.Kill(); $child.WaitForExit() }
        $child.Dispose()
    }
    [IO.Directory]::Delete($root, $true)
}
