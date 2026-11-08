#nullable enable
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Metadata;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Godot;
using Microsoft.CodeAnalysis;
using RoslynCSharpScript = Microsoft.CodeAnalysis.CSharp.Scripting.CSharpScript;
using Microsoft.CodeAnalysis.Scripting;

namespace Godot.AIDebug;

public static class Agent
{
    private static string _aiDebugDir = "";
    private static bool _started;
    private static ScriptState<object>? _scriptState;
    private static AIDebugGlobals? _globals;
    private static readonly object _evalLock = new();
    private sealed class Evaluation
    {
        public long Id;
        public long Generation;
        public AIDebugGlobals Globals = null!;
        public CancellationTokenSource Cancellation = null!;
        public Task<Dictionary<string, object?>>? Task;
    }
    private static readonly Dictionary<long, Evaluation> _evaluations = new();
    private static Evaluation? _runningEvaluation;
    private static long _nextEvaluationId;
    private static long _sessionGeneration;
    internal static int MainThreadId { get; private set; }

    private static readonly Dictionary<Assembly, MetadataReference> _metadataCache = new();

    private static readonly string[] DefaultImports =
    {
        "System",
        "System.Collections.Generic",
        "System.Linq",
        "System.Reflection",
        "System.Text",
        "Godot",
        "HarmonyLib",
    };

    public static void Start(string aiDebugDir)
    {
        _aiDebugDir = aiDebugDir ?? "";
        if (_started)
            return;
        _started = true;
        MainThreadId = System.Environment.CurrentManagedThreadId;
        // Force-load 0Harmony so HarmonyLib is immediately in AppDomain.CurrentDomain.GetAssemblies()
        _ = typeof(HarmonyLib.Harmony).Assembly;
    }

    public static string Dispatch(string cmd, string argsJson)
    {
        try
        {
            var args = ParseJsonDict(argsJson);
            Dictionary<string, object?> result = cmd switch
            {
                "start" => new Dictionary<string, object?> { ["ok"] = true, ["runtime"] = RuntimeInformation.FrameworkDescription },
                "info" => HandleInfo(),
                "eval_cs_start" => StartEvaluation(args),
                "eval_cs_poll" => PollEvaluation(args),
                "eval_cs_cancel" => CancelEvaluation(args),
                "patch_cs" => HandlePatchCs(args),
                "unpatch" => HandleUnpatch(args),
                "list_patches" => new Dictionary<string, object?> { ["ok"] = true, ["patches"] = PatchManager.ListPatches() },
                "reset" => HandleReset(),
                _ => new Dictionary<string, object?> { ["ok"] = false, ["error"] = $"Unknown managed command: {cmd}" },
            };
            return SerializeJson(result);
        }
        catch (Exception ex)
        {
            return SerializeJson(new Dictionary<string, object?>
            {
                ["ok"] = false,
                ["error"] = $"{ex.GetType().Name}: {ex.Message}",
                ["stack_trace"] = ex.ToString(),
            });
        }
    }

    private static Dictionary<string, object?> HandleInfo()
    {
        var projAsms = GetProjectAssemblies().Select(a => (object?)(a.GetName().Name ?? "")).ToList();
        return new Dictionary<string, object?>
        {
            ["ok"] = true,
            ["dotnet_runtime"] = RuntimeInformation.FrameworkDescription,
            ["alc"] = System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(typeof(Agent).Assembly)?.Name ?? "Default",
            ["project_assemblies"] = projAsms,
            ["patches"] = PatchManager.ListPatches(),
        };
    }

    private static Dictionary<string, object?> HandleReset()
    {
        Evaluation? running;
        Evaluation[] pending;
        lock (_evalLock)
        {
            ++_sessionGeneration;
            _scriptState = null;
            _globals = null;
            running = _runningEvaluation;
            pending = _evaluations.Values.ToArray();
            _evaluations.Clear();
        }
        // Never wait for user code while holding the session lock or the engine main thread.
        RequestCancellation(running);
        foreach (var job in pending)
            _ = job.Task?.ContinueWith(_ => job.Cancellation.Dispose(), TaskScheduler.Default);
        var patches = PatchManager.Unpatch("all");
        bool cleared = GetBool(patches, "ok");
        return new Dictionary<string, object?>
        {
            ["ok"] = cleared,
            ["patches_cleared"] = cleared,
            ["patch_cleanup"] = patches,
            ["script_state_reset"] = true,
            ["execution_may_continue"] = running?.Task?.IsCompleted == false,
        };
    }

    private static Dictionary<string, object?> HandlePatchCs(Dictionary<string, object?> args)
    {
        string target = GetStr(args, "target");
        string kind = GetStr(args, "kind", "prefix");
        string code = GetStr(args, "code");
        bool jitNoInline = System.Environment.GetEnvironmentVariable("DOTNET_JitNoInline") == "1" &&
                           GetBool(args, "jit_no_inline", true);
        return PatchManager.ApplyPatch(target, kind, code, jitNoInline);
    }

    private static Dictionary<string, object?> HandleUnpatch(Dictionary<string, object?> args)
    {
        string id = GetStr(args, "id", "all");
        return PatchManager.Unpatch(id);
    }

    private static Dictionary<string, object?> StartEvaluation(Dictionary<string, object?> args)
    {
        string code = GetStr(args, "code", GetStr(args, "expression"));
        if (string.IsNullOrWhiteSpace(code))
            return new() { ["ok"] = false, ["error"] = "Parameter 'code' cannot be empty." };
        string thread = GetStr(args, "thread", "main");
        if (thread is not ("main" or "worker"))
            return new() { ["ok"] = false, ["error"] = "thread must be 'main' or 'worker'." };
        int timeoutMs = (int)Math.Clamp(GetLong(args, "timeout_ms", 10000), 1, 300000);
        Evaluation job;
        ScriptState<object>? previous;
        lock (_evalLock)
        {
            if (_runningEvaluation != null)
                return new() { ["ok"] = false, ["busy"] = true, ["error"] = "A C# evaluation is still running. Cancellation is cooperative; reset cannot forcibly stop user code." };
            if (GetBool(args, "reset_session"))
            {
                ++_sessionGeneration;
                _scriptState = null;
                _globals = null;
            }
            _globals ??= new AIDebugGlobals();
            _globals.Logs.Clear();
            var cancellation = new CancellationTokenSource(timeoutMs);
            _globals.Cancellation = cancellation.Token;
            job = new Evaluation
            {
                Id = ++_nextEvaluationId,
                Generation = _sessionGeneration,
                Globals = _globals,
                Cancellation = cancellation,
            };
            previous = _scriptState;
            _runningEvaluation = job;
            _evaluations.Add(job.Id, job);
        }
        job.Task = thread == "worker"
            ? Task.Run(() => ExecuteEvaluation(job, previous, code))
            : ExecuteEvaluation(job, previous, code);
        return new() { ["ok"] = true, ["job_id"] = job.Id };
    }

    private static async Task<Dictionary<string, object?>> ExecuteEvaluation(Evaluation job, ScriptState<object>? previous, string code)
    {
        ScriptState<object>? state = null;
        try
        {
            var options = BuildScriptOptions();
            var token = job.Cancellation.Token;
            state = previous == null
                ? await RoslynCSharpScript.RunAsync<object>(code, options, job.Globals, typeof(AIDebugGlobals), token)
                : await previous.ContinueWithAsync<object>(code, options, token);
            var vars = new Dictionary<string, object?>();
            foreach (var variable in state.Variables)
                vars[variable.Name] = FormatValue(variable.Value, 0);
            List<object?> logs;
            lock (job.Globals.Logs)
                logs = job.Globals.Logs.Cast<object?>().ToList();
            return new()
            {
                ["ok"] = true,
                ["result"] = FormatValue(state.ReturnValue, 0),
                ["return_type"] = state.ReturnValue?.GetType().FullName ?? "null",
                ["variables"] = vars,
                ["logs"] = logs,
                ["cancellation_requested"] = token.IsCancellationRequested,
            };
        }
        catch (CompilationErrorException ex)
        {
            return new() { ["ok"] = false, ["error"] = "C# compilation error:\n" + string.Join("\n", ex.Diagnostics) };
        }
        catch (OperationCanceledException)
        {
            return new() { ["ok"] = false, ["cancelled"] = true, ["execution_may_continue"] = false, ["error"] = "C# evaluation observed cancellation." };
        }
        catch (Exception ex)
        {
            return new() { ["ok"] = false, ["error"] = ex.Message, ["stack_trace"] = ex.ToString() };
        }
        finally
        {
            lock (_evalLock)
            {
                if (state != null && job.Generation == _sessionGeneration && !job.Cancellation.IsCancellationRequested)
                    _scriptState = state;
                if (ReferenceEquals(_runningEvaluation, job))
                    _runningEvaluation = null;
            }
            // Keep the source alive until poll/cancel has observed completion.
        }
    }

    private static Dictionary<string, object?> PollEvaluation(Dictionary<string, object?> args)
    {
        Evaluation? job;
        lock (_evalLock)
        {
            if (!_evaluations.TryGetValue(GetLong(args, "job_id"), out job))
                return new() { ["ok"] = false, ["error"] = "Evaluation was cancelled or reset." };
            if (job.Task?.IsCompleted != true)
                return new() { ["ok"] = true, ["done"] = false };
            _evaluations.Remove(job.Id);
        }
        var result = job.Task.GetAwaiter().GetResult();
        job.Cancellation.Dispose();
        return new() { ["ok"] = true, ["done"] = true, ["result"] = result };
    }

    private static Dictionary<string, object?> CancelEvaluation(Dictionary<string, object?> args)
    {
        Evaluation? job;
        lock (_evalLock)
        {
            if (!_evaluations.Remove(GetLong(args, "job_id"), out job))
                return new() { ["ok"] = true, ["execution_may_continue"] = false };
        }
        RequestCancellation(job);
        bool stillRunning = job.Task?.IsCompleted != true;
        // Dispose only after user code finishes; never join it during reset/shutdown.
        _ = job.Task?.ContinueWith(_ => job.Cancellation.Dispose(), TaskScheduler.Default);
        return new() { ["ok"] = true, ["execution_may_continue"] = stillRunning };
    }

    private static void RequestCancellation(Evaluation? job)
    {
        if (job == null)
            return;
        try
        {
            // User cancellation callbacks may block or throw. Do not run them on the engine thread.
            _ = job.Cancellation.CancelAsync().ContinueWith(t => { _ = t.Exception; }, TaskScheduler.Default);
        }
        catch (ObjectDisposedException)
        {
            // The task completed and was polled between reset's lookup and cancellation request.
        }
    }

    private static ScriptOptions BuildScriptOptions()
    {
        var refs = GetMetadataReferences();
        var imports = new HashSet<string>(DefaultImports);
        foreach (var asm in GetProjectAssemblies())
        {
            try
            {
                foreach (var t in asm.GetTypes())
                {
                    if (!string.IsNullOrEmpty(t.Namespace))
                        imports.Add(t.Namespace);
                }
            }
            catch { }
        }

        return ScriptOptions.Default
            .WithReferences(refs)
            .WithImports(imports)
            .WithAllowUnsafe(true);
    }

    public static unsafe List<MetadataReference> GetMetadataReferences()
    {
        var list = new List<MetadataReference>();
        var seenNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var assemblies = AppDomain.CurrentDomain.GetAssemblies();

        // Traverse in reverse order so the newest hot-reloaded assembly wins if multiple exist
        for (int i = assemblies.Length - 1; i >= 0; i--)
        {
            var asm = assemblies[i];
            if (asm.IsDynamic)
                continue;

            string? name = asm.GetName().Name;
            if (string.IsNullOrEmpty(name) || name.StartsWith("ℛ*") || name.StartsWith("AIDebugDynamicPatch_"))
                continue;
            if (!seenNames.Add(name))
                continue;

            lock (_metadataCache)
            {
                if (_metadataCache.TryGetValue(asm, out var cached))
                {
                    list.Add(cached);
                    continue;
                }
            }

            MetadataReference? metaRef = null;
            try
            {
                if (asm.TryGetRawMetadata(out byte* blob, out int length) && blob != null && length > 0)
                {
                    var moduleMeta = ModuleMetadata.CreateFromMetadata((IntPtr)blob, length);
                    var asmMeta = AssemblyMetadata.Create(moduleMeta);
                    metaRef = asmMeta.GetReference();
                }
            }
            catch { }

            if (metaRef == null)
            {
                try
                {
                    if (!string.IsNullOrEmpty(asm.Location) && File.Exists(asm.Location))
                    {
                        metaRef = MetadataReference.CreateFromFile(asm.Location);
                    }
                }
                catch { }
            }

            if (metaRef != null)
            {
                lock (_metadataCache)
                {
                    _metadataCache[asm] = metaRef;
                }
                list.Add(metaRef);
            }
        }

        return list;
    }

    public static List<Assembly> GetProjectAssemblies()
    {
        var result = new List<Assembly>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var all = AppDomain.CurrentDomain.GetAssemblies();

        for (int i = all.Length - 1; i >= 0; i--)
        {
            var asm = all[i];
            if (asm.IsDynamic)
                continue;
            string name = asm.GetName().Name ?? "";
            if (string.IsNullOrEmpty(name))
                continue;
            if (name.StartsWith("System", StringComparison.Ordinal) ||
                name.StartsWith("Microsoft", StringComparison.Ordinal) ||
                name.StartsWith("GodotSharp", StringComparison.Ordinal) ||
                name.StartsWith("GodotPlugins", StringComparison.Ordinal) ||
                name.StartsWith("GodotTools", StringComparison.Ordinal) ||
                name.StartsWith("Godot.AIDebug", StringComparison.Ordinal) ||
                name.StartsWith("0Harmony", StringComparison.Ordinal) ||
                name.StartsWith("MonoMod", StringComparison.Ordinal) ||
                name.StartsWith("Mono.Cecil", StringComparison.Ordinal) ||
                name.StartsWith("netstandard", StringComparison.Ordinal) ||
                name.StartsWith("mscorlib", StringComparison.Ordinal) ||
                name.StartsWith("ℛ*", StringComparison.Ordinal) ||
                name.StartsWith("AIDebugDynamicPatch_", StringComparison.Ordinal))
            {
                continue;
            }
            if (seen.Add(name))
            {
                result.Add(asm);
            }
        }

        return result;
    }

    private static object? FormatValue(object? val, int depth)
    {
        if (val == null)
            return null;
        if (val is bool or int or long or short or byte or sbyte or uint or ulong or ushort or float or double or decimal or string)
            return val;
        if (val is StringName sn)
            return sn.ToString();
        if (val is NodePath np)
            return np.ToString();
        if (val is Enum)
            return val.ToString();
        if (val is Variant v)
            return v.ToString();
        if (depth >= 3)
            return val.ToString();

        if (val is IDictionary dict)
        {
            var outDict = new Dictionary<string, object?>();
            foreach (DictionaryEntry de in dict)
            {
                outDict[de.Key?.ToString() ?? "null"] = FormatValue(de.Value, depth + 1);
            }
            return outDict;
        }

        if (val is IEnumerable seq and not string)
        {
            var outList = new List<object?>();
            int count = 0;
            foreach (var item in seq)
            {
                if (count++ >= 100)
                {
                    outList.Add("... (truncated)");
                    break;
                }
                outList.Add(FormatValue(item, depth + 1));
            }
            return outList;
        }

        if (val is Node node)
        {
            AIDebugGlobals.RequireMainThread();
            return new Dictionary<string, object?>
            {
                ["_type"] = node.GetType().FullName,
                ["_class"] = node.GetClass(),
                ["name"] = node.Name.ToString(),
            };
        }

        return val.ToString();
    }

    // ========================================================================
    // Minimal JSON Parser & Serializer
    // ========================================================================

    internal static Dictionary<string, object?> ParseJsonDict(string json)
    {
        if (string.IsNullOrWhiteSpace(json))
            return new Dictionary<string, object?>();
        try
        {
            using var doc = JsonDocument.Parse(json, new JsonDocumentOptions
            {
                AllowTrailingCommas = true,
                CommentHandling = JsonCommentHandling.Skip,
            });
            if (FromJsonElement(doc.RootElement) is Dictionary<string, object?> d)
                return d;
        }
        catch { }
        return new Dictionary<string, object?>();
    }

    private static object? FromJsonElement(JsonElement el)
    {
        switch (el.ValueKind)
        {
            case JsonValueKind.Object:
                var d = new Dictionary<string, object?>();
                foreach (var p in el.EnumerateObject())
                    d[p.Name] = FromJsonElement(p.Value);
                return d;
            case JsonValueKind.Array:
                var l = new List<object?>();
                foreach (var item in el.EnumerateArray())
                    l.Add(FromJsonElement(item));
                return l;
            case JsonValueKind.String:
                return el.GetString();
            case JsonValueKind.Number:
                return el.TryGetInt64(out long i) ? i : el.GetDouble();
            case JsonValueKind.True:
                return true;
            case JsonValueKind.False:
                return false;
            default:
                return null;
        }
    }

    internal static string SerializeJson(object? value)
    {
        var sb = new StringBuilder();
        WriteJsonValue(sb, value);
        return sb.ToString();
    }

    private static void WriteJsonValue(StringBuilder sb, object? v)
    {
        switch (v)
        {
            case null:
                sb.Append("null");
                break;
            case bool b:
                sb.Append(b ? "true" : "false");
                break;
            case string s:
                sb.Append(Bootstrap.QuoteJson(s));
                break;
            case int or long or short or byte or sbyte or uint or ulong or ushort:
                sb.Append(Convert.ToString(v, CultureInfo.InvariantCulture));
                break;
            case float or double or decimal:
                double dv = Convert.ToDouble(v, CultureInfo.InvariantCulture);
                if (double.IsNaN(dv) || double.IsInfinity(dv))
                    sb.Append("null");
                else
                    sb.Append(dv.ToString("R", CultureInfo.InvariantCulture));
                break;
            case IDictionary<string, object?> dict:
                sb.Append('{');
                bool firstProp = true;
                foreach (var kv in dict)
                {
                    if (!firstProp) sb.Append(',');
                    firstProp = false;
                    sb.Append(Bootstrap.QuoteJson(kv.Key)).Append(':');
                    WriteJsonValue(sb, kv.Value);
                }
                sb.Append('}');
                break;
            case IEnumerable seq:
                sb.Append('[');
                bool firstElem = true;
                foreach (var item in seq)
                {
                    if (!firstElem) sb.Append(',');
                    firstElem = false;
                    WriteJsonValue(sb, item);
                }
                sb.Append(']');
                break;
            default:
                sb.Append(Bootstrap.QuoteJson(v.ToString() ?? ""));
                break;
        }
    }

    private static string GetStr(Dictionary<string, object?> d, string key, string def = "")
    {
        return d.TryGetValue(key, out var v) && v != null ? v.ToString() ?? def : def;
    }

    private static long GetLong(Dictionary<string, object?> d, string key, long def = 0)
    {
        if (!d.TryGetValue(key, out var v) || v == null)
            return def;
        return v switch
        {
            long l => l,
            int i => i,
            double db => (long)db,
            string s when long.TryParse(s, out var r) => r,
            _ => def,
        };
    }

    private static bool GetBool(Dictionary<string, object?> d, string key, bool def = false)
    {
        if (!d.TryGetValue(key, out var v) || v == null)
            return def;
        return v switch
        {
            bool b => b,
            long l => l != 0,
            string s => s.Equals("true", StringComparison.OrdinalIgnoreCase) || s == "1",
            _ => def,
        };
    }
}
