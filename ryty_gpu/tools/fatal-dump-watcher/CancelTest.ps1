$ErrorActionPreference='Stop'; $base=$PSScriptRoot; $p=Start-Process (Join-Path $base 'GTSport.exe') -WindowStyle Hidden -PassThru
try {
 Add-Type -Path (Join-Path $base 'SnapshotDump.cs')
 $path=Join-Path $base ('cancel-'+[guid]::NewGuid().ToString('N')+'.dmp')
 try { [SnapshotDump]::Capture($p.Id,$p.Path,$p.StartTime.ToUniversalTime().ToFileTimeUtc(),$path,10,1); throw 'Size cancellation did not occur' } catch { if($_.Exception.Message -notmatch '0x800704c7') { throw }; "cooperative size cancellation: $($_.Exception.Message)" }
 if ((Get-Content ($path+'.attempts.txt') -Raw) -notmatch '^primary=0x1a00\s*$' -or (Test-Path ($path -replace '\.dmp$','-fallback.dmp'))) { throw 'Unexpected fallback after cancellation' }
} finally { if(-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
