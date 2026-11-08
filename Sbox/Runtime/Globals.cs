using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using System.Threading;
using Sandbox;

namespace AIDebug.Sbox;



public class AIDebugGlobals
{
    public static Func<Scene?> SceneProvider { get; set; } = () => Game.ActiveScene;
    public static Action<string>? Logger { get; set; }
    public CancellationToken Cancellation { get; internal set; }
    public List<string> Logs { get; } = new();
    internal static void RequireMainThread(){if(Environment.CurrentManagedThreadId != Agent.MainThreadId)throw new InvalidOperationException("Scene helpers require thread:main; worker scripts must use thread-safe data.");}
    public Scene? Tree { get { RequireMainThread(); return SceneProvider(); } }
    public Scene? Root => Tree;
    public Scene? CurrentScene => Tree;
    public Dictionary<string,object?> Session { get; } = new();
    public GameObject? Node(string path)
    {
        var scene=Tree;if(scene is null)return null;
        if(path is "" or "." or "/root" or "root")return scene;
        var p=path.Trim('/');if(p.StartsWith("root/"))p=p[5..];
        if(Guid.TryParse(p,out var id))return scene.GetAllObjects(true).FirstOrDefault(o=>o.Id==id);
        GameObject? current=scene;
        foreach(var segment in p.Split('/',StringSplitOptions.RemoveEmptyEntries)){current=current?.Children.FirstOrDefault(o=>o.Name==segment);if(current is null)break;}
        return current??scene.GetAllObjects(true).FirstOrDefault(o=>o.Name==p);
    }
    public IEnumerable<T> FindAll<T>(string? name=null) where T:class => Objects().OfType<T>().Where(o=>name==null||o is GameObject g&&g.Name==name||o is Component c&&c.GameObject.Name==name);
    public T? Find<T>(string? name=null) where T:class=>FindAll<T>(name).FirstOrDefault();
    public IEnumerable<object> Objects()=>Tree?.GetAllObjects(true).SelectMany(o=>new object[]{o}.Concat(o.Components.GetAll()))??Enumerable.Empty<object>();
    public void Log(object? value){var text=value?.ToString()??"null";lock(Logs)Logs.Add(text);Logger?.Invoke(text);}
    public Traverse Traverse(object target)=>HarmonyLib.Traverse.Create(target);
    public object? GetPrivate(object target,string name)
    {
        if(target is null)throw new ArgumentNullException(nameof(target));
        for(var type=target as Type??target.GetType();type!=null;type=type.BaseType){var flags=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static|BindingFlags.Instance|BindingFlags.DeclaredOnly;var p=type.GetProperty(name,flags);if(p!=null)return p.GetValue(target is Type?null:target);var f=type.GetField(name,flags);if(f!=null)return f.GetValue(target is Type?null:target);}
        throw new MissingMemberException(target.GetType().FullName,name);
    }
    public void SetPrivate(object target,string name,object? value)
    {
        for(var type=target as Type??target.GetType();type!=null;type=type.BaseType){var flags=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static|BindingFlags.Instance|BindingFlags.DeclaredOnly;var p=type.GetProperty(name,flags);if(p!=null){p.SetValue(target is Type?null:target,value);return;}var f=type.GetField(name,flags);if(f!=null){f.SetValue(target is Type?null:target,value);return;}}
        throw new MissingMemberException(target.GetType().FullName,name);
    }
    public object? CallPrivate(object target,string method,params object?[] args)
    {
        var methods=(target as Type??target.GetType()).GetMethods(BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static|BindingFlags.Instance).Where(m=>m.Name==method&&m.GetParameters().Length==args.Length).ToArray();
        if(methods.Length!=1)throw new AmbiguousMatchException($"Expected one {method} overload matching {args.Length} arguments; found {methods.Length}");
        return methods[0].Invoke(target is Type?null:target,args);
    }
}
