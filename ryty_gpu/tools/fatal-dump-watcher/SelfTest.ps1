$ErrorActionPreference='Stop'
$base=$PSScriptRoot
$fixture=Join-Path $base 'GTSport.exe'
$logDir=Join-Path $base 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
$log=Join-Path $logDir 'stderr.log'
$out=Join-Path $base ('fixture-result-'+[guid]::NewGuid().ToString('N'))
[IO.File]::WriteAllText($log,'')
function Write-FixtureLog([string]$text) {
 $file=[IO.FileStream]::new($log,[IO.FileMode]::Append,[IO.FileAccess]::Write,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
 try { $writer=[IO.StreamWriter]::new($file,[Text.UTF8Encoding]::new($false)); try { $writer.WriteLine($text); $writer.Flush() } finally { $writer.Dispose() } } finally { $file.Dispose() }
}
$watch=$null
$p=Start-Process -FilePath $fixture -WindowStyle Hidden -PassThru
try {
 $start=$p.StartTime.ToUniversalTime().ToFileTimeUtc()
 $watch=Start-Process -FilePath (Get-Process -Id $PID).Path -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-File',('"'+(Join-Path $base 'Watch-Fatal.ps1')+'"'),'-TargetPid',$p.Id,'-ExePath',('"'+$fixture+'"'),'-StartFileTime',$start,'-Stderr',('"'+$log+'"'),'-OutputDir',('"'+$out+'"'),'-WatchSeconds','30','-DumpSeconds','30','-MaxMiB','256')
 $readyDeadline=[DateTime]::UtcNow.AddSeconds(20)
 while (-not (Test-Path (Join-Path $out 'ready.txt'))) { if ($watch.HasExited -or [DateTime]::UtcNow -gt $readyDeadline) { throw 'Watcher not ready' }; Start-Sleep -Milliseconds 100; $watch.Refresh() }
 Write-FixtureLog "[fault] passed to the renderer's guarded read: 0x123"
 Start-Sleep -Seconds 1
 if(Test-Path (Join-Path $out 'fatal.dmp')) { throw 'Guarded read triggered a dump' }
 Write-FixtureLog "FATAL: unhandled exception 0xc0000005 on thread 123 'fixture'`n  rip 0x0000`n  pausing 60 s for inspection (pid $($p.Id))"
 if(-not $watch.WaitForExit(40000)) { Stop-Process -Id $watch.Id -Force; throw 'Watcher timed out' }
 if($watch.ExitCode -ne 0) { Get-Content (Join-Path $out 'failure.txt'); throw "Watcher failed $($watch.ExitCode)" }
 $d=[IO.File]::ReadAllBytes((Join-Path $out 'fatal.dmp')); $text=[Text.Encoding]::ASCII.GetString($d)
 if(-not $text.Contains('RYGT_FATAL_WATCHER_PRIVATE_HEAP_MARKER_8A3E06D51A6A7B2F')) { throw 'Private heap marker missing from dump' }
 Get-Content (Join-Path $out 'status.txt'); Write-Output 'guarded fault ignored; marker found'
} finally { if ($null -ne $watch) { $watch.Refresh(); if (-not $watch.HasExited) { Stop-Process -Id $watch.Id -Force; $watch.WaitForExit() } }; $p.Refresh(); if(-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit() } }



