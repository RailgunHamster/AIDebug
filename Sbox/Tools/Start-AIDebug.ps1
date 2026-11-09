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
$manifest=Join-Path $root '.verso-dependencies.json'
$target='Libraries/AIDebug'
if(Test-Path -LiteralPath $manifest){$definition=(Get-Content -LiteralPath $manifest -Raw|ConvertFrom-Json).dependencies|Where-Object id -eq 'aidebug';if($definition){$target=$definition.targetPath}}
$library=[IO.Path]::GetFullPath((Join-Path $root $target))
if(!$library.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Library path escapes project'}
$builder=Join-Path $library 'Editor/AIDebug/DebugRuntimeCompiler.cs'
if(!(Test-Path -LiteralPath $builder)){throw 'Finish the Verso update before attaching AI Debug'}
$escaped=$builder.Replace('"','""');$escapedLibrary=$library.Replace('"','""');$escapedRoot=$root.Replace('"','""');$escapedToken=$Token.Replace('"','""')
$code=@"
var paths=new System.Collections.Generic.Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
foreach(var file in ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")??"").Split(System.IO.Path.PathSeparator)){
 if(!file.EndsWith(".dll",StringComparison.OrdinalIgnoreCase)||!System.IO.File.Exists(file))continue;
 try{System.Reflection.AssemblyName.GetAssemblyName(file);paths.TryAdd(System.IO.Path.GetFileName(file),file);}catch(BadImageFormatException){}
}
var sdk=System.IO.Path.GetDirectoryName(typeof(Sandbox.Scene).Assembly.Location);
foreach(var name in new[]{"Sandbox.Engine","Sandbox.System","Sandbox.Reflection","Microsoft.CodeAnalysis","Microsoft.CodeAnalysis.CSharp"}){
 var file=System.IO.Path.Combine(sdk,name+".dll");if(System.IO.File.Exists(file))paths[System.IO.Path.GetFileName(file)]=file;
}
var syntax=Microsoft.CodeAnalysis.CSharp.CSharpSyntaxTree.ParseText(System.IO.File.ReadAllText(@"$escaped"));
var options=new Microsoft.CodeAnalysis.CSharp.CSharpCompilationOptions(Microsoft.CodeAnalysis.OutputKind.DynamicallyLinkedLibrary);
var compile=Microsoft.CodeAnalysis.CSharp.CSharpCompilation.Create("AIDebug.RuntimeBuilder."+Guid.NewGuid().ToString("N"),new[]{syntax},paths.Values.Select(p=>Microsoft.CodeAnalysis.MetadataReference.CreateFromFile(p)),options);
using var bytes=new System.IO.MemoryStream();var emitted=compile.Emit(bytes);
if(!emitted.Success)throw new InvalidOperationException(string.Join("\n",emitted.Diagnostics));
var helper=System.Reflection.Assembly.Load(bytes.ToArray()).GetType("AIDebug.Sbox.EditorIntegration.DebugRuntimeCompiler",true);
var runtime=(string)helper.GetMethod("Prepare",System.Reflection.BindingFlags.Static|System.Reflection.BindingFlags.NonPublic).Invoke(null,new object[]{@"$escapedLibrary",@"$escapedRoot"});
var assembly=AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a=>a.GetName().Name=="AIDebug.Sbox")??System.Runtime.Loader.AssemblyLoadContext.Default.LoadFromAssemblyPath(runtime);
if(!System.IO.Path.GetFullPath(assembly.Location).Equals(runtime,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Restart the target to load updated AI Debug sources");
return assembly.GetType("AIDebug.Sbox.RuntimeService",true).GetMethod("Start").Invoke(null,new object[]{@"$escapedRoot",$Port,@"$escapedToken",@"$escapedLibrary"});
"@
$argsJson=@{code=$code}|ConvertTo-Json -Compress
& $cli --connect $TargetPid --call run_csharp $argsJson
if($LASTEXITCODE -ne 0){throw 'Could not attach the runtime debugger'}
