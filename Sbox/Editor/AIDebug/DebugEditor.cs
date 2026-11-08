using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using Editor;
using Editor.Mcp;
using Sandbox;
namespace AIDebug.Sbox.EditorIntegration;
public static class DebugEditor
{
    private static Type service;
    private static DateTime next;
    private static string error;
    private static bool suspended;
    private static string Root=>Path.GetFullPath(Project.Current.GetRootPath());
    private static Type Load(){
        if(service!=null)return service;
        var dir=Path.Combine(Root,"Editor","AIDebug","Managed");
        var assembly=AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a=>a.GetName().Name=="AIDebug.Sbox")??AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(dir,"AIDebug.Sbox.dll"));
        return service=assembly.GetType("AIDebug.Sbox.RuntimeService",true);
    }
    private static object Invoke(string method,params object[] args)=>Load().GetMethod(method).Invoke(null,args);
    [EditorEvent.Frame]
    public static void Frame(){
        if(Project.Current==null||DateTime.UtcNow<next)return;next=DateTime.UtcNow.AddSeconds(1);
        try{
            var path=Path.Combine(Root,".ai_debug","config.json");var config=File.Exists(path)?JsonNode.Parse(File.ReadAllText(path)).AsObject():new JsonObject();
            if(!suspended&&(config["enabled"]?.GetValue<bool>()==true||Environment.GetEnvironmentVariable("SBOX_AI_DEBUG")=="1"))Start();
        }catch(Exception e){error=e.GetBaseException().Message;}
    }
    [Menu("Editor","AI Debug/启动本地调试","bug_report")]
    [Shortcut("aidebug.start","CTRL+ALT+SHIFT+F12",ShortcutType.Window)]
    public static void StartMenu()=>Start();
    [McpTool("aidebug_start")]
    public static object Start(){
        suspended=false;
        var path=Path.Combine(Root,".ai_debug","config.json");var config=File.Exists(path)?JsonNode.Parse(File.ReadAllText(path)).AsObject():new JsonObject();
        var port=config["port"]?.GetValue<int>()??7651;var token=config["token"]?.GetValue<string>()??"";
        var result=(string)Invoke("Start",Root,port,token);error=null;return JsonNode.Parse(result);
    }
    [Menu("Editor","AI Debug/重置并停止调试","stop")]
    public static void StopMenu()=>_=Stop();
    [McpTool("aidebug_stop")]
    public static async Task<object> Stop(){suspended=true;if(service==null)return new{ok=true,active=false};return JsonNode.Parse(await (Task<string>)Invoke("Stop"));}
    [McpTool.ReadOnly("aidebug_status")]
    public static object Status()=>service==null?new{active=false,error}:JsonNode.Parse((string)Invoke("Status"));
    [McpTool.ReadOnly("aidebug_tools")]
    public static object Tools()=>JsonNode.Parse(File.ReadAllText(Path.Combine(Root,"Editor","AIDebug","ToolDefinitions.json")));
    [McpTool("aidebug_call")]
    public static async Task<object> Call(string tool,JsonObject args=null){
        if(service==null)throw new InvalidOperationException("Start AI Debug first through aidebug_start or its editor menu");
        return JsonNode.Parse(await (Task<string>)Invoke("CallToolJson",tool,(args??new()).ToJsonString()));
    }
    [EditorEvent.Hotload]
    public static void Hotload(){if(service!=null)_=Call("reset");}
}
