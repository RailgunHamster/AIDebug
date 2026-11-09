using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Sandbox;

namespace AIDebug.Sbox.EditorIntegration;

/// <summary>Builds trusted debugger C# with the editor's Roslyn and actual engine references.
/// No SDK, IDE, shell, NuGet restore or project source installation runs on the user's machine.</summary>
internal static class DebugRuntimeCompiler
{
    internal static string Prepare(string library, string project)
    {
        var source=Path.Combine(library,"Runtime");
        var dependencies=Path.Combine(library,"Editor","AIDebug","Managed");
        var manifest=Path.Combine(library,"Editor","AIDebug","RuntimeFiles.json");
        var files=JsonNode.Parse(File.ReadAllText(manifest))["files"].AsObject();
        var package=Path.Combine(library,"Editor","AIDebug");
        foreach(var file in files)
        {
            var full=Path.GetFullPath(Path.Combine(package,file.Key));
            if(!full.StartsWith(package+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase)||!File.Exists(full)||
                !Hash(full).Equals(file.Value.GetValue<string>(),StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("AI Debug runtime dependency is missing or modified. Finish the Verso update: "+file.Key);
        }
        var sources=Directory.GetFiles(source,"*.cs").OrderBy(p=>p,StringComparer.Ordinal).ToArray();
        if(sources.Length==0)throw new InvalidOperationException("AI Debug C# runtime sources are missing.");
        var paths=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        void Add(string path)
        {
            if(!File.Exists(path)||!path.EndsWith(".dll",StringComparison.OrdinalIgnoreCase))return;
            try { AssemblyName.GetAssemblyName(path); }
            catch(BadImageFormatException) { return; }
            paths.TryAdd(Path.GetFileNameWithoutExtension(path),path);
        }
        // The editor already carries the framework and Roslyn. Reuse them before private dependencies.
        foreach(var path in ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")??"").Split(Path.PathSeparator))Add(path);
        var engine=Path.GetDirectoryName(typeof(Scene).Assembly.Location);
        foreach(var name in new[]{"Sandbox.Engine","Sandbox.System","Sandbox.Reflection","Microsoft.CodeAnalysis","Microsoft.CodeAnalysis.CSharp"})
            Add(Path.Combine(engine,name+".dll"));
        foreach(var path in Directory.GetFiles(dependencies,"*.dll"))Add(path);
        var identity=new StringBuilder();
        identity.Append(Hash(Path.Combine(package,"DebugRuntimeCompiler.cs")));
        foreach(var path in sources)identity.Append(Path.GetFileName(path)).Append(':').Append(Hash(path)).Append('\n');
        identity.Append(File.ReadAllText(manifest));
        foreach(var path in paths.Values.OrderBy(p=>p,StringComparer.Ordinal))identity.Append(Path.GetFileName(path)).Append(':').Append(Hash(path)).Append('\n');
        var hash=Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity.ToString())));
        var cache=Path.Combine(project,".ai_debug","runtime-cache",hash);
        var output=Path.Combine(cache,"AIDebug.Sbox.dll");
        var digest=output+".sha256";
        if(File.Exists(output)&&File.Exists(digest)&&Hash(output)==File.ReadAllText(digest))
        {
            File.WriteAllText(Path.Combine(cache,"dependencies.path"),dependencies);
            return output;
        }
        var parse=CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Preview);
        var trees=sources.Select(p=>CSharpSyntaxTree.ParseText(File.ReadAllText(p),parse,p)).ToList();
        trees.Add(CSharpSyntaxTree.ParseText("global using System; global using System.IO; global using System.Linq; global using System.Collections.Generic; global using System.Threading; global using System.Threading.Tasks;",parse));
        var references=paths.Values.Select(p=>MetadataReference.CreateFromFile(p));
        var options=new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,optimizationLevel:OptimizationLevel.Release,
            allowUnsafe:true,nullableContextOptions:NullableContextOptions.Enable,deterministic:true);
        var compilation=CSharpCompilation.Create("AIDebug.Sbox",trees,references,options);
        using var bytes=new MemoryStream();
        var result=compilation.Emit(bytes);
        if(!result.Success)throw new InvalidOperationException("AI Debug runtime compilation failed:\n"+
            string.Join("\n",result.Diagnostics.Where(d=>d.Severity==DiagnosticSeverity.Error)));
        Directory.CreateDirectory(cache);
        File.WriteAllText(Path.Combine(cache,"dependencies.path"),dependencies);
        var temporary=output+"."+Guid.NewGuid().ToString("N")+".tmp";
        File.WriteAllBytes(temporary,bytes.ToArray());
        File.Move(temporary,output,true);
        File.WriteAllText(digest,Hash(output));
        return output;
    }
    private static string Hash(string path)=>Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
}
