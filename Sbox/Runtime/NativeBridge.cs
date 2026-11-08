using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
namespace AIDebug.Sbox;
internal static class NativeBridge
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr Error();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate ulong Resolve([MarshalAs(UnmanagedType.LPUTF8Str)]string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Create(ulong address,int mode,long constant,ulong replacement);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Remove(int id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate long Count(int id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadLogs(int id,IntPtr buffer,int capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int WriteDump([MarshalAs(UnmanagedType.LPWStr)]string path,int full);
    [StructLayout(LayoutKind.Sequential)] private struct Record {public ulong time,rcx,rdx,r8,r9,result;}
    private sealed record Entry(int Id,string Symbol,ulong Address,string Mode,long Constant,string Dll,string Export,IntPtr ReplacementModule);
    private static readonly Dictionary<int,Entry> entries=new();
    private static IntPtr module;
    private static Error error=null!;private static Resolve resolve=null!;private static Create create=null!;private static Remove remove=null!;private static Count count=null!;private static ReadLogs read=null!;private static WriteDump dump=null!;
    private static T Export<T>(string name) where T:Delegate=>Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(module,"aidbg_"+name));
    private static void Load(){
        if(module!=IntPtr.Zero)return;
        var source=Path.Combine(RuntimeService.Root,"Editor","AIDebug","Native","aidebug_native.dll");
        if(!File.Exists(source))throw new FileNotFoundException("Build and reinstall the AIDebug Sbox native adapter",source);
        var hash=Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(source)))[..16];
        var dir=Path.Combine(RuntimeService.Data,"native-cache",hash);Directory.CreateDirectory(dir);var path=Path.Combine(dir,"aidebug_native.dll");if(!File.Exists(path))File.Copy(source,path);
        module=NativeLibrary.Load(path);error=Export<Error>("error");resolve=Export<Resolve>("resolve");create=Export<Create>("hook");remove=Export<Remove>("unhook");count=Export<Count>("count");read=Export<ReadLogs>("logs");dump=Export<WriteDump>("dump");
    }
    private static Exception Failure()=>new InvalidOperationException(Marshal.PtrToStringUTF8(error())??"Native debugger failed");
    internal static JsonObject Hook(JsonObject args){
        Load();var symbol=args.S("symbol");var mode=args.S("mode","log");var kind=mode switch{"log"=>0,"return_const"=>1,"dll"=>2,_=>throw new ArgumentException("Unknown native hook mode")};
        var address=resolve(symbol);if(address==0)throw Failure();
        var constant=args["return_const"]?.GetValue<long>()??0;var library=IntPtr.Zero;ulong replacement=0;var dll=args.S("dll_path");var export=args.S("export","hook_replacement");
        if(kind==2){library=NativeLibrary.Load(Path.GetFullPath(dll,RuntimeService.Root));replacement=(ulong)NativeLibrary.GetExport(library,export).ToInt64();}
        var id=create(address,kind,constant,replacement);if(id<1)throw Failure();
        // Replacement modules, retired stubs and trampolines remain resident for in-flight calls.
        var entry=new Entry(id,symbol,address,mode,constant,dll,export,library);entries.Add(id,entry);
        return Describe(entry);
    }
    private static JsonObject Describe(Entry entry){
        var recent=new JsonArray();var bytes=Marshal.SizeOf<Record>();var memory=Marshal.AllocHGlobal(bytes*64);
        try{var n=read(entry.Id,memory,64);for(var i=0;i<n;i++){var r=Marshal.PtrToStructure<Record>(memory+i*bytes);recent.Add(new JsonObject{["timestamp_msec"]=r.time,["rcx"]=r.rcx,["rdx"]=r.rdx,["r8"]=r.r8,["r9"]=r.r9,["return_value"]=r.result});}}
        finally{Marshal.FreeHGlobal(memory);}
        return new JsonObject{["ok"]=true,["id"]="nh"+entry.Id,["symbol"]=entry.Symbol,["address"]="0x"+entry.Address.ToString("X"),["mode"]=entry.Mode,["return_const"]=entry.Constant,["call_count"]=count(entry.Id),["recent_calls"]=recent};
    }
    internal static JsonObject List(){var array=new JsonArray();foreach(var e in entries.Values)array.Add(Describe(e));return new(){["ok"]=true,["hooks"]=array};}
    internal static JsonObject Unhook(string id){
        var all=id is "" or "all" or "*";var selected=entries.Values.Where(e=>all||"nh"+e.Id==id||e.Symbol==id).ToArray();
        if(!all&&selected.Length==0)throw new ArgumentException("Native hook not found: "+id);
        var errors=new JsonArray();var removed=new JsonArray();
        foreach(var entry in selected){if(remove(entry.Id)!=0){errors.Add(Failure().Message);continue;}entries.Remove(entry.Id);removed.Add("nh"+entry.Id);}
        return new(){["ok"]=errors.Count==0,["unhooked"]=removed,["errors"]=errors};
    }
    internal static JsonObject Dump(string requested,string mode){
        if(mode is not ("mini" or "full"))throw new ArgumentException("dump mode must be mini or full");
        Load();var path=RuntimeService.Artifact("dumps",requested,".dmp");if(dump(path,mode=="full"?1:0)!=0)throw Failure();return new(){["ok"]=true,["path"]=path,["mode"]=mode};
    }
}
