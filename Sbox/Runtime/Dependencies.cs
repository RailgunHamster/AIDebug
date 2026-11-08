using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;

namespace AIDebug.Sbox;

internal static class Dependencies
{
    // Keep this delegate in the default-context debugger DLL, rather than pinning an
    // editor hotload assembly or a collectible MCP bootstrap script through Resolving.
    [ModuleInitializer]
    internal static void Initialize()
    {
        var directory=Path.GetDirectoryName(typeof(Dependencies).Assembly.Location)!;
        AssemblyLoadContext.Default.Resolving+=(_,name)=>{
            var path=Path.Combine(directory,name.Name+".dll");
            return File.Exists(path)?AssemblyLoadContext.Default.LoadFromAssemblyPath(path):null;
        };
    }
}
