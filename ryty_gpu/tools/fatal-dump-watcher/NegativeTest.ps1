$ErrorActionPreference='Stop'
$base=$PSScriptRoot; $fixture=Join-Path $base 'GTSport.exe'; $log=Join-Path $base 'logs\stderr.log'; $p=Start-Process $fixture -WindowStyle Hidden -PassThru
try {
 $start=$p.StartTime.ToUniversalTime().ToFileTimeUtc(); $watchScript=Join-Path $base 'Watch-Fatal.ps1'
 $bad=Join-Path $base ('negative-start-'+[guid]::NewGuid().ToString('N'))
 try { & $watchScript -TargetPid $p.Id -ExePath $fixture -StartFileTime ($start+1) -Stderr $log -OutputDir $bad -WatchSeconds 10 -MaxMiB 256; throw 'Wrong start accepted' } catch { if($_ -notmatch 'identity mismatch') { throw } }
 $badPid=Join-Path $base ('negative-pid-'+[guid]::NewGuid().ToString('N'))
 try { & $watchScript -TargetPid 2147483647 -ExePath $fixture -StartFileTime $start -Stderr $log -OutputDir $badPid -WatchSeconds 10 -MaxMiB 256; throw 'Wrong PID accepted' } catch { if($_ -match 'Wrong PID accepted') { throw } }
 [IO.File]::WriteAllText($log,"FATAL: unhandled exception 0xc0000005 on thread 123`n  pausing 60 s for inspection (pid $($p.Id))`n")
 $out=Join-Path $base ('negative-old-'+[guid]::NewGuid().ToString('N'))
 try { & $watchScript -TargetPid $p.Id -ExePath $fixture -StartFileTime $start -Stderr $log -OutputDir $out -WatchSeconds 10 -MaxMiB 256; throw 'Old fatal accepted' } catch { if($_ -notmatch 'Watch timeout') { throw } }
 if(Test-Path (Join-Path $out 'fatal.dmp')) { throw 'Old fatal generated dump' }
 'wrong start and PID rejected; old fatal ignored; watch timeout passed'
} finally { if(-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
