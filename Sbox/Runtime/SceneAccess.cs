using System.Reflection;
using System.Text.Json.Nodes;
using Sandbox;
namespace AIDebug.Sbox;
internal static class SceneAccess {
    internal static Scene RunningScene()=>Game.IsPlaying?Game.ActiveScene??throw new InvalidOperationException("No active game scene"):throw new InvalidOperationException("Enter Play or connect a running game first");
    internal static object Target(string path,bool runtime=true)
    {
        var scene=RunningScene()??throw new InvalidOperationException(runtime?"No running game; enter Play first.":"No editor scene.");
        var split=path.Split('#',2);var name=split[0].Trim('/');
        var objects=scene.GetAllObjects(true).Prepend(scene).Distinct().ToArray();
        GameObject obj;
        if(name is "" or "." or "root")obj=scene;
        else if(Guid.TryParse(name,out var id))obj=objects.FirstOrDefault(o=>o.Id==id);
        else{obj=scene;var segments=name.Split('/',StringSplitOptions.RemoveEmptyEntries);foreach(var part in segments){if(part=="root")continue;obj=obj?.Children.FirstOrDefault(o=>o.Name==part);}obj??=objects.FirstOrDefault(o=>o.Name==name);}
        if(obj==null){foreach(var component in objects.SelectMany(o=>o.Components.GetAll()))if(component.Id.ToString()==name)return component;throw new ArgumentException("Object not found: "+path);}
        if(split.Length==1)return obj;
        return obj.Components.GetAll().FirstOrDefault(c=>c.GetType().Name==split[1]||c.GetType().FullName==split[1]||c.Id.ToString()==split[1])??throw new ArgumentException("Component not found: "+path);
    }
    private const BindingFlags Flags=BindingFlags.Instance|BindingFlags.Static|BindingFlags.Public|BindingFlags.NonPublic;
    private static MemberInfo Member(object target,string name)
    {
        for(var t=target.GetType();t!=null;t=t.BaseType){var p=t.GetProperty(name,Flags|BindingFlags.DeclaredOnly);if(p!=null)return p;var f=t.GetField(name,Flags|BindingFlags.DeclaredOnly);if(f!=null)return f;}throw new MissingMemberException(target.GetType().FullName,name);
    }
    private static object Read(object target,string name)=>Member(target,name) switch{PropertyInfo p=>p.GetValue(target),FieldInfo f=>f.GetValue(target),_=>null};
    private static object ConvertValue(JsonNode value,Type type)
    {
        var method=typeof(Sandbox.Json).GetMethods(BindingFlags.Public|BindingFlags.Static).First(m=>m.Name=="Deserialize"&&m.IsGenericMethodDefinition&&m.GetParameters().Length==1&&m.GetParameters()[0].ParameterType==typeof(string));
        return method.MakeGenericMethod(type).Invoke(null,new object[]{value?.ToJsonString()??"null"});
    }
    internal static JsonNode Tree(JsonObject a,bool runtime=true)
    {
        var start=Target(a.S("root"),runtime) as GameObject??throw new ArgumentException("Tree root must be a game object.");
        JsonNode Walk(GameObject g,int depth){var children=new JsonArray();if(depth<a.I("depth",6))foreach(var c in g.Children)children.Add(Walk(c,depth+1));var comps=new JsonArray();foreach(var c in g.Components.GetAll()){var item=new JsonObject{["id"]=c.Id.ToString(),["type"]=c.GetType().FullName,["enabled"]=c.Enabled};if(a.B("include_properties"))item["properties"]=c.Serialize();comps.Add(item);}return new JsonObject{["name"]=g.Name,["id"]=g.Id.ToString(),["enabled"]=g.Enabled,["position"]=g.WorldPosition.ToString(),["components"]=comps,["children"]=children};}
        return Walk(start,0);
    }
    internal static object? Get(object target,string name){object? current=target;foreach(var part in name.Split(new[]{'.',':'},StringSplitOptions.RemoveEmptyEntries))current=Read(current??throw new InvalidOperationException("Null property in path"),part);return current;}
    private static void Write(object target,MemberInfo member,object? value){if(member is PropertyInfo p)p.SetValue(target,value);else if(member is FieldInfo f)f.SetValue(target,value);}
    internal static void Set(object target,string name,JsonNode? value){
        var names=name.Split(new[]{'.',':'},StringSplitOptions.RemoveEmptyEntries);if(names.Length==0)throw new ArgumentException("Property path is empty");
        var parents=new List<object>{target};for(var i=0;i<names.Length-1;i++)parents.Add(Read(parents[^1],names[i])??throw new InvalidOperationException("Null property in path"));
        var leaf=Member(parents[^1],names[^1]);var type=leaf is PropertyInfo p?p.PropertyType:((FieldInfo)leaf).FieldType;
        Write(parents[^1],leaf,ConvertValue(value!,type));
        for(var i=parents.Count-1;i>0&&parents[i].GetType().IsValueType;i--)Write(parents[i-1],Member(parents[i-1],names[i-1]),parents[i]);
    }
    internal static object? Invoke(object target,string name,JsonArray args){var candidates=target.GetType().GetMethods(Flags).Where(m=>m.Name==name&&!m.IsGenericMethod&&m.GetParameters().Length==args.Count).ToArray();if(candidates.Length!=1)throw new ArgumentException("Method missing or overloaded; use eval_cs with an explicit signature.");var method=candidates[0];return method.Invoke(target,method.GetParameters().Select((p,i)=>ConvertValue(args[i]!,p.ParameterType)).ToArray());}
}
