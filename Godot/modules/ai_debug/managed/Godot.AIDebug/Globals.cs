#nullable enable
using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Threading;
using Godot;
using HarmonyLib;

namespace Godot.AIDebug;

/// <summary>
/// Global context object exposed to Roslyn C# scripts in `eval_cs`.
/// Provides `Tree`, `Root`, `Node(path)`, `Find&lt;T&gt;()`, `Log()`, and private member helpers.
/// </summary>
public class AIDebugGlobals
{
    internal readonly List<string> Logs = new();

    public CancellationToken Cancellation { get; internal set; }

    internal static void RequireMainThread()
    {
        if (System.Environment.CurrentManagedThreadId != Agent.MainThreadId)
            throw new InvalidOperationException("SceneTree helpers require thread:'main'. Worker evaluations must use thread-safe data.");
    }

    public SceneTree Tree
    {
        get
        {
            RequireMainThread();
            return (SceneTree)Engine.GetMainLoop();
        }
    }

    public Window Root => Tree.Root;

    public Node? CurrentScene => Tree.CurrentScene;

    public Node? Node(string path)
    {
        return FindNodeByPathOrName(Root, CurrentScene, path);
    }

    internal static Node? FindNodeByPathOrName(Node? root, Node? currentScene, string path)
    {
        if (root == null)
            return null;
        path = path.Trim();
        if (string.IsNullOrEmpty(path))
            return currentScene ?? root;
        if (path == "/root" || path == "root")
            return root;
        if (path == "." && currentScene != null)
            return currentScene;

        if (path.StartsWith("/root/"))
            path = path.Substring(6);
        else if (path.StartsWith("/"))
            path = path.Substring(1);

        var segments = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (segments.Length > 0)
        {
            Node? cur = root;
            foreach (var seg in segments)
            {
                cur = FindDirectChild(cur, seg);
                if (cur == null) break;
            }
            if (cur != null) return cur;

            if (currentScene != null)
            {
                cur = currentScene;
                int startIdx = string.Equals(currentScene.Name.ToString(), segments[0], StringComparison.Ordinal) ? 1 : 0;
                for (int i = startIdx; i < segments.Length; i++)
                {
                    cur = FindDirectChild(cur, segments[i]);
                    if (cur == null) break;
                }
                if (cur != null) return cur;
            }
        }

        return FindRecursiveChild(root, path);
    }

    private static Node? FindDirectChild(Node parent, string name)
    {
        int count = parent.GetChildCount(includeInternal: true);
        for (int i = 0; i < count; i++)
        {
            var ch = parent.GetChild(i, includeInternal: true);
            if (ch != null && string.Equals(ch.Name.ToString(), name, StringComparison.Ordinal))
                return ch;
        }
        return null;
    }

    private static Node? FindRecursiveChild(Node parent, string name)
    {
        int count = parent.GetChildCount(includeInternal: true);
        for (int i = 0; i < count; i++)
        {
            var ch = parent.GetChild(i, includeInternal: true);
            if (ch == null) continue;
            if (string.Equals(ch.Name.ToString(), name, StringComparison.OrdinalIgnoreCase))
                return ch;
            var sub = FindRecursiveChild(ch, name);
            if (sub != null) return sub;
        }
        return null;
    }

    public T? Node<T>(string path) where T : class
    {
        var n = Node(path);
        if (n is T typed)
            return typed;
        return null;
    }

    public T? Find<T>(string? name = null) where T : class
    {
        return FindAll<T>(name, maxCount: 1).FirstOrDefault();
    }

    public List<T> FindAll<T>(string? name = null, int maxCount = int.MaxValue) where T : class
    {
        var results = new List<T>();
        var visited = new HashSet<object>(ReferenceEqualityComparer.Instance);
        var root = Root;
        if (root != null)
        {
            SearchTreeForType(root, name, results, visited, maxCount);
        }

        // If T is not a Godot.Node and nothing was found directly as a Node, also scan static fields of project assemblies
        if (results.Count < maxCount && !typeof(Node).IsAssignableFrom(typeof(T)))
        {
            foreach (var asm in Agent.GetProjectAssemblies())
            {
                Type[] types;
                try { types = asm.GetTypes(); }
                catch (ReflectionTypeLoadException ex) { types = ex.Types.Where(t => t != null).ToArray()!; }
                catch { continue; }

                foreach (var type in types)
                {
                    if (type.ContainsGenericParameters)
                        continue;
                    const BindingFlags flags = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
                    foreach (var field in type.GetFields(flags))
                    {
                        try
                        {
                            var val = field.GetValue(null);
                            if (val != null)
                                InspectObjectForType(val, name, results, visited, maxCount, depth: 0);
                        }
                        catch { }
                    }
                    foreach (var prop in type.GetProperties(flags))
                    {
                        if (!prop.CanRead || prop.GetIndexParameters().Length > 0)
                            continue;
                        try
                        {
                            var val = prop.GetValue(null);
                            if (val != null)
                                InspectObjectForType(val, name, results, visited, maxCount, depth: 0);
                        }
                        catch { }
                    }
                    if (results.Count >= maxCount)
                        break;
                }
            }
        }

        return results;
    }

    private static void SearchTreeForType<T>(Node node, string? name, List<T> results, HashSet<object> visited, int maxCount) where T : class
    {
        if (results.Count >= maxCount)
            return;

        bool nameMatches = string.IsNullOrEmpty(name) ||
                           node.Name.ToString().Contains(name, StringComparison.OrdinalIgnoreCase);

        if (node is T matchedNode && nameMatches && visited.Add(matchedNode))
        {
            results.Add(matchedNode);
            if (results.Count >= maxCount)
                return;
        }

        // If T is a non-Node C# object, also inspect fields/properties of C# nodes
        if (!typeof(Node).IsAssignableFrom(typeof(T)) && node.GetType().Assembly != typeof(Node).Assembly)
        {
            InspectObjectForType(node, name, results, visited, maxCount, depth: 0);
            if (results.Count >= maxCount)
                return;
        }

        int childCount = node.GetChildCount(includeInternal: true);
        for (int i = 0; i < childCount; i++)
        {
            var child = node.GetChild(i, includeInternal: true);
            if (child != null)
            {
                SearchTreeForType(child, name, results, visited, maxCount);
                if (results.Count >= maxCount)
                    return;
            }
        }
    }

    private static void InspectObjectForType<T>(object obj, string? name, List<T> results, HashSet<object> visited, int maxCount, int depth) where T : class
    {
        if (obj == null || results.Count >= maxCount || depth > 2 || !visited.Add(obj))
            return;

        if (obj is T directMatch)
        {
            results.Add(directMatch);
            if (results.Count >= maxCount)
                return;
        }

        Type t = obj.GetType();
        if (t.IsPrimitive || t == typeof(string) || t.Namespace?.StartsWith("System") == true || t.Assembly == typeof(Node).Assembly)
            return;

        const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        for (Type? cur = t; cur != null && cur != typeof(object) && cur.Assembly != typeof(Node).Assembly; cur = cur.BaseType)
        {
            foreach (var f in cur.GetFields(flags | BindingFlags.DeclaredOnly))
            {
                try
                {
                    var v = f.GetValue(obj);
                    if (v is T tv && (string.IsNullOrEmpty(name) || f.Name.Contains(name, StringComparison.OrdinalIgnoreCase)))
                    {
                        if (visited.Add(tv))
                        {
                            results.Add(tv);
                            if (results.Count >= maxCount)
                                return;
                        }
                    }
                    else if (v != null && depth < 1)
                    {
                        InspectObjectForType(v, name, results, visited, maxCount, depth + 1);
                    }
                }
                catch { }
            }
        }
    }

    public void Log(object? message)
    {
        string text = message?.ToString() ?? "null";
        lock (Logs)
        {
            Logs.Add(text);
        }
        GD.Print("[AI Debug] " + text);
    }

    public Traverse Traverse(object target) => target is Type t ? HarmonyLib.Traverse.Create(t) : HarmonyLib.Traverse.Create(target);

    public Traverse Traverse<T>() => HarmonyLib.Traverse.Create<T>();

    public object? GetPrivate(object target, string memberName)
    {
        Type? type = target as Type ?? target?.GetType();
        if (type == null)
            throw new ArgumentNullException(nameof(target));
        object? instance = target is Type ? null : target;

        var field = AccessTools.Field(type, memberName);
        if (field != null)
            return field.GetValue(instance);

        var prop = AccessTools.Property(type, memberName);
        if (prop != null)
            return prop.GetValue(instance);

        throw new MissingMemberException(type.FullName, memberName);
    }

    public void SetPrivate(object target, string memberName, object? value)
    {
        Type? type = target as Type ?? target?.GetType();
        if (type == null)
            throw new ArgumentNullException(nameof(target));
        object? instance = target is Type ? null : target;

        var field = AccessTools.Field(type, memberName);
        if (field != null)
        {
            field.SetValue(instance, value);
            return;
        }

        var prop = AccessTools.Property(type, memberName);
        if (prop != null)
        {
            prop.SetValue(instance, value);
            return;
        }

        throw new MissingMemberException(type.FullName, memberName);
    }

    public object? CallPrivate(object target, string methodName, params object?[] args)
    {
        Type? type = target as Type ?? target?.GetType();
        if (type == null)
            throw new ArgumentNullException(nameof(target));
        object? instance = target is Type ? null : target;

        Type[]? argTypes = args.All(a => a != null) ? args.Select(a => a!.GetType()).ToArray() : null;
        var method = argTypes != null
            ? AccessTools.Method(type, methodName, argTypes) ?? AccessTools.Method(type, methodName)
            : AccessTools.Method(type, methodName);

        if (method == null)
            throw new MissingMethodException(type.FullName, methodName);

        return method.Invoke(instance, args);
    }
}
