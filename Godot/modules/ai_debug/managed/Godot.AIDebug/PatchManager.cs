#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Text;
using System.Threading;
using HarmonyLib;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace Godot.AIDebug;

public sealed class PatchEntry
{
    public string Id = "";
    public string Target = "";
    public string Kind = "";
    public string AppliedAt = "";
    public MethodBase TargetMethod = null!;
    public MethodInfo PatchMethod = null!;
    public FieldInfo? CallCountField;
    public int IlSize;
    public bool JitNoInline;
    public string InlineWarning = "";

    public long GetCallCount()
    {
        try
        {
            if (CallCountField != null && CallCountField.GetValue(null) is long c)
                return c;
        }
        catch { }
        return 0;
    }

    public Dictionary<string, object?> ToDictionary()
    {
        var d = new Dictionary<string, object?>
        {
            ["id"] = Id,
            ["target"] = Target,
            ["kind"] = Kind,
            ["applied_at"] = AppliedAt,
            ["call_count"] = GetCallCount(),
            ["il_size"] = IlSize,
            ["jit_no_inline"] = JitNoInline,
        };
        if (!string.IsNullOrEmpty(InlineWarning))
            d["warning"] = InlineWarning;
        return d;
    }
}

public static class PatchManager
{
    private const string HarmonyId = "org.godotengine.aidebug";
    private static Harmony? _harmony;
    private static readonly Dictionary<string, PatchEntry> _patches = new();
    private static int _nextPatchId = 1;

    private static Harmony GetHarmony() => _harmony ??= new Harmony(HarmonyId);

    public static Dictionary<string, object?> ApplyPatch(string targetSpec, string kind, string code, bool jitNoInline)
    {
        kind = (kind ?? "prefix").Trim().ToLowerInvariant();
        if (kind is not ("prefix" or "postfix" or "finalizer" or "transpiler" or "replace"))
        {
            return new Dictionary<string, object?>
            {
                ["ok"] = false,
                ["error"] = $"Unsupported patch kind '{kind}'. Expected one of: prefix, postfix, finalizer, transpiler, replace.",
            };
        }

        if (string.IsNullOrWhiteSpace(targetSpec))
        {
            return new Dictionary<string, object?>
            {
                ["ok"] = false,
                ["error"] = "Target method specification cannot be empty. Format: 'Namespace.Type:Method(ParamType1, ParamType2)'.",
            };
        }

        if (string.IsNullOrWhiteSpace(code))
        {
            return new Dictionary<string, object?>
            {
                ["ok"] = false,
                ["error"] = "Patch C# code cannot be empty.",
            };
        }

        if (!TryResolveTargetMethod(targetSpec, out MethodBase? targetMethod, out string resolveError) || targetMethod == null)
        {
            return new Dictionary<string, object?>
            {
                ["ok"] = false,
                ["error"] = resolveError,
            };
        }

        int ilSize = targetMethod.GetMethodBody()?.GetILAsByteArray()?.Length ?? 0;
        bool hasNoInlineAttr = (targetMethod.MethodImplementationFlags & MethodImplAttributes.NoInlining) != 0;
        string inlineWarning = "";
        if (!jitNoInline)
        {
            inlineWarning = string.Concat(
                "注意：当前进程未在启动前开启 DOTNET_JitNoInline=1（运行时热键激活或使用了 --ai-debug-inline），",
                ilSize > 0 && ilSize <= 64 && !hasNoInlineAttr
                    ? $"且目标方法 IL 较小（{ilSize} 字节，无 NoInlining 标记），极可能已被 JIT 内联到调用方，补丁对已内联的调用点可能不生效。如需对小方法稳定生效，请带 --ai-debug 启动进程。"
                    : "若目标方法已被 JIT 内联，Harmony 补丁可能对已内联调用点不生效。");
        }

        string patchId = $"p{_nextPatchId++}";
        if (!TryCompilePatchMethod(patchId, targetMethod, kind, code, out MethodInfo? patchMethod, out FieldInfo? callCountField, out string compileError) || patchMethod == null)
        {
            return new Dictionary<string, object?>
            {
                ["ok"] = false,
                ["error"] = compileError,
            };
        }

        try
        {
            var harmony = GetHarmony();
            HarmonyMethod hm = new HarmonyMethod(patchMethod);
            switch (kind)
            {
                case "prefix":
                case "replace":
                    harmony.Patch(targetMethod, prefix: hm);
                    break;
                case "postfix":
                    harmony.Patch(targetMethod, postfix: hm);
                    break;
                case "finalizer":
                    harmony.Patch(targetMethod, finalizer: hm);
                    break;
                case "transpiler":
                    harmony.Patch(targetMethod, transpiler: hm);
                    break;
            }
        }
        catch (Exception ex)
        {
            return new Dictionary<string, object?>
            {
                ["ok"] = false,
                ["error"] = $"Harmony.Patch failed for '{FormatMethodSignature(targetMethod)}': {ex.Message}\n{ex}",
            };
        }

        var entry = new PatchEntry
        {
            Id = patchId,
            Target = FormatMethodSignature(targetMethod),
            Kind = kind,
            AppliedAt = DateTime.Now.ToString("yyyy-MM-ddTHH:mm:ss"),
            TargetMethod = targetMethod,
            PatchMethod = patchMethod,
            CallCountField = callCountField,
            IlSize = ilSize,
            JitNoInline = jitNoInline,
            InlineWarning = inlineWarning,
        };

        lock (_patches)
        {
            _patches[patchId] = entry;
        }

        var res = entry.ToDictionary();
        res["ok"] = true;
        return res;
    }

    public static Dictionary<string, object?> Unpatch(string? id)
    {
        string targetId = (id ?? "").Trim();
        var removed = new List<string>();
        var errors = new List<string>();

        lock (_patches)
        {
            if (string.IsNullOrEmpty(targetId) || targetId == "*" || targetId.Equals("all", StringComparison.OrdinalIgnoreCase))
            {
                foreach (var kv in _patches.ToArray())
                {
                    try
                    {
                        _harmony?.Unpatch(kv.Value.TargetMethod, kv.Value.PatchMethod);
                    }
                    catch (Exception ex)
                    {
                        errors.Add(kv.Key + ": " + ex.Message);
                        continue;
                    }
                    _patches.Remove(kv.Key);
                    removed.Add(kv.Key);
                }
            }
            else if (_patches.TryGetValue(targetId, out var entry))
            {
                try
                {
                    _harmony?.Unpatch(entry.TargetMethod, entry.PatchMethod);
                }
                catch (Exception ex)
                {
                    return new Dictionary<string, object?>
                    {
                        ["ok"] = false,
                        ["error"] = $"Failed to unpatch '{targetId}': {ex.Message}",
                    };
                }
                _patches.Remove(targetId);
                removed.Add(targetId);
            }
            else
            {
                return new Dictionary<string, object?>
                {
                    ["ok"] = false,
                    ["error"] = $"Patch '{targetId}' not found.",
                };
            }
        }

        return new Dictionary<string, object?>
        {
            ["ok"] = errors.Count == 0,
            ["errors"] = errors,
            ["unpatched"] = removed,
            ["remaining_count"] = Count,
        };
    }

    public static void UnpatchAll()
    {
        Unpatch("all");
    }

    public static int Count
    {
        get
        {
            lock (_patches)
            {
                return _patches.Count;
            }
        }
    }

    public static List<object?> ListPatches()
    {
        lock (_patches)
        {
            return _patches.Values.Select(p => (object?)p.ToDictionary()).ToList();
        }
    }

    public static bool TryResolveTargetMethod(string spec, out MethodBase? method, out string error)
    {
        method = null;
        error = "";

        string trimmed = spec.Trim();
        string? paramListStr = null;

        int parenOpen = trimmed.IndexOf('(');
        if (parenOpen >= 0)
        {
            if (!trimmed.EndsWith(")"))
            {
                error = $"Invalid target method specification '{spec}': missing closing ')'.";
                return false;
            }
            paramListStr = trimmed.Substring(parenOpen + 1, trimmed.Length - parenOpen - 2).Trim();
            trimmed = trimmed.Substring(0, parenOpen).Trim();
        }

        string typeName;
        string methodName;
        int colonIdx = trimmed.LastIndexOf(':');
        if (colonIdx > 0)
        {
            typeName = trimmed.Substring(0, colonIdx).Trim();
            methodName = trimmed.Substring(colonIdx + 1).Trim();
        }
        else
        {
            int dotIdx = trimmed.LastIndexOf('.');
            if (dotIdx <= 0)
            {
                error = $"Invalid target specification '{spec}'. Expected 'Namespace.Type:Method' or 'Namespace.Type:Method(ParamTypes)'.";
                return false;
            }
            typeName = trimmed.Substring(0, dotIdx).Trim();
            methodName = trimmed.Substring(dotIdx + 1).Trim();
        }

        Type? type = ResolveType(typeName);
        if (type == null)
        {
            error = $"Type '{typeName}' not found in any loaded assembly.";
            return false;
        }

        const BindingFlags AllFlags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;
        List<MethodBase> candidates = new();
        for (Type? cur = type; cur != null && cur != typeof(object); cur = cur.BaseType)
        {
            if (methodName == ".ctor" || methodName == cur.Name)
            {
                candidates.AddRange(cur.GetConstructors(AllFlags));
            }
            candidates.AddRange(cur.GetMethods(AllFlags).Where(m => m.Name == methodName));
            if (candidates.Count > 0)
                break;
        }

        if (candidates.Count == 0)
        {
            error = $"Method '{methodName}' not found on type '{type.FullName}'.";
            return false;
        }

        if (paramListStr == null)
        {
            if (candidates.Count > 1)
            {
                var sigs = candidates.Select(FormatMethodSignature);
                error = $"Method '{type.FullName}:{methodName}' is overloaded. Specify parameter types explicitly, e.g.:\n  " +
                        string.Join("\n  ", sigs);
                return false;
            }
            method = candidates[0];
            return true;
        }

        string[] wantedParams = string.IsNullOrWhiteSpace(paramListStr)
            ? Array.Empty<string>()
            : SplitTopLevelCommas(paramListStr);

        foreach (var cand in candidates)
        {
            var ps = cand.GetParameters();
            if (ps.Length != wantedParams.Length)
                continue;

            bool match = true;
            for (int i = 0; i < ps.Length; i++)
            {
                if (!ParameterTypeMatches(ps[i].ParameterType, wantedParams[i]))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                method = cand;
                return true;
            }
        }

        var avail = candidates.Select(FormatMethodSignature);
        error = $"No overload of '{type.FullName}:{methodName}' matched parameter types '({paramListStr})'. Available overloads:\n  " +
                string.Join("\n  ", avail);
        return false;
    }

    private static string[] SplitTopLevelCommas(string input)
    {
        var list = new List<string>();
        int depth = 0;
        int start = 0;
        for (int i = 0; i < input.Length; i++)
        {
            char c = input[i];
            if (c is '<' or '[' or '(') depth++;
            else if (c is '>' or ']' or ')') depth--;
            else if (c == ',' && depth == 0)
            {
                list.Add(input.Substring(start, i - start).Trim());
                start = i + 1;
            }
        }
        list.Add(input.Substring(start).Trim());
        return list.ToArray();
    }

    private static bool ParameterTypeMatches(Type paramType, string wanted)
    {
        wanted = wanted.Trim();
        if (wanted.StartsWith("ref ")) wanted = wanted.Substring(4).Trim();
        else if (wanted.StartsWith("out ")) wanted = wanted.Substring(4).Trim();
        else if (wanted.StartsWith("in ")) wanted = wanted.Substring(3).Trim();

        Type cleanType = paramType.IsByRef ? paramType.GetElementType()! : paramType;
        Type? resolvedWanted = ResolveType(wanted);
        if (resolvedWanted != null && resolvedWanted == cleanType)
            return true;

        if (string.Equals(cleanType.FullName, wanted, StringComparison.Ordinal) ||
            string.Equals(cleanType.Name, wanted, StringComparison.Ordinal) ||
            string.Equals(GetFriendlyTypeName(cleanType), wanted, StringComparison.Ordinal))
        {
            return true;
        }

        return false;
    }

    public static Type? ResolveType(string typeName)
    {
        typeName = typeName.Trim();
        switch (typeName)
        {
            case "bool": case "System.Boolean": return typeof(bool);
            case "byte": case "System.Byte": return typeof(byte);
            case "sbyte": case "System.SByte": return typeof(sbyte);
            case "short": case "Int16": case "System.Int16": return typeof(short);
            case "ushort": case "UInt16": case "System.UInt16": return typeof(ushort);
            case "int": case "Int32": case "System.Int32": return typeof(int);
            case "uint": case "UInt32": case "System.UInt32": return typeof(uint);
            case "long": case "Int64": case "System.Int64": return typeof(long);
            case "ulong": case "UInt64": case "System.UInt64": return typeof(ulong);
            case "float": case "Single": case "System.Single": return typeof(float);
            case "double": case "Double": case "System.Double": return typeof(double);
            case "decimal": case "Decimal": case "System.Decimal": return typeof(decimal);
            case "char": case "Char": case "System.Char": return typeof(char);
            case "string": case "String": case "System.String": return typeof(string);
            case "object": case "Object": case "System.Object": return typeof(object);
            case "void": case "Void": case "System.Void": return typeof(void);
        }

        // Check project assemblies first (prefer latest/Default ALC over any unused isolated ALC)
        foreach (var asm in Agent.GetProjectAssemblies())
        {
            try
            {
                var match = asm.GetType(typeName, throwOnError: false, ignoreCase: false)
                            ?? asm.GetTypes().FirstOrDefault(x => x.FullName == typeName || x.Name == typeName);
                if (match != null)
                    return match;
            }
            catch { }
        }

        var assemblies = AppDomain.CurrentDomain.GetAssemblies();
        for (int i = assemblies.Length - 1; i >= 0; i--)
        {
            var asm = assemblies[i];
            if (asm.IsDynamic)
                continue;
            try
            {
                var t = asm.GetType(typeName, throwOnError: false, ignoreCase: false);
                if (t != null)
                    return t;
            }
            catch { }
        }

        return AccessTools.TypeByName(typeName);
    }

    public static string FormatMethodSignature(MethodBase m)
    {
        var ps = string.Join(", ", m.GetParameters().Select(p => GetFriendlyTypeName(p.ParameterType)));
        return $"{m.DeclaringType?.FullName}:{m.Name}({ps})";
    }

    public static string GetFriendlyTypeName(Type t)
    {
        if (t.IsByRef)
            return "ref " + GetFriendlyTypeName(t.GetElementType()!);
        if (t == typeof(void)) return "void";
        if (t == typeof(bool)) return "bool";
        if (t == typeof(int)) return "int";
        if (t == typeof(long)) return "long";
        if (t == typeof(float)) return "float";
        if (t == typeof(double)) return "double";
        if (t == typeof(string)) return "string";
        if (t == typeof(object)) return "object";
        if (t.IsGenericType)
        {
            string baseName = t.FullName ?? t.Name;
            int tick = baseName.IndexOf('`');
            if (tick > 0) baseName = baseName.Substring(0, tick);
            baseName = baseName.Replace('+', '.');
            var args = string.Join(", ", t.GetGenericArguments().Select(GetFriendlyTypeName));
            return $"{baseName}<{args}>";
        }
        return (t.FullName ?? t.Name).Replace('+', '.');
    }

    private static bool TryCompilePatchMethod(
        string patchId,
        MethodBase targetMethod,
        string kind,
        string userCode,
        out MethodInfo? patchMethod,
        out FieldInfo? callCountField,
        out string error)
    {
        patchMethod = null;
        callCountField = null;
        error = "";

        string className = $"AIDebugPatch_{patchId}";
        string fullSource = BuildCompilationUnitSource(className, targetMethod, kind, userCode, out string targetMethodName);

        var syntaxTree = CSharpSyntaxTree.ParseText(fullSource);
        var references = Agent.GetMetadataReferences();

        var compilation = CSharpCompilation.Create(
            assemblyName: $"AIDebugDynamicPatch_{patchId}_{Guid.NewGuid():N}",
            syntaxTrees: new[] { syntaxTree },
            references: references,
            options: new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                optimizationLevel: OptimizationLevel.Debug,
                allowUnsafe: true,
                metadataImportOptions: MetadataImportOptions.All));

        // Enable top-level internal/private member access in compiled patches via IgnoresAccessChecksTo if needed
        using var peStream = new MemoryStream();
        var emitResult = compilation.Emit(peStream);
        if (!emitResult.Success)
        {
            var diags = emitResult.Diagnostics
                .Where(d => d.Severity == DiagnosticSeverity.Error)
                .Select(d => d.ToString());
            error = $"Failed to compile patch C# code:\n{string.Join("\n", diags)}\n--- Generated source ---\n{fullSource}";
            return false;
        }

        peStream.Seek(0, SeekOrigin.Begin);
        var patchAsm = AssemblyLoadContext.Default.LoadFromStream(peStream);
        var patchType = patchAsm.GetType(className);
        if (patchType == null)
        {
            error = $"Compiled patch assembly did not contain expected class '{className}'.";
            return false;
        }

        callCountField = patchType.GetField("__CallCount", BindingFlags.Public | BindingFlags.Static);
        patchMethod = patchType.GetMethod(targetMethodName, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
            ?? patchType.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
                .FirstOrDefault(m => !m.IsSpecialName);

        if (patchMethod == null)
        {
            error = $"No static patch method found in compiled class '{className}'.";
            return false;
        }

        return true;
    }

    private static string BuildCompilationUnitSource(
        string className,
        MethodBase targetMethod,
        string kind,
        string userCode,
        out string methodName)
    {
        var sb = new StringBuilder();
        sb.AppendLine("using System;");
        sb.AppendLine("using System.Collections.Generic;");
        sb.AppendLine("using System.Linq;");
        sb.AppendLine("using System.Reflection;");
        sb.AppendLine("using System.Runtime.CompilerServices;");
        sb.AppendLine("using System.Threading;");
        sb.AppendLine("using Godot;");
        sb.AppendLine("using HarmonyLib;");
        sb.AppendLine("using static Godot.AIDebug.PatchHelpers;");

        if (targetMethod.DeclaringType?.Namespace is { Length: > 0 } ns)
        {
            sb.AppendLine($"using {ns};");
        }

        foreach (var projAsm in Agent.GetProjectAssemblies())
        {
            string? asmName = projAsm.GetName().Name;
            if (!string.IsNullOrEmpty(asmName))
            {
                sb.AppendLine($"[assembly: IgnoresAccessChecksTo(\"{asmName.Replace("\"", "\\\"")}\")]");
            }
        }
        sb.AppendLine("namespace System.Runtime.CompilerServices { [AttributeUsage(AttributeTargets.Assembly, AllowMultiple = true)] internal sealed class IgnoresAccessChecksToAttribute : Attribute { public IgnoresAccessChecksToAttribute(string assemblyName) {} } }");

        string trimmed = userCode.Trim();
        bool looksLikeFullMethod =
            trimmed.Contains("static ") &&
            (trimmed.Contains("Prefix(") || trimmed.Contains("Postfix(") ||
             trimmed.Contains("Finalizer(") || trimmed.Contains("Transpiler(") ||
             trimmed.Contains("Replace(") || trimmed.Contains("Patch("));

        sb.AppendLine($"public static class {className}");
        sb.AppendLine("{");
        sb.AppendLine("    public static long __CallCount;");

        if (looksLikeFullMethod)
        {
            // Inject Interlocked.Increment(ref __CallCount); at the start of the first method body '{'
            int braceIdx = trimmed.IndexOf('{');
            if (braceIdx >= 0)
            {
                trimmed = trimmed.Substring(0, braceIdx + 1) +
                          "\n        System.Threading.Interlocked.Increment(ref __CallCount);\n" +
                          trimmed.Substring(braceIdx + 1);
            }
            sb.AppendLine(trimmed);
            methodName = kind switch
            {
                "postfix" => "Postfix",
                "finalizer" => "Finalizer",
                "transpiler" => "Transpiler",
                _ => "Prefix",
            };
        }
        else
        {
            methodName = "PatchMethod";
            var paramDecls = new List<string>();
            if (!targetMethod.IsStatic && targetMethod.DeclaringType != null)
            {
                string instType = targetMethod.DeclaringType.IsVisible
                    ? GetFriendlyTypeName(targetMethod.DeclaringType)
                    : "object";
                paramDecls.Add($"{instType} __instance");
            }

            Type returnType = (targetMethod as MethodInfo)?.ReturnType ?? typeof(void);
            bool hasReturn = returnType != typeof(void);
            string retTypeName = hasReturn && returnType.IsVisible
                ? GetFriendlyTypeName(returnType)
                : "object";
            if (hasReturn)
            {
                paramDecls.Add($"ref {retTypeName} __result");
            }

            if (kind == "finalizer")
            {
                paramDecls.Add("Exception __exception");
            }

            foreach (var p in targetMethod.GetParameters())
            {
                Type pType = p.ParameterType.IsByRef ? p.ParameterType.GetElementType()! : p.ParameterType;
                string pTypeName = pType.IsVisible ? GetFriendlyTypeName(pType) : "object";
                paramDecls.Add($"ref {pTypeName} {p.Name}");
            }

            string paramList = string.Join(", ", paramDecls);

            if (kind == "replace")
            {
                // Replace mode: prefix returns false and sets __result.
                // Allow user code to either set `__result = ...;` OR write `return <value>;` directly!
                if (hasReturn && !trimmed.Contains("__result"))
                {
                    sb.AppendLine($"    private static {retTypeName} __UserReplace({paramList})");
                    sb.AppendLine("    {");
                    sb.AppendLine("        " + trimmed);
                    if (!trimmed.EndsWith(";"))
                        sb.AppendLine(";");
                    sb.AppendLine("    }");
                    sb.AppendLine($"    public static bool {methodName}({paramList})");
                    sb.AppendLine("    {");
                    sb.AppendLine("        System.Threading.Interlocked.Increment(ref __CallCount);");
                    var callArgs = new List<string>();
                    if (!targetMethod.IsStatic && targetMethod.DeclaringType != null)
                        callArgs.Add("__instance");
                    callArgs.Add("ref __result");
                    foreach (var p in targetMethod.GetParameters())
                        callArgs.Add($"ref {p.Name}");
                    sb.AppendLine($"        __result = __UserReplace({string.Join(", ", callArgs)});");
                    sb.AppendLine("        return false;");
                    sb.AppendLine("    }");
                }
                else
                {
                    sb.AppendLine($"    public static bool {methodName}({paramList})");
                    sb.AppendLine("    {");
                    sb.AppendLine("        System.Threading.Interlocked.Increment(ref __CallCount);");
                    sb.AppendLine("        " + trimmed);
                    sb.AppendLine("        return false;");
                    sb.AppendLine("    }");
                }
            }
            else if (kind == "prefix")
            {
                sb.AppendLine($"    public static bool {methodName}({paramList})");
                sb.AppendLine("    {");
                sb.AppendLine("        System.Threading.Interlocked.Increment(ref __CallCount);");
                sb.AppendLine("        " + trimmed);
                if (!trimmed.Contains("return "))
                {
                    sb.AppendLine("        return true;");
                }
                sb.AppendLine("    }");
            }
            else
            {
                sb.AppendLine($"    public static void {methodName}({paramList})");
                sb.AppendLine("    {");
                sb.AppendLine("        System.Threading.Interlocked.Increment(ref __CallCount);");
                sb.AppendLine("        " + trimmed);
                sb.AppendLine("    }");
            }
        }

        sb.AppendLine("}");
        return sb.ToString();
    }
}

public static class PatchHelpers
{
    private static readonly AIDebugGlobals _helperGlobals = new();

    public static Godot.SceneTree Tree => _helperGlobals.Tree;
    public static Godot.Window Root => _helperGlobals.Root;
    public static Godot.Node? Node(string path) => _helperGlobals.Node(path);
    public static T? Find<T>(string? name = null) where T : class => _helperGlobals.Find<T>(name);
    public static void Log(object? msg) => _helperGlobals.Log(msg);
    public static Traverse Traverse(object target) => _helperGlobals.Traverse(target);
    public static object? GetPrivate(object target, string member) => _helperGlobals.GetPrivate(target, member);
    public static void SetPrivate(object target, string member, object? value) => _helperGlobals.SetPrivate(target, member, value);
    public static object? CallPrivate(object target, string method, params object?[] args) => _helperGlobals.CallPrivate(target, method, args);
}
