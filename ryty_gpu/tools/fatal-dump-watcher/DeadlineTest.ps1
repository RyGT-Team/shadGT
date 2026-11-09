$ErrorActionPreference='Stop'; $base=$PSScriptRoot; $p=Start-Process (Join-Path $base 'GTSport.exe') -WindowStyle Hidden -PassThru
try {
 Add-Type -Path (Join-Path $base 'SnapshotDump.cs')
 $path=Join-Path $base ('cancel-'+[guid]::NewGuid().ToString('N')+'.dmp')
 try { [SnapshotDump]::Capture($p.Id,$p.Path,$p.StartTime.ToUniversalTime().ToFileTimeUtc(),$path,0,1073741824); throw 'Deadline cancellation did not occur' } catch { if($_.Exception.Message -notmatch 'Dump budget exhausted') { throw }; "cooperative deadline cancellation: $($_.Exception.Message)" }
 if ((Test-Path ($path+'.attempts.txt')) -or (Test-Path ($path -replace '\.dmp$','-fallback.dmp'))) { throw 'Unexpected dump attempt after deadline' }
} finally { if(-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }

