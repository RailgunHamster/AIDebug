#nullable enable
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

namespace Godot.AIDebug;

public static class Bootstrap
{
    private static Func<string, string, string>? _defaultAlcDispatcher;
    private static string _aiDebugDir = "";
    private static bool _resolverHooked;

    [UnmanagedCallersOnly]
    public static unsafe int ManagedEntry(
        char* dllPathPtr,
        char* cmdPtr,
        char* argsJsonPtr,
        delegate* unmanaged[Stdcall]<char*, int, void*, void> resultCallback,
        void* userData)
    {
        try
        {
            string dllPath = new string(dllPathPtr);
            string cmd = new string(cmdPtr);
            string argsJson = new string(argsJsonPtr);

            if (_defaultAlcDispatcher == null)
            {
                InitializeDefaultAlc(dllPath);
            }

            string response = _defaultAlcDispatcher!(cmd, argsJson);
            fixed (char* p = response)
            {
                resultCallback(p, response.Length, userData);
            }
            return 1;
        }
        catch (Exception ex)
        {
            string err = "{\"ok\":false,\"error\":" + QuoteJson(ex.ToString()) + "}";
            fixed (char* p = err)
            {
                resultCallback(p, err.Length, userData);
            }
            return 0;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void InitializeDefaultAlc(string dllPath)
    {
        _aiDebugDir = Path.GetDirectoryName(dllPath) ?? "";

        if (!_resolverHooked)
        {
            _resolverHooked = true;
            AssemblyLoadContext.Default.Resolving += OnResolving;
            var currentAlc = AssemblyLoadContext.GetLoadContext(typeof(Bootstrap).Assembly);
            if (currentAlc != null && currentAlc != AssemblyLoadContext.Default)
            {
                currentAlc.Resolving += OnResolving;
            }
            AppDomain.CurrentDomain.AssemblyResolve += OnAssemblyResolve;
        }

        Assembly currentAsm = typeof(Bootstrap).Assembly;
        Assembly targetAsm;
        if (AssemblyLoadContext.GetLoadContext(currentAsm) == AssemblyLoadContext.Default)
        {
            targetAsm = currentAsm;
        }
        else
        {
            targetAsm = AssemblyLoadContext.Default.Assemblies
                .FirstOrDefault(a => a.GetName().Name == "Godot.AIDebug")
                ?? AssemblyLoadContext.Default.LoadFromAssemblyPath(dllPath);
        }

        Type agentType = targetAsm.GetType("Godot.AIDebug.Agent", throwOnError: true)!;
        MethodInfo startMethod = agentType.GetMethod("Start", BindingFlags.Public | BindingFlags.Static)
            ?? throw new MissingMethodException("Godot.AIDebug.Agent", "Start");
        startMethod.Invoke(null, new object?[] { _aiDebugDir });

        MethodInfo dispatchMethod = agentType.GetMethod("Dispatch", BindingFlags.Public | BindingFlags.Static)
            ?? throw new MissingMethodException("Godot.AIDebug.Agent", "Dispatch");
        _defaultAlcDispatcher = (Func<string, string, string>)Delegate.CreateDelegate(
            typeof(Func<string, string, string>), dispatchMethod);
    }

    private static Assembly? OnAssemblyResolve(object? sender, ResolveEventArgs args)
    {
        var name = new AssemblyName(args.Name);
        return ResolveByName(AssemblyLoadContext.Default, name);
    }

    private static Assembly? OnResolving(AssemblyLoadContext context, AssemblyName name)
    {
        return ResolveByName(context, name);
    }

    private static Assembly? ResolveByName(AssemblyLoadContext context, AssemblyName name)
    {
        string? simpleName = name.Name;
        if (string.IsNullOrEmpty(simpleName))
            return null;

        // Ignore satellite resource assemblies
        if (simpleName.EndsWith(".resources", StringComparison.OrdinalIgnoreCase))
            return null;

        // 1. Check already loaded assemblies across the AppDomain
        var loaded = AppDomain.CurrentDomain.GetAssemblies();
        for (int i = loaded.Length - 1; i >= 0; i--)
        {
            var asm = loaded[i];
            if (asm.IsDynamic)
                continue;
            if (string.Equals(asm.GetName().Name, simpleName, StringComparison.OrdinalIgnoreCase))
            {
                return asm;
            }
        }

        // 2. Check _aiDebugDir for managed dependencies (0Harmony, MonoMod.*, Microsoft.CodeAnalysis.*, etc.)
        if (!string.IsNullOrEmpty(_aiDebugDir))
        {
            string candidate = Path.Combine(_aiDebugDir, simpleName + ".dll");
            if (File.Exists(candidate))
            {
                return AssemblyLoadContext.Default.LoadFromAssemblyPath(candidate);
            }
        }

        return null;
    }

    internal static string QuoteJson(string s)
    {
        var sb = new StringBuilder(s.Length + 8);
        sb.Append('"');
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20)
                        sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else
                        sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
        return sb.ToString();
    }
}
