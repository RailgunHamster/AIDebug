#Requires -Version 7.0
$ErrorActionPreference='Stop'
$package=(Resolve-Path "$PSScriptRoot/..").Path
$project=Join-Path $package ('Art/build/install-tests/with spaces/'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $project|Out-Null
[IO.File]::WriteAllText("$project/test.sbproj",'{}')
[IO.File]::WriteAllText("$project/.loreignore","/Art/build/`n")
function Assert($condition,$message){if(!$condition){throw $message}}
function Install{& "$package/Install.ps1" -Project $project -NoBuild}
& "$package/Install.ps1" -Project $project -Action Check
& "$package/Install.ps1" -Project $project -DryRun
Assert (!(Test-Path "$project/Editor") -and !(Test-Path "$project/.ai_debug")) 'Read-only preflight wrote files'
Write-Output 'PASS check and dry run'
Install
$record=Get-Content -LiteralPath "$project/.ai_debug/install.json" -Raw|ConvertFrom-Json
Assert ($record.repository -eq 'https://github.com/RailgunHamster/AIDebug.git' -and $record.package -eq 'Sbox') 'Provenance'
Assert (Test-Path "$project/Editor/AIDebug/Managed/AIDebug.Sbox.dll") 'Runtime missing; build the package before testing'
Write-Output 'PASS clean install'
[IO.File]::WriteAllText("$project/.ai_debug/config.json",'{"port":7659}')
$before=(Get-FileHash "$project/Editor/AIDebug/Managed/AIDebug.Sbox.dll").Hash
Install
Assert ((Get-FileHash "$project/Editor/AIDebug/Managed/AIDebug.Sbox.dll").Hash -eq $before) 'Repeat install changed bytes'
Assert ((Get-Content "$project/.ai_debug/config.json" -Raw|ConvertFrom-Json).port -eq 7659) 'Local config changed'
Write-Output 'PASS repeat install and local configuration'
$file="$project/Editor/AIDebug/DebugEditor.cs";$original=[IO.File]::ReadAllBytes($file)
[IO.File]::AppendAllText($file,"`n// local edit`n")
$rejected=$false;try{Install}catch{if($_.Exception.Message -notlike 'Local plugin edits*'){throw};$rejected=$true}
Assert $rejected 'Local edits overwritten';[IO.File]::WriteAllBytes($file,$original)
Write-Output 'PASS local edit protection'
$before=[IO.File]::ReadAllBytes("$project/.ai_debug/install.json")
$rejected=$false;try{& "$package/Install.ps1" -Project $project -NoBuild -Verify -Engine $project}catch{if($_.Exception.Message -notlike 'sbox-cli is missing:*'){throw};$rejected=$true}
Assert $rejected 'Broken verify succeeded'
Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes("$project/.ai_debug/install.json")) -eq [Convert]::ToBase64String($before)) 'Manifest was not restored'
Assert ((Get-FileHash -LiteralPath $file).Hash -eq (Get-FileHash "$package/Editor/AIDebug/DebugEditor.cs").Hash) 'Source was not restored'
Write-Output 'PASS verification failure rollback'
[IO.File]::WriteAllText("$project/Editor/AIDebug/unknown.cs",'unowned')
$rejected=$false;try{Install}catch{if($_.Exception.Message -notlike 'Local plugin edits*'){throw};$rejected=$true}
Assert $rejected 'Unmanaged source was overwritten'
Write-Output 'PASS unmanaged file protection'
Write-Output "6/6 installer checks passed; $project"
