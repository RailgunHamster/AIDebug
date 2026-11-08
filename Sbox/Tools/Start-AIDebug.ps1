#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Project,[Parameter(Mandatory)][string]$Engine,[int]$TargetPid=0,[int]$Port=7651,[string]$Token='')
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $Project).Path
$projectFiles=@(Get-ChildItem -LiteralPath $root -File -Force -Filter '*.sbproj');if($projectFiles.Count -ne 1){throw 'Expected exactly one .sbproj file'};$projectFile=$projectFiles[0].FullName
$game=(Resolve-Path -LiteralPath $Engine).Path
if(!(Test-Path -LiteralPath "$game/sbox-dev.exe")){$game=Join-Path $game 'game'}
$cli=Join-Path $game 'bin/managed/sbox-cli.exe'
if(!$TargetPid){
    $instances=(& $cli --list|Out-String)|ConvertFrom-Json
    $matches=@($instances|Where-Object {$_.project -and [IO.Path]::GetFullPath($_.project).Equals($projectFile,[StringComparison]::OrdinalIgnoreCase)})
    if($matches.Count -ne 1){throw 'Pass -TargetPid to select exactly one running editor or game with local MCP enabled'}
    $TargetPid=$matches[0].pid
}
$deadline=[DateTime]::UtcNow.AddSeconds(180)
do{
    $state=(& $cli --connect $TargetPid --call instance_status|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -ne 0){throw 'The target engine is not responding; finish game startup before attaching AI Debug.'}
    if($state.Kind -ne 'game' -or (!$state.IsLoading -and $state.ActiveScene)){break}
    Start-Sleep -Milliseconds 500
}while([DateTime]::UtcNow -lt $deadline)
if($state.Kind -eq 'game' -and ($state.IsLoading -or !$state.ActiveScene)){throw 'The game did not become ready before the attachment deadline'}
$directory=Join-Path $root 'Editor/AIDebug/Managed'
$escaped=$directory.Replace('"','""');$escapedRoot=$root.Replace('"','""');$escapedToken=$Token.Replace('"','""')
$code=@"
var dir=@"$escaped";
var assembly=AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a=>a.GetName().Name=="AIDebug.Sbox")??System.Runtime.Loader.AssemblyLoadContext.Default.LoadFromAssemblyPath(System.IO.Path.Combine(dir,"AIDebug.Sbox.dll"));
return assembly.GetType("AIDebug.Sbox.RuntimeService",true).GetMethod("Start").Invoke(null,new object[]{@"$escapedRoot",$Port,@"$escapedToken"});
"@
$argsJson=@{code=$code}|ConvertTo-Json -Compress
& $cli --connect $TargetPid --call run_csharp $argsJson
if($LASTEXITCODE -ne 0){throw 'Could not attach the runtime debugger'}
