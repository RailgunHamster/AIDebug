using System;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AIDebug.Sbox;

internal static class J
{
    internal static readonly JsonSerializerOptions Options = new() { WriteIndented = true, IncludeFields = true };
    internal static string S(this JsonObject o, string key, string fallback = "") => o?[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : fallback;
    internal static bool B(this JsonObject o, string key, bool fallback = false) => o?[key] is JsonValue v && v.TryGetValue<bool>(out var b) ? b : fallback;
    internal static int I(this JsonObject o, string key, int fallback = 0) => o?[key] is JsonValue v && v.TryGetValue<int>(out var n) ? n : fallback;
    internal static JsonObject O(this JsonObject o, string key) => o?[key] as JsonObject ?? new();
    internal static JsonArray A(this JsonObject o, string key) => o?[key] as JsonArray ?? new();
    internal static JsonNode Copy(this JsonNode node) => node?.DeepClone();
    internal static JsonNode Node(object value) => value is JsonNode n ? n.DeepClone() : JsonSerializer.SerializeToNode(value, Options);
    internal static JsonObject Error(Exception e) => new() { ["ok"] = false, ["error"] = e.GetBaseException().Message, ["type"] = e.GetBaseException().GetType().Name };
    internal static JsonObject TextResult(JsonNode result, bool error = false) => new() { ["content"] = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = result?.ToJsonString(Options) ?? "null" }), ["isError"] = error };
}
