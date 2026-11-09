$ErrorActionPreference='Stop'
$fixturePath=Join-Path $PSScriptRoot 'GTSport.exe'
if(Test-Path -LiteralPath $fixturePath) { throw 'Fixture already exists; refusing to overwrite an executable.' }
$compilerPath=Join-Path ([Environment]::GetFolderPath('Windows')) 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
if(!(Test-Path -LiteralPath $compilerPath)) { throw '64-bit .NET Framework C# compiler not found.' }
& $compilerPath /nologo /platform:x64 /target:exe ("/out:"+$fixturePath) (Join-Path $PSScriptRoot 'Fixture.cs')
if($LASTEXITCODE -ne 0) { throw 'Fixture build failed.' }
Write-Output 'Built disposable test fixture; this is not the converted game.'
