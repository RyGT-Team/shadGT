$ErrorActionPreference='Stop'
$dir=Join-Path (Join-Path $PSScriptRoot 'audit-20261102-fixture') ('fallback-fixture-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'SnapshotDump.cs'),(Join-Path $PSScriptRoot 'Watch-Fatal.ps1'),(Join-Path $PSScriptRoot 'SelfTest.ps1'),(Join-Path $PSScriptRoot 'GTSport.exe') -Destination $dir
$source=Join-Path $dir 'SnapshotDump.cs'
$code=[IO.File]::ReadAllText($source)
$code=$code.Replace('ok=MiniDumpWriteDump(', 'ok=attempt==0 ? false : MiniDumpWriteDump(').Replace('error=ok ? 0 : Marshal.GetLastWin32Error();','error=ok ? 0 : attempt==0 ? unchecked((int)0x80070057) : Marshal.GetLastWin32Error();')
[IO.File]::WriteAllText($source,$code)
$test=Join-Path $dir 'SelfTest.ps1'
$code=[IO.File]::ReadAllText($test).Replace("Join-Path `$out 'fatal.dmp'", "Join-Path `$out 'fatal-fallback.dmp'")
$code=$code.Replace("if(-not `$text.Contains('RYGT_FATAL_WATCHER_PRIVATE_HEAP_MARKER_8A3E06D51A6A7B2F')) { throw 'Private heap marker missing from dump' }", "if (`$d.Length -lt 32) { throw 'Fallback dump too small' }")
$code=$code.Replace('guarded fault ignored; marker found','guarded fault ignored; fallback dump validated')
[IO.File]::WriteAllText($test,$code)
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $test
if($LASTEXITCODE -ne 0) { throw 'Fallback fixture failed' }
$result=Get-ChildItem -LiteralPath $dir -Directory -Filter 'fixture-result-*' | Select-Object -First 1
if(!(Test-Path (Join-Path $result.FullName 'fatal.dmp.primary-error.txt')) -or !(Test-Path (Join-Path $result.FullName 'fatal-fallback.dmp'))) { throw 'Missing primary error or fallback dump' }
if((Get-Content (Join-Path $result.FullName 'fatal.dmp.primary-error.txt') -Raw) -notmatch '0x80070057') { throw 'Primary HRESULT missing' }
if((Get-Content (Join-Path $result.FullName 'fatal.dmp.attempts.txt') -Raw) -notmatch '^primary=0x1a00\s+fallback=0x0\s*$') { throw 'Unexpected attempt sequence' }
if((Get-Content (Join-Path $result.FullName 'fatal.dmp.dbghelp.txt') -Raw) -notmatch '(?m)^path=.+dbghelp\.dll') { throw 'Loaded DbgHelp path missing' }
if((Get-Content (Join-Path $result.FullName 'status.txt') -Raw) -notmatch 'dump=fatal-fallback.dmp') { throw 'Fallback was not selected' }
if((Get-Content (Join-Path $result.FullName 'status.txt') -Raw) -notmatch 'usable_threads=[1-9][0-9]*') { throw 'No usable thread' }
