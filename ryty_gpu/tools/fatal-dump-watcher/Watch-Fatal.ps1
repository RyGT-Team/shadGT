param([Parameter(Mandatory=$true)][int]$TargetPid,[Parameter(Mandatory=$true)][string]$ExePath,[Parameter(Mandatory=$true)][long]$StartFileTime,[Parameter(Mandatory=$true)][string]$Stderr,[Parameter(Mandatory=$true)][string]$OutputDir,[ValidateRange(10,86400)][int]$WatchSeconds=3600,[ValidateRange(10,600)][int]$DumpSeconds=120,[ValidateRange(256,32768)][int]$MaxMiB=8192,[switch]$Worker)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=[IO.Path]::GetFullPath($OutputDir)
if (-not $out.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be inside investigation-build' }
$exe=[IO.Path]::GetFullPath($ExePath)
if ([IO.Path]::GetFileName($exe) -ine 'GTSport.exe') { throw 'Only exact GTSport.exe allowed' }
if ($Worker) { Add-Type -Path (Join-Path $PSScriptRoot 'SnapshotDump.cs'); [SnapshotDump]::Capture($TargetPid,$exe,$StartFileTime,(Join-Path $out 'fatal.dmp'),$DumpSeconds,([long]$MaxMiB*1MB)); exit }
$proc=Get-Process -Id $TargetPid
if ($proc.Path -ine $exe -or $proc.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $StartFileTime) { throw 'Process identity mismatch' }
$log=[IO.Path]::GetFullPath($Stderr)
if (-not (Test-Path -LiteralPath $log -PathType Leaf)) { throw 'Existing session stderr required' }
$expectedLogDir=[IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetDirectoryName($exe)) 'logs'))
if ([IO.Path]::GetDirectoryName($log) -ine $expectedLogDir -or [IO.Path]::GetFileName($log) -ine 'stderr.log') { throw 'Expected package logs/stderr.log' }
if (Test-Path -LiteralPath $out) { throw 'Output directory must be new' }
$drive=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($out))
if ($drive.AvailableFreeSpace -lt ([long]$MaxMiB*1MB + 1GB)) { throw 'Insufficient free space for configured dump limit and reserve' }
New-Item -ItemType Directory -Path $out | Out-Null
$stream=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
try {
 $offset=$stream.Length; $stream.Position=$offset; $pending=''; $deadline=[DateTime]::UtcNow.AddSeconds($WatchSeconds)
 "ready pid=$TargetPid start=$StartFileTime initial_offset=$offset" | Set-Content (Join-Path $out 'ready.txt')
 while ([DateTime]::UtcNow -lt $deadline) {
  $proc.Refresh(); if ($proc.HasExited -or $proc.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $StartFileTime) { throw 'Target exited or changed' }
  if ($stream.Length -lt $offset) { throw 'Log truncated or rotated' }
  $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true,4096,$true); $pending += $reader.ReadToEnd(); $offset=$stream.Position
  if ($pending -match '(?m)^FATAL: unhandled exception 0x[0-9a-fA-F]{8} on thread [0-9]+') {
   # Capture only after the complete report reaches its final stack section and flush.
   if ($pending -match '(?m)^  pausing [0-9]+ s for inspection \(pid ([0-9]+)\)') {
    if ([int]$Matches[1] -ne $TargetPid) { throw 'Fatal pause PID mismatch' }
    [IO.File]::WriteAllText((Join-Path $out 'fatal-context.txt'),$pending)
    $args=@('-NoProfile','-File',('"'+$PSCommandPath+'"'),'-Worker','-TargetPid',$TargetPid,'-ExePath',('"'+$exe+'"'),'-StartFileTime',$StartFileTime,'-Stderr',('"'+$log+'"'),'-OutputDir',('"'+$out+'"'),'-DumpSeconds',$DumpSeconds,'-MaxMiB',$MaxMiB)
    $dumpProcess=Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $args -WindowStyle Hidden -RedirectStandardError (Join-Path $out "worker-error.txt") -RedirectStandardOutput (Join-Path $out "worker-output.txt") -PassThru
    $null=$dumpProcess.Handle
    $limit=[DateTime]::UtcNow.AddSeconds($DumpSeconds+5); $dump=Join-Path $out 'fatal.dmp'
    while (-not $dumpProcess.HasExited) { if ([DateTime]::UtcNow -gt $limit) { Stop-Process -Id $dumpProcess.Id -Force; throw 'Dump worker exceeded time or size limit; partial dump invalid' }; Start-Sleep -Milliseconds 250; $dumpProcess.Refresh() }
    $dumpProcess.WaitForExit(); if ($dumpProcess.ExitCode -ne 0) { throw "Dump worker failed: $($dumpProcess.ExitCode)" }
    $selected=(Get-Content -LiteralPath (Join-Path $out 'worker-output.txt') -Raw).Trim()
    $fallback=Join-Path $out 'fatal-fallback.dmp'
    if ($selected -ine $dump -and $selected -ine $fallback) { throw 'Worker returned unexpected dump path' }
    $total=(Get-Item -LiteralPath $dump).Length; if ($selected -ieq $fallback) { $total+=(Get-Item -LiteralPath $fallback).Length }
    if ($total -gt [long]$MaxMiB*1MB) { throw "Completed dumps exceed size limit; dump invalid" }
    $drive=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($out)); if ($drive.AvailableFreeSpace -lt 1GB) { throw "Completed dump breached free-space reserve; dump invalid" }
    $f=[IO.File]::OpenRead($selected); $reader=[IO.BinaryReader]::new($f)
    try {
     if ($f.Length -lt 32 -or $reader.ReadUInt32() -ne 0x504d444d) { throw 'Invalid dump header' }
     $null=$reader.ReadUInt32(); $count=$reader.ReadUInt32(); $directory=$reader.ReadUInt32()
     if ($count -gt 4096 -or [long]$directory+12*[long]$count -gt $f.Length) { throw 'Invalid stream directory' }
     $memoryRanges=@(); $f.Position=$directory
     for($i=0;$i -lt $count;$i++) {
      $type=$reader.ReadUInt32(); $size=$reader.ReadUInt32(); $rva=$reader.ReadUInt32(); $next=$f.Position
      if([long]$rva+[long]$size -gt $f.Length) { throw 'Invalid stream extent' }
      if($type -eq 5 -and $size -ge 4) {
       $f.Position=$rva; $ranges=$reader.ReadUInt32()
       if(4+[long]$ranges*16 -gt $size) { throw 'Invalid memory list' }
       for($j=0;$j -lt $ranges;$j++) {
        $base=$reader.ReadUInt64(); $length=$reader.ReadUInt32(); $location=$reader.ReadUInt32()
        if([long]$location+[long]$length -gt $f.Length) { throw 'Invalid memory range extent' }
        $memoryRanges+=@{base=$base;length=$length;location=$location}
       }
      }
      if($type -eq 9 -and $size -ge 16) {
       $f.Position=$rva; $ranges=$reader.ReadUInt64(); $location=$reader.ReadUInt64()
       if($ranges -gt [ulong](($size-16)/16) -or $location -gt [ulong]$f.Length) { throw 'Invalid memory64 list' }
       for($j=0;$j -lt $ranges;$j++) {
        $base=$reader.ReadUInt64(); $length=$reader.ReadUInt64()
        if($length -gt [ulong]$f.Length-$location) { throw 'Invalid memory64 range extent' }
        $memoryRanges+=@{base=$base;length=$length;location=$location}; $location+=$length
       }
      }
      $f.Position=$next
     }
     $f.Position=$directory; $thread=$false; $module=$false; $threadCount=0; $moduleCount=0; $usableThreads=0
     for ($i=0;$i -lt $count;$i++) {
      $type=$reader.ReadUInt32(); $size=$reader.ReadUInt32(); $rva=$reader.ReadUInt32()
      if ([long]$rva+[long]$size -gt $f.Length) { throw 'Invalid stream extent' }
      if (($type -eq 3 -or $type -eq 4) -and $size -ge 4) {
       $next=$f.Position; $f.Position=$rva; $entries=$reader.ReadUInt32(); $f.Position=$next
       $entrySize=if($type -eq 3){48}else{108}
       if ($entries -gt 0 -and 4+[long]$entries*$entrySize -le $size) {
        if($type -eq 3) {
         $thread=$true; $threadCount=$entries
         for($j=0;$j -lt $entries;$j++) {
          $f.Position=[long]$rva+4+48*[long]$j+24
          $stackBase=$reader.ReadUInt64(); $stackSize=$reader.ReadUInt32(); $stackRva=$reader.ReadUInt32(); $contextSize=$reader.ReadUInt32(); $contextRva=$reader.ReadUInt32()
          $stackValid=$stackSize -gt 0 -and $stackRva -gt 0 -and [long]$stackRva+[long]$stackSize -le $f.Length
          if(!$stackValid -and $stackSize -gt 0) {
           foreach($range in $memoryRanges) { if($range.location -gt 0 -and $stackBase -ge $range.base -and ($stackBase-$range.base) -le $range.length -and $stackSize -le ($range.length-($stackBase-$range.base))) { $stackValid=$true; break } }
          }
          if($stackValid -and $contextSize -gt 0 -and $contextRva -gt 0 -and [long]$contextRva+[long]$contextSize -le $f.Length) { $usableThreads++ }
         }
         $f.Position=$next
        } else { $module=$true; $moduleCount=$entries }
       }
      }
     }
     if (!$thread -or !$module -or $usableThreads -eq 0) { throw 'Missing usable thread or module stream' }
    } finally { $reader.Dispose(); $f.Dispose() }
    $coverage=if($selected -ieq $fallback){'fallback=normal; threads/modules/stacks requested; private heap not included'}else{'primary=private-RW memory, memory-info and thread-info requested'}
    "success pid=$TargetPid start=$StartFileTime stderr_offset=$offset dump=$([IO.Path]::GetFileName($selected)) bytes=$((Get-Item $selected).Length) total_bytes=$total threads=$threadCount usable_threads=$usableThreads modules=$moduleCount coverage=$coverage" | Set-Content (Join-Path $out 'status.txt'); exit
   }
  }
  if ($pending.Length -gt 2MB) { $pending=$pending.Substring($pending.Length-1MB) }
  Start-Sleep -Milliseconds 250
 }
 throw 'Watch timeout'
} catch { $_ | Out-String | Set-Content (Join-Path $out 'failure.txt'); throw } finally { $stream.Dispose() }









