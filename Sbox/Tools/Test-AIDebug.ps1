#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Project,[string]$BaseUrl='http://127.0.0.1:7651/mcp',[string]$Token='',[string]$ResultsPath,[switch]$SkipGuide)
$ErrorActionPreference='Stop'
$package=(Resolve-Path "$PSScriptRoot/..").Path
$projectRoot=(Resolve-Path -LiteralPath $Project).Path
if(!$ResultsPath){$ResultsPath=Join-Path $package 'Art/build/runtime-results.json'}
New-Item -ItemType Directory -Force -Path (Split-Path $ResultsPath)|Out-Null
$results=[Collections.Generic.List[object]]::new();$requestId=0
$http=[Net.Http.HttpClient]::new();$http.Timeout=[TimeSpan]::FromSeconds(40)
if($Token){$http.DefaultRequestHeaders.Authorization=[Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer',$Token)}
function Start-Rpc($method,$parameters=@{}){
    $id=++$script:requestId
    $body=@{jsonrpc='2.0';id=$id;method=$method;params=$parameters}|ConvertTo-Json -Depth 40 -Compress
    $content=[Net.Http.StringContent]::new($body,[Text.Encoding]::UTF8,'application/json')
    return @{id=$id;task=$http.PostAsync($BaseUrl,$content)}
}
function Finish-Rpc($pending){
    $response=$pending.task.GetAwaiter().GetResult();$body=$response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
    Assert $response.IsSuccessStatusCode "HTTP $($response.StatusCode): $body"
    $r=$body|ConvertFrom-Json -Depth 60;Assert ($r.id -eq $pending.id) 'Response ID association'
    if($r.error){throw ($r.error|ConvertTo-Json -Compress)};return $r.result
}
function Rpc($method,$parameters=@{}){Finish-Rpc (Start-Rpc $method $parameters)}
function RawTool($name,$arguments=@{}){
    $r=Rpc 'tools/call' @{name=$name;arguments=$arguments}
    return (($r.content|Where-Object type -eq text|Select-Object -First 1).text|ConvertFrom-Json -Depth 60)
}
function Tool($name,$arguments=@{}){$r=RawTool $name $arguments;if($r.PSObject.Properties.Name -contains 'ok' -and !$r.ok){throw ($r|ConvertTo-Json -Depth 10 -Compress)};return $r}
function Eval($code,$extra=@{}){$extra.code=$code;(Tool eval_cs $extra).result}
function Assert($condition,$message){if(!$condition){throw $message}}
function Test($name,[scriptblock]$body){$watch=[Diagnostics.Stopwatch]::StartNew();try{&$body|Out-Null;$results.Add(@{name=$name;passed=$true;ms=$watch.ElapsedMilliseconds});Write-Output "PASS $name"}catch{$results.Add(@{name=$name;passed=$false;ms=$watch.ElapsedMilliseconds;error=$_.Exception.Message});Write-Output "FAIL $name : $($_.Exception.Message)"}}
$fixture=Join-Path $package 'Art/build/fixture/SboxDebugFixture.dll'
$native=Join-Path $package 'Art/build/native/native_fixture.dll'
$target='SboxDebugFixture.Probe:Add(System.Int32)'
try{
    Test 'MCP handshake and 19 generic schemas' {$info=Rpc initialize @{protocolVersion='2025-11-25'};Assert ($info.serverInfo.name -eq 'sbox-ai-debug') 'server name';$tools=(Rpc 'tools/list').tools;Assert ($tools.Count -eq 19) 'tool count';Assert (!@($tools|Where-Object {$_.name -match 'snapshot|input_|eval_gd|run_gd'}).Count) 'Removed or unsupported tool exposed'}
    Test 'runtime identity and independent project' {$info=Tool info;Assert ($info.playing -and $info.project.Equals($projectRoot,[StringComparison]::OrdinalIgnoreCase)) 'Wrong runtime project';Assert ((Tool tree @{depth=1}).children.Count -gt 0) 'tree'}
    Test 'scene helpers and persistent C# session' {Assert ((Eval 'int fixtureSession=40; fixtureSession+2') -eq 42) 'session declare';Assert ((Eval 'fixtureSession+=1; fixtureSession') -eq 41) 'session continuation';Assert (Eval 'Root!=null && FindAll<CameraComponent>().Any()') 'scene helper'}
    Test 'async main-thread continuation' {Assert (Eval 'var beforeThread=Environment.CurrentManagedThreadId;await Task.Delay(30);return beforeThread==Environment.CurrentManagedThreadId && Root!=null;') 'continuation left game thread'}
    Test 'worker evaluation and scene guard' {Assert ((Eval '6*7' @{thread='worker'}) -eq 42) 'worker value';$r=RawTool eval_cs @{thread='worker';code='Root.Name'};Assert (!$r.ok -and $r.error -like '*require thread:main*') 'worker scene access was accepted'}
    Test 'C# diagnostic and removed tool rejection' {$r=RawTool eval_cs @{code='invalid C# syntax'};Assert (!$r.ok -and $r.error -match 'compilation') 'Invalid script accepted';Assert (!(RawTool snapshot_save).ok) 'snapshot remained callable';Assert (!(RawTool eval_gd @{code='pass'}).ok) 'GDScript was treated as C#'}
    Test 'generic object property and method access' {Eval 'var fixtureObject=Root.CreateObject();fixtureObject.Name="AIDebugFixture";return true;';Tool set @{node='AIDebugFixture';property='Name';value='AIDebugFixtureRenamed'};Assert ((Tool get @{node='AIDebugFixtureRenamed';property='Name'}).value -eq 'AIDebugFixtureRenamed') 'set/get';Tool set @{node='AIDebugFixtureRenamed';property='WorldPosition:x';value=12.5};Assert ((Tool get @{node='AIDebugFixtureRenamed';property='WorldPosition:x'}).value -eq 12.5) 'nested struct property';Assert ((Tool call @{node='AIDebugFixtureRenamed';method='ToString';args=@()}).result) 'call'}
    Test 'fixture assembly and method resolution' {Assert (Test-Path -LiteralPath $fixture) 'Build the fixture first';Eval ('System.Runtime.Loader.AssemblyLoadContext.Default.LoadFromAssemblyPath(@"'+$fixture.Replace('"','""')+'");return true;');Assert ((Eval 'SboxDebugFixture.Probe.Add(1)') -eq 8) 'fixture baseline'}
    Test 'managed replace, counters and unpatch' {$p=Tool patch_cs @{target=$target;kind='replace';code='return value*3;'};Assert ((Eval 'SboxDebugFixture.Probe.Add(4)') -eq 12) 'replacement';Assert (@((Tool list_patches).patches).Count -eq 1) 'patch listing';Tool unpatch @{id='all'};Assert ((Eval 'SboxDebugFixture.Probe.Add(4)') -eq 11) 'unpatch'}
    Test 'managed prefix and postfix' {Tool patch_cs @{target=$target;kind='prefix';code='value+=1;'};Tool patch_cs @{target=$target;kind='postfix';code='__result*=2;'};Assert ((Eval 'SboxDebugFixture.Probe.Add(1)') -eq 18) 'prefix/postfix';Tool unpatch @{id='all'}}
    Test 'managed finalizer and transpiler' {Tool patch_cs @{target='SboxDebugFixture.Probe:Throws()';kind='finalizer';code='public static Exception Finalizer(Exception __exception){return null;}'};Assert ((Eval 'SboxDebugFixture.Probe.Throws()') -eq 0) 'finalizer';Tool unpatch @{id='all'};Tool patch_cs @{target=$target;kind='transpiler';code='public static System.Collections.Generic.IEnumerable<HarmonyLib.CodeInstruction> Transpiler(System.Collections.Generic.IEnumerable<HarmonyLib.CodeInstruction> instructions){return instructions;}'};Assert ((Eval 'SboxDebugFixture.Probe.Add(2)') -eq 9) 'transpiler';Tool unpatch @{id='all'}}
    Test 'failed managed patch preserves active registry' {$bad=RawTool patch_cs @{target=$target;kind='replace';code='not C#'};Assert (!$bad.ok) 'invalid patch accepted';Assert (@((Tool list_patches).patches).Count -eq 0) 'failed patch registered'}
    Test 'pause, scene ticks and time scale' {Tool pause @{paused=$true};Assert ((Tool pause).paused) 'pause';Assert ((Tool step @{frames=3}).frames -eq 3) 'step';Assert ((Tool pause).paused) 'step must remain paused';Assert ((Tool time_scale @{scale=0.5}).scale -eq 0.5) 'scale'}
    Test 'PNG runtime camera capture' {$shot=Tool screenshot @{max_width=320};$bytes=[IO.File]::ReadAllBytes($shot.path);Assert ($bytes[0] -eq 137 -and $bytes[1] -eq 80 -and $shot.width -eq 320) 'PNG screenshot'}
    Test 'log capture' {Eval 'Log("AIDebug fixture message");return true;';$r=Tool logs @{limit=100};Assert (@($r.entries|Where-Object message -like '*AIDebug fixture message*').Count -gt 0) 'logs'}
    Test 'native fixture ABI baseline' {Assert (Test-Path -LiteralPath $native) 'Build native fixture first';Eval ('SboxDebugFixture.Probe.LoadNative(@"'+$native.Replace('"','""')+'");return true;');Assert ((Eval 'SboxDebugFixture.Probe.NativeInteger(1)') -eq 8) 'native integer';Assert ((Eval 'SboxDebugFixture.Probe.NativeFloating(1,2,3,4)') -eq 30) 'four doubles'}
    Test 'native return constant and unhook' {Tool hook_native @{symbol='native_fixture.dll!probe_integer';mode='return_const';return_const=55};Assert ((Eval 'SboxDebugFixture.Probe.NativeInteger(1)') -eq 55) 'constant result';Tool unhook_native @{id='all'};Assert ((Eval 'SboxDebugFixture.Probe.NativeInteger(1)') -eq 8) 'native unhook'}
    Test 'native log preserves four floating arguments' {Tool hook_native @{symbol='native_fixture.dll!probe_four_doubles';mode='log'};Assert ((Eval 'SboxDebugFixture.Probe.NativeFloating(1,2,3,4)') -eq 30) 'floating ABI was changed';$hooks=(Tool list_native_hooks).hooks;Assert ($hooks.Count -eq 1 -and $hooks[0].call_count -gt 0) 'native count';Tool unhook_native @{id='all'}}
    Test 'native DLL replacement' {Tool hook_native @{symbol='native_fixture.dll!probe_integer';mode='dll';dll_path=$native;export='hook_replacement'};Assert ((Eval 'SboxDebugFixture.Probe.NativeInteger(3)') -eq 103) 'DLL replacement';Tool unhook_native @{id='all'}}
    Test 'Windows minidump' {$dump=Tool dump @{mode='mini'};$head=[IO.File]::ReadAllBytes($dump.path);Assert ([Text.Encoding]::ASCII.GetString($head,0,4) -eq 'MDMP') 'dump header'}
    Test 'concurrent HTTP response association' {$pending=@(1..8|ForEach-Object {Start-Rpc 'tools/call' @{name='info';arguments=@{}}});foreach($p in $pending){$r=Finish-Rpc $p;Assert ($r.content[0].text|ConvertFrom-Json).pid 'concurrent reply'}}
    Test 'single execution slot and deadline cancellation' {
        $pending=Start-Rpc 'tools/call' @{name='eval_cs';arguments=@{thread='worker';code='await Task.Delay(500);return 9;';timeout_ms=3000}}
        Start-Sleep -Milliseconds 100;$busy=RawTool eval_cs @{code='1+1'};Assert $busy.busy 'Concurrent eval was not busy';Assert (((Finish-Rpc $pending).content[0].text|ConvertFrom-Json).result -eq 9) 'pending result'
        $timeout=RawTool eval_cs @{thread='worker';code='System.Threading.Thread.Sleep(400);return 5;';timeout_ms=50};Assert (!$timeout.ok -and $timeout.timeout) 'deadline response'
        $reset=Tool reset;Assert $reset.execution_may_continue 'Ignoring cancellation must be reported';Assert (RawTool eval_cs @{code='1+1'}).busy 'Reset opened a second execution slot';Start-Sleep -Milliseconds 500;Assert ((Eval '40+2') -eq 42) 'slot did not recover'
    }
    Test 'reset clears patches and controls but preserves gameplay edits' {Tool patch_cs @{target=$target;kind='replace';code='return 99;'};Tool hook_native @{symbol='native_fixture.dll!probe_integer';mode='return_const';return_const=55};Tool pause @{paused=$true};Tool time_scale @{scale=0.25};Tool reset;Assert (@((Tool list_patches).patches).Count -eq 0) 'managed reset';Assert (@((Tool list_native_hooks).hooks).Count -eq 0) 'native reset';Assert ((Tool get @{node='AIDebugFixtureRenamed';property='Name'}).value -eq 'AIDebugFixtureRenamed') 'Reset rolled back arbitrary gameplay state'}
    Test 'HTTP origin, host, method and optional token checks' {
        $headers=@{};if($Token){$headers.Authorization='Bearer '+$Token}
        $body='{"jsonrpc":"2.0","id":1,"method":"ping"}'
        $bad=$headers.Clone();$bad.Origin='https://untrusted.invalid';Assert ((Invoke-WebRequest $BaseUrl -Method Post -Headers $bad -Body $body -SkipHttpErrorCheck).StatusCode -eq 403) 'Origin'
        $bad=$headers.Clone();$bad.Host='untrusted.invalid';Assert ((Invoke-WebRequest $BaseUrl -Method Post -Headers $bad -Body $body -SkipHttpErrorCheck).StatusCode -eq 403) 'Host'
        Assert ((Invoke-WebRequest $BaseUrl -Method Get -Headers $headers -SkipHttpErrorCheck).StatusCode -eq 405) 'GET'
        if($Token){Assert ((Invoke-WebRequest $BaseUrl -Method Post -Body $body -SkipHttpErrorCheck).StatusCode -eq 401) 'Token'}
    }
    if(!$SkipGuide){Test 'optional AI Guide forwarding' {$body=@{jsonrpc='2.0';id=1;method='tools/call';params=@{name='ai_debug_info';arguments=@{}}}|ConvertTo-Json -Depth 8;$r=Invoke-RestMethod 'http://127.0.0.1:7660/mcp' -Method Post -ContentType application/json -Body $body;Assert (($r.result.content[0].text|ConvertFrom-Json).pid -eq (Tool info).pid) 'Guide forwarding'}}
}finally{
    try{Tool reset|Out-Null}catch{}
    $http.Dispose()
    $results|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $ResultsPath -Encoding utf8
}
$failed=@($results|Where-Object {!$_.passed});Write-Output "$($results.Count-$failed.Count)/$($results.Count) passed; $ResultsPath"
if($failed.Count){exit 1}
