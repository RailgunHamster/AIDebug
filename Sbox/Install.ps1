#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Project,
    [ValidateSet('Install','Check','Verify')][string]$Action='Install',
    [switch]$DryRun,
    [string]$Engine,
    [switch]$Verify,
    [switch]$NoBuild,
    [ValidateRange(10,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path -LiteralPath $Project).Path.TrimEnd('\','/')
$projectFiles=@(Get-ChildItem -LiteralPath $projectRoot -File -Force -Filter '*.sbproj')
if($projectFiles.Count -ne 1){throw 'Project directory must contain exactly one .sbproj file'}
$projectFile=$projectFiles[0].FullName
$repoRoot=(Resolve-Path "$PSScriptRoot/..").Path
$repository='https://github.com/RailgunHamster/AIDebug.git'
if($Action -eq 'Install' -and !$DryRun -and !$NoBuild){
    if(!$Engine){throw 'AIDebug installation builds its runtime adapter; pass -Engine.'}
    & "$PSScriptRoot/Tools/Build.ps1" -Engine $Engine
}
$source=(Resolve-Path -LiteralPath "$PSScriptRoot/Editor/AIDebug").Path
if($Action -eq 'Install' -and !$DryRun -and (!(Test-Path -LiteralPath "$source/Managed/AIDebug.Sbox.dll") -or !(Test-Path -LiteralPath "$source/Native/aidebug_native.dll"))){throw 'Build the complete runtime package before using -NoBuild'}
$destination=Join-Path $projectRoot 'Editor/AIDebug'
$data=Join-Path $projectRoot '.ai_debug'
$manifestPath=Join-Path $data 'install.json'


function Assert-PlainTarget([string]$Path){
    $absolute=[IO.Path]::GetFullPath($Path)
    if(!$absolute.StartsWith($projectRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Target escapes project: $absolute"}
    $cursor=$absolute
    while($cursor -ne $projectRoot){
        if((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Linked target requires manual management: $cursor"}
        $cursor=Split-Path $cursor
    }
}
function Verify-Editor{
    if(!$Engine){throw 'Pass -Engine with the engine directory containing sbox-dev.exe (or its parent).'}
    $bin=(Resolve-Path -LiteralPath $Engine).Path
    if(!(Test-Path -LiteralPath "$bin/sbox-dev.exe")){$bin=Join-Path $bin 'game'}
    $cli=Join-Path $bin 'bin/managed/sbox-cli.exe'
    if(!(Test-Path -LiteralPath $cli)){throw "sbox-cli is missing: $cli"}
    $instances=(& $cli --list|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -ne 0){throw 'Could not list editor instances'}
    $existing=@($instances|Where-Object {$_.kind -eq 'editor' -and [IO.Path]::GetFullPath($_.project).Equals($projectFile,[StringComparison]::OrdinalIgnoreCase)})
    if($existing.Count -gt 1){throw 'Multiple editors have this project open'}
    $owned=$null
    try{
        if($existing.Count){$editorPid=$existing[0].pid}else{
            $owned=Start-Process -FilePath (Join-Path $bin 'sbox-dev.exe') -WorkingDirectory $bin -ArgumentList @('-project',('"'+$projectFile+'"'),'-nosteam','-disable_stall_monitor') -WindowStyle Hidden -PassThru
            $editorPid=$owned.Id
        }
        $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        do{
            if($owned -and $owned.HasExited){throw "Verification editor exited: $($owned.ExitCode)"}
            $instances=(& $cli --list|Out-String)|ConvertFrom-Json
            if(@($instances|Where-Object pid -eq $editorPid).Count){break}
            Start-Sleep -Milliseconds 500
        }while([DateTime]::UtcNow -lt $deadline)
        if(!@($instances|Where-Object pid -eq $editorPid).Count){throw 'Editor startup verification timed out'}
        do{
            $status=(& $cli --connect $editorPid --call editor_status|Out-String)|ConvertFrom-Json
            if($LASTEXITCODE -ne 0){throw 'Could not read editor status'}
            if(!$status.IsCompiling){break}
            Start-Sleep -Milliseconds 300
        }while([DateTime]::UtcNow -lt $deadline)
        if($status.IsCompiling -or !$status.LastCompileSucceeded -or $status.LastCompileErrors){throw 'Editor compilation verification failed'}
        $plugin=(& $cli --connect $editorPid --call aidebug_start|Out-String)|ConvertFrom-Json
        if($LASTEXITCODE -ne 0 -or !$plugin.active){throw 'AI Debug did not initialize'}
        $definitions=(& $cli --connect $editorPid --call aidebug_tools|Out-String)|ConvertFrom-Json
        if($LASTEXITCODE -ne 0 -or @($definitions).Count -ne 19){throw 'AI Debug tool schemas are unavailable'}
        if(!$status.Paths.ProjectRoot.Equals($projectRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Verification attached to a different project'}
        Write-Output "Verified editor compilation, debug activation and 19 MCP schemas in PID $editorPid."
    }finally{
        if($owned -and !$owned.HasExited){
            & $cli --connect $owned.Id --call run_csharp '{"code":"Editor.EditorUtility.Quit();return true;"}' 2>$null|Out-Null
            $owned.WaitForExit(10000)|Out-Null
            if(!$owned.HasExited){Stop-Process -Id $owned.Id}
        }
    }
}
if($Action -eq 'Verify'){Verify-Editor;return}
if($Verify -and !$Engine){throw '-Verify requires -Engine'}
Assert-PlainTarget $destination
Assert-PlainTarget $manifestPath
$revision=(& git --no-optional-locks -C $repoRoot rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0){throw 'Run from a checkout of the existing AIDebug repository'}
$dirty=!!(& git --no-optional-locks -C $repoRoot status --porcelain -- Sbox)
if($LASTEXITCODE -ne 0){throw 'Cannot read source checkout status'}
$previous=if(Test-Path -LiteralPath $manifestPath){Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json -AsHashtable}else{$null}
if($previous -and ($previous.repository -ne $repository -or $previous.package -ne 'Sbox' -or $previous.files -isnot [Collections.IDictionary])){throw 'Unrecognized AI Debug install manifest'}
$files=[ordered]@{}
foreach($file in Get-ChildItem -LiteralPath $source -Recurse -File|Sort-Object FullName){
    if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Linked package file: $($file.FullName)"}
    $relative=[IO.Path]::GetRelativePath($source,$file.FullName).Replace('\','/')
    $files[$relative]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
}
if(!$files.Contains('DebugEditor.cs') -or !$files.Contains('Native/aidebug_native.dll')){throw 'Incomplete Sbox package'}

if(Test-Path -LiteralPath $destination){foreach($file in Get-ChildItem -LiteralPath $destination -Recurse -File){
    Assert-PlainTarget $file.FullName
    $relative=[IO.Path]::GetRelativePath($destination,$file.FullName).Replace('\','/')
    $hash=(Get-FileHash -LiteralPath $file.FullName).Hash
    if($files.Contains($relative) -and $hash -eq $files[$relative]){continue}
    if(!$previous -or !$previous.files.Contains($relative) -or $previous.files[$relative] -ne $hash){throw "Local plugin edits require manual resolution: $relative"}
}}
$ignore=if(Test-Path -LiteralPath (Join-Path $projectRoot '.loreignore')){Join-Path $projectRoot '.loreignore'}else{Join-Path $projectRoot '.gitignore'}
Assert-PlainTarget $ignore
$ignoreBefore=if(Test-Path -LiteralPath $ignore){[IO.File]::ReadAllBytes($ignore)}else{$null}
$manifestBefore=if(Test-Path -LiteralPath $manifestPath){[IO.File]::ReadAllBytes($manifestPath)}else{$null}
Write-Output "$Action plan: $($files.Count) debugger files; source $revision (dirty: $dirty)."
if($Action -eq 'Check' -or $DryRun){return}
$session=Join-Path $data 'session.json'
if(Test-Path -LiteralPath $session){
    $sessionInfo=Get-Content -LiteralPath $session -Raw|ConvertFrom-Json
    $running=Get-Process -Id $sessionInfo.pid -ErrorAction SilentlyContinue
    if($running -and $running.ProcessName -eq 'sbox-dev' -and $running.StartTime -le (Get-Item -LiteralPath $session).LastWriteTime){throw 'Close this project editor before installing; use -Action Verify to inspect an existing editor.'}
}
$backup=Join-Path $projectRoot ('Art/build/aidebug-installs/'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))
Assert-PlainTarget $backup
New-Item -ItemType Directory -Force -Path $backup|Out-Null
$staged=Join-Path $backup 'new';$old=Join-Path $backup 'AIDebug'
Copy-Item -LiteralPath $source -Destination $staged -Recurse
$swapped=$false
try{
    if(Test-Path -LiteralPath $destination){Move-Item -LiteralPath $destination -Destination $old}
    New-Item -ItemType Directory -Force -Path (Split-Path $destination)|Out-Null
    Move-Item -LiteralPath $staged -Destination $destination;$swapped=$true
    $text=if($ignoreBefore){[Text.Encoding]::UTF8.GetString($ignoreBefore)}else{''}
    foreach($rule in @('/Editor/AIDebug/','/.ai_debug/')){if(($text -split '\r?\n') -notcontains $rule){$text+="`n$rule`n"}}
    [IO.File]::WriteAllText($ignore,$text)
    New-Item -ItemType Directory -Force -Path $data|Out-Null
    $record=@{schema=1;repository=$repository;revision=$revision;dirty=$dirty;package='Sbox';files=$files}
    [IO.File]::WriteAllText($manifestPath,($record|ConvertTo-Json -Depth 5))
    if($Verify){Verify-Editor}
}catch{
    if($swapped){Move-Item -LiteralPath $destination -Destination (Join-Path $backup 'failed-install')}
    if(Test-Path -LiteralPath $old){Move-Item -LiteralPath $old -Destination $destination}
    foreach($pair in @(@{path=$ignore;bytes=$ignoreBefore},@{path=$manifestPath;bytes=$manifestBefore})){
        if($null -eq $pair.bytes){if(Test-Path -LiteralPath $pair.path){Remove-Item -LiteralPath $pair.path}}
        else{[IO.File]::WriteAllBytes($pair.path,$pair.bytes)}
    }
    throw
}
Write-Output "Installed AIDebug/Sbox at $destination ($revision; local changes: $dirty)."
Write-Output "Previous deployment retained at $backup. Restart the editor to load it."
