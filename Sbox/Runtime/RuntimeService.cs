using System.Collections.Concurrent;
using System.Reflection;
using System.Text.Json.Nodes;
using System.Threading;
using Sandbox;
using Sandbox.Diagnostics;

namespace AIDebug.Sbox;

/// <summary>Trusted, explicitly activated debugger; no project gameplay interfaces or engine patches.</summary>
public static class RuntimeService
{
    public const string Version="0.2.0";
    private static DebugTransport? transport;
    private static string project="", token="";
    private static int port;
    private static DateTime lastUse;
    private static Scene? capturedScene;
    private static bool initialPaused;
    private static float initialScale=1;
    private static readonly ConcurrentDictionary<long,byte> jobs=new();
    private static readonly ConcurrentQueue<JsonObject> logs=new();
    private static Delegate? logSink;
    private static EventInfo? logEvent;
    private static Timer? idle;
    private static int idleReset;
    private static bool exitSubscribed;
    private static readonly Logger logger=new("AI Debug");
    public static bool Active=>transport!=null;
    internal static string Root=>project;
    internal static string Library { get; private set; }="";
    internal static string Token=>token;
    internal static string Data=>Path.Combine(project,".ai_debug");
    internal static IDisposable GameScope()=>(IDisposable)typeof(Game).Assembly.GetType("Sandbox.Engine.GlobalContext",true)!.GetMethod("GameScope",BindingFlags.Public|BindingFlags.Static)!.Invoke(null,null)!;
    internal static async Task<T> Main<T>(Func<T> work,CancellationToken cancellation=default)
    {
        var completion=new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        var expired=0;
        MainThread.Queue(()=>{
            if(Volatile.Read(ref expired)!=0)return;
            if(cancellation.IsCancellationRequested){completion.TrySetCanceled(cancellation);return;}
            try{using var game=GameScope();var context=new ScriptContext();completion.TrySetResult(context.Run(work));}
            catch(Exception e){completion.TrySetException(e);}
        });
        try{return await completion.Task.WaitAsync(TimeSpan.FromSeconds(30),cancellation).ConfigureAwait(false);}
        finally{Interlocked.Exchange(ref expired,1);}
    }
    internal static Task<T> StartOnMain<T>(Func<Task<T>> work)=>Task.Run(async()=>await await Main(work));
    private sealed class ScriptContext:SynchronizationContext
    {
        internal T Run<T>(Func<T> work){var old=Current;SetSynchronizationContext(this);try{return work();}finally{SetSynchronizationContext(old);}}
        public override void Post(SendOrPostCallback callback,object? state){_=Task.Run(()=>Main(()=>Run(()=>{callback(state);return true;})));}
        public override SynchronizationContext CreateCopy()=>this;
    }
    public static string Start(string projectRoot,int listenPort=7651,string bearerToken="",string libraryRoot="")
    {
        if(Active){if(!project.Equals(Path.GetFullPath(projectRoot),StringComparison.OrdinalIgnoreCase)||port!=listenPort)throw new InvalidOperationException("A different debug session is already active");return Status();}
        if(!OperatingSystem.IsWindows()||!Environment.Is64BitProcess)throw new PlatformNotSupportedException("The s&box adapter currently targets Windows x64");
        project=Path.GetFullPath(projectRoot);token=bearerToken;port=listenPort;
        Library=Path.GetFullPath(string.IsNullOrEmpty(libraryRoot)?Path.Combine(project,"Libraries","AIDebug"):libraryRoot);
        if(!Directory.EnumerateFiles(project,"*.sbproj").Any())throw new ArgumentException("Not an s&box project");
        using var game=GameScope();
        Agent.Start(Data);AIDebugGlobals.SceneProvider=()=>Game.IsPlaying?Game.ActiveScene:null;AIDebugGlobals.Logger=s=>logger.Info(s);
        CaptureRunState();lastUse=DateTime.UtcNow;
        var next=new DebugTransport(listenPort);next.Start();transport=next;
        Directory.CreateDirectory(Data);
        File.WriteAllText(Path.Combine(Data,"session.json"),new JsonObject{["pid"]=Environment.ProcessId,["project"]=project,["url"]=$"http://127.0.0.1:{port}/mcp",["version"]=Version}.ToJsonString());
        if(!exitSubscribed){AppDomain.CurrentDomain.ProcessExit+=(_,_)=>{try{DeleteOwnSession();}catch{}};exitSubscribed=true;}
        SubscribeLogs();idle=new Timer(_=>{
            if(DateTime.UtcNow-lastUse<TimeSpan.FromMinutes(30)||Interlocked.Exchange(ref idleReset,1)!=0)return;
            _=Task.Run(async()=>{try{await Reset();}finally{Interlocked.Exchange(ref idleReset,0);}});
        },null,1000,1000);
        return Status();
    }
    public static string Status()=>new JsonObject{["active"]=Active,["pid"]=Environment.ProcessId,["project"]=project,["url"]=Active?$"http://127.0.0.1:{port}/mcp":null,["version"]=Version}.ToJsonString();
    internal static JsonArray Definitions()=>JsonNode.Parse(File.ReadAllText(Path.Combine(Library,"Editor","AIDebug","ToolDefinitions.json")))!.AsArray();
    private static void CaptureRunState(){if(capturedScene==null&&Game.IsPlaying&&Game.ActiveScene is {} scene){capturedScene=scene;initialPaused=Game.IsPaused;initialScale=scene.TimeScale;}}
    private static void SubscribeLogs()
    {
        logEvent=typeof(LogEvent).Assembly.GetType("Sandbox.Diagnostics.Logging",true)!.GetEvent("OnMessage",BindingFlags.NonPublic|BindingFlags.Static);
        if(logEvent==null)throw new NotSupportedException("The engine log event is unavailable");
        Action<LogEvent> handler=e=>{logs.Enqueue(new(){["time"]=e.Time.ToString("O"),["level"]=e.Level.ToString(),["message"]=e.Message,["logger"]=e.Logger});while(logs.Count>500)logs.TryDequeue(out _);};
        logSink=handler;logEvent.GetAddMethod(true)!.Invoke(null,new object[]{handler});
    }
    private static JsonObject Managed(string command,JsonObject args)=>JsonNode.Parse(Agent.Dispatch(command,args.ToJsonString()))!.AsObject();
    public static async Task<string> CallToolJson(string name,string argsJson)
    {
        try{return (await Call(name,JsonNode.Parse(argsJson)?.AsObject()??new(),CancellationToken.None))?.ToJsonString()??"null";}
        catch(Exception e){return J.Error(e).ToJsonString();}
    }
    internal static async Task<JsonNode?> Call(string name,JsonObject args,CancellationToken cancellation)
    {
        if(!Active)throw new InvalidOperationException("Activate AI Debug first through its editor menu or Start-AIDebug.ps1");
        if(name.StartsWith("ai_debug_",StringComparison.Ordinal))name=name[9..];
        if(!Definitions().Any(x=>x?["name"]?.GetValue<string>()==name))throw new ArgumentException("Unknown AI Debug tool: "+name);
        lastUse=DateTime.UtcNow;
        if(name=="eval_cs")return await Evaluate(args,cancellation);
        if(name=="reset")return await Reset();
        if(name=="logs"){
            var entries=logs.Where(x=>args.S("level")==""||x.S("level").Equals(args.S("level"),StringComparison.OrdinalIgnoreCase)).TakeLast(Math.Clamp(args.I("limit",100),1,500)).ToArray();
            if(args.B("clear"))while(logs.TryDequeue(out _)){}
            return J.Node(new{ok=true,entries});
        }
        return await Main(()=>{
            CaptureRunState();
            switch(name){
                case "info":return J.Node(new{ok=true,engine=Application.Version,pid=Environment.ProcessId,project,version=Version,playing=Game.IsPlaying,scene=Game.ActiveScene?.Name,paused=Game.IsPaused,time_scale=Game.ActiveScene?.TimeScale,managed=Managed("info",new()),native_hooks=NativeBridge.List(),scope="editor Play or explicitly attached standalone game",unsupported_tools=new[]{"eval_gd","run_gd","snapshot_*","input_*"}});
                case "tree":return SceneAccess.Tree(args);
                case "get":return new JsonObject{["ok"]=true,["value"]=Value(SceneAccess.Get(SceneAccess.Target(args.S("node")),args.S("property")))};
                case "set":SceneAccess.Set(SceneAccess.Target(args.S("node")),args.S("property"),args["value"]);return new JsonObject{["ok"]=true};
                case "call":return new JsonObject{["ok"]=true,["result"]=Value(SceneAccess.Invoke(SceneAccess.Target(args.S("node")),args.S("method"),args.A("args")))};
                case "patch_cs":case "unpatch":case "list_patches":return Managed(name,args);
                case "pause":SceneAccess.RunningScene();if(args.ContainsKey("paused"))Game.IsPaused=args.B("paused");return new JsonObject{["ok"]=true,["paused"]=Game.IsPaused};
                case "step":{
                    var scene=SceneAccess.RunningScene();if(!Game.IsPaused)throw new InvalidOperationException("Pause the game before stepping");
                    var frames=args.I("frames",1);if(frames is <1 or >1000)throw new ArgumentOutOfRangeException("frames");
                    Game.IsPaused=false;try{using var scope=scene.Push();for(var i=0;i<frames;i++)scene.GameTick(1.0/60);}finally{Game.IsPaused=true;}
                    return new JsonObject{["ok"]=true,["frames"]=frames,["paused"]=true,["scope"]="scene simulation ticks; not transport or real-time clock"};
                }
                case "time_scale":{
                    var scene=SceneAccess.RunningScene();if(args.ContainsKey("scale")){var scale=args["scale"]!.GetValue<float>();if(!float.IsFinite(scale)||scale<0||scale>10)throw new ArgumentOutOfRangeException("scale");scene.TimeScale=scale;}
                    return new JsonObject{["ok"]=true,["scale"]=scene.TimeScale};
                }
                case "screenshot":return Screenshot(args);
                case "hook_native":return NativeBridge.Hook(args);
                case "unhook_native":return NativeBridge.Unhook(args.S("id","all"));
                case "list_native_hooks":return NativeBridge.List();
                case "dump":return NativeBridge.Dump(args.S("path"),args.S("mode","mini"));
                default:throw new ArgumentException("Unknown tool: "+name);
            }
        },cancellation);
    }
    private static JsonNode? Value(object? value)
    {
        if(value==null)return null;
        if(value is GameObject g)return new JsonObject{["id"]=g.Id.ToString(),["name"]=g.Name};
        if(value is Component c)return new JsonObject{["id"]=c.Id.ToString(),["type"]=c.GetType().FullName,["object"]=c.GameObject.Name};
        if(value is Resource r)return JsonValue.Create(r.ResourcePath);
        if(value is Task)throw new ArgumentException("Await asynchronous methods through eval_cs");
        try{return J.Node(value);}catch{return JsonValue.Create(value.ToString());}
    }
    internal static string Artifact(string category,string requested,string extension)
    {
        var path=string.IsNullOrEmpty(requested)?Path.Combine(Data,category,Guid.NewGuid().ToString("N")+extension):Path.GetFullPath(requested,project);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);return path;
    }
    private static JsonObject Screenshot(JsonObject args)
    {
        var camera=SceneAccess.RunningScene().Camera??throw new InvalidOperationException("The game scene has no camera");
        var width=Math.Clamp(args.I("max_width",1280),16,4096);var height=(width*9/16);
        using var bitmap=new Bitmap(width,height);camera.RenderToBitmap(bitmap,true);var png=bitmap.ToPng();var path=Artifact("screenshots",args.S("path"),".png");File.WriteAllBytes(path,png);
        return new JsonObject{["ok"]=true,["path"]=path,["width"]=width,["height"]=height,["base64"]=args.B("base64")?Convert.ToBase64String(png):null};
    }
    private static async Task<JsonObject> Evaluate(JsonObject args,CancellationToken cancellation)
    {
        var start=await Main(()=>Managed("eval_cs_start",args),cancellation);
        if(!start.B("ok"))return start;
        var id=start["job_id"]!.GetValue<long>();jobs[id]=0;
        var jobArg=new JsonObject{["job_id"]=id};var deadline=DateTime.UtcNow.AddMilliseconds(Math.Clamp(args.I("timeout_ms",10000),1,300000));
        try{
            while(DateTime.UtcNow<deadline&&!cancellation.IsCancellationRequested){
                var poll=Managed("eval_cs_poll",jobArg);
                if(!poll.B("ok"))return poll;
                if(poll.B("done"))return poll.O("result");
                await Task.Delay(20,cancellation).ConfigureAwait(false);
            }
        }catch(OperationCanceledException){}
        finally{jobs.TryRemove(id,out _);}
        var cancelled=Managed("eval_cs_cancel",jobArg);
        return new JsonObject{["ok"]=false,["timeout"]=!cancellation.IsCancellationRequested,["cancelled"]=cancellation.IsCancellationRequested,["execution_may_continue"]=cancelled.B("execution_may_continue"),["error"]="C# execution response deadline expired or the client disconnected; cancellation is cooperative."};
    }
    private static async Task<JsonObject> Reset()
    {
        foreach(var id in jobs.Keys)Managed("eval_cs_cancel",new(){["job_id"]=id});
        return await Main(()=>{
            var managed=Managed("reset",new());var native=NativeBridge.Unhook("all");
            if(capturedScene!=null&&Game.ActiveScene==capturedScene){Game.IsPaused=initialPaused;capturedScene.TimeScale=initialScale;}
            capturedScene=null;lastUse=DateTime.UtcNow;
            return new JsonObject{["ok"]=managed.B("ok")&&native.B("ok"),["managed"]=managed,["native"]=native,["execution_may_continue"]=managed.B("execution_may_continue"),["note"]="Session, owned patches, pause and time scale reset; arbitrary gameplay mutations are retained."};
        });
    }
    public static async Task<string> Stop()
    {
        if(!Active)return new JsonObject{["ok"]=true,["active"]=false}.ToJsonString();
        var result=await Reset();if(!result.B("ok"))return result.ToJsonString();
        transport?.Dispose();transport=null;idle?.Dispose();idle=null;
        if(logSink!=null)logEvent?.GetRemoveMethod(true)?.Invoke(null,new object[]{logSink});logSink=null;
        DeleteOwnSession();
        return result.ToJsonString();
    }
    private static void DeleteOwnSession(){var path=Path.Combine(Data,"session.json");if(File.Exists(path)&&JsonNode.Parse(File.ReadAllText(path))?["pid"]?.GetValue<int>()==Environment.ProcessId)File.Delete(path);}
}
