using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json.Nodes;
namespace AIDebug.Sbox;
internal sealed class DebugTransport:IDisposable
{
    private readonly TcpListener listener;
    private readonly CancellationTokenSource stop=new();
    private int clients;
    internal DebugTransport(int port){if(port is <1024 or >65535)throw new ArgumentOutOfRangeException(nameof(port));listener=new(IPAddress.Loopback,port);}
    internal void Start(){listener.Start(32);_=Task.Run(Accept);}
    public void Dispose(){stop.Cancel();listener.Stop();}
    private async Task Accept(){while(!stop.IsCancellationRequested){try{var client=await listener.AcceptTcpClientAsync(stop.Token);if(Interlocked.Increment(ref clients)>32){client.Dispose();Interlocked.Decrement(ref clients);continue;}_=Serve(client);}catch(OperationCanceledException){break;}catch(ObjectDisposedException){break;}}}
    private async Task Serve(TcpClient client)
    {
        using(client)using(var cancellation=CancellationTokenSource.CreateLinkedTokenSource(stop.Token)){
            cancellation.CancelAfter(TimeSpan.FromSeconds(30));
            try{
                var stream=client.GetStream();var header=new List<byte>();var one=new byte[1];bool complete=false;
                while(header.Count<16384){if(await stream.ReadAsync(one,cancellation.Token)==0)return;header.Add(one[0]);var n=header.Count;if(n>=4&&header[n-4]==13&&header[n-3]==10&&header[n-2]==13&&header[n-1]==10){complete=true;break;}}
                if(!complete){await Reply(stream,431,new(){["error"]="Header exceeds 16 KiB"});return;}
                var lines=Encoding.ASCII.GetString(header.ToArray()).Split("\r\n");var request=lines[0].Split(' ');
                var headers=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
                foreach(var line in lines.Skip(1).Where(x=>x.Length>0)){var colon=line.IndexOf(':');if(colon<1||!headers.TryAdd(line[..colon].Trim(),line[(colon+1)..].Trim())){await Reply(stream,400,new(){["error"]="Malformed or duplicate header"});return;}}
                if(headers.ContainsKey("Transfer-Encoding")){await Reply(stream,400,new(){["error"]="Chunked requests are unsupported"});return;}
                var hasLength=headers.TryGetValue("Content-Length",out var length);var size=0;
                if(hasLength&&(!int.TryParse(length,out size)||size<0||size>16*1024*1024)){await Reply(stream,413,new(){["error"]="Invalid Content-Length"});return;}
                var body=new byte[size];if(size>0)await stream.ReadExactlyAsync(body,cancellation.Token);
                if(request.Length!=3||request[1]!="/mcp"){await Reply(stream,404,new(){["error"]="Not found"});return;}
                if(!headers.TryGetValue("Host",out var host)||!Uri.TryCreate("http://"+host,UriKind.Absolute,out var uri)||!uri.IsLoopback){await Reply(stream,403,new(){["error"]="Loopback Host required"});return;}
                if(headers.TryGetValue("Origin",out var origin)&&(!Uri.TryCreate(origin,UriKind.Absolute,out var originUri)||!originUri.IsLoopback)){await Reply(stream,403,new(){["error"]="Origin rejected"});return;}
                var token=RuntimeService.Token;
                if(token.Length>0&&headers.GetValueOrDefault("Authorization")!="Bearer "+token&&headers.GetValueOrDefault("X-AIDebug-Token")!=token){await Reply(stream,401,new(){["error"]="Token required"});return;}
                if(request[0]=="DELETE"){await Reply(stream,200,J.TextResult(await RuntimeService.Call("reset",new(),cancellation.Token)));return;}
                if(request[0]!="POST"){await Reply(stream,405,new(){["error"]="Use JSON-RPC POST; SSE is not provided"});return;}
                if(!hasLength){await Reply(stream,411,new(){["error"]="Content-Length required"});return;}
                cancellation.CancelAfter(Timeout.InfiniteTimeSpan);
                _=Task.Run(async()=>{try{while(!cancellation.IsCancellationRequested){if(client.Client.Poll(0,SelectMode.SelectRead)&&client.Available==0){await cancellation.CancelAsync();break;}await Task.Delay(100,cancellation.Token);}}catch(OperationCanceledException){}catch(ObjectDisposedException){}});
                var message=JsonNode.Parse(body)?.AsObject()??throw new ArgumentException("One JSON-RPC object is required");
                var result=await Rpc(message,cancellation.Token);await Reply(stream,result==null?202:200,result);
            }catch(Exception e){try{await Reply(client.GetStream(),400,J.Error(e));}catch{}}
            finally{await cancellation.CancelAsync();Interlocked.Decrement(ref clients);}
        }
    }
    private static async Task Reply(NetworkStream stream,int status,JsonObject? value){
        var body=value==null?Array.Empty<byte>():Encoding.UTF8.GetBytes(value.ToJsonString());
        var header=Encoding.ASCII.GetBytes($"HTTP/1.1 {status} Response\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {body.Length}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
        await stream.WriteAsync(header);await stream.WriteAsync(body);
    }
    private static async Task<JsonObject?> Rpc(JsonObject message,CancellationToken cancellation){
        var id=message["id"]?.DeepClone();var method=message.S("method");if(method.StartsWith("notifications/"))return null;
        JsonNode? result;
        switch(method){
            case "initialize":result=new JsonObject{["protocolVersion"]=message.O("params").S("protocolVersion","2025-11-25"),["capabilities"]=new JsonObject{["tools"]=new JsonObject()},["serverInfo"]=new JsonObject{["name"]="sbox-ai-debug",["version"]=RuntimeService.Version}};break;
            case "ping":result=new JsonObject();break;
            case "tools/list":result=new JsonObject{["tools"]=RuntimeService.Definitions()};break;
            case "tools/call":{
                try{var p=message.O("params");var output=await RuntimeService.Call(p.S("name"),p.O("arguments"),cancellation);result=J.TextResult(output,output is JsonObject o&&o.ContainsKey("ok")&&!o.B("ok"));}
                catch(Exception e){result=J.TextResult(J.Error(e),true);}break;
            }
            default:return new(){["jsonrpc"]="2.0",["id"]=id,["error"]=new JsonObject{["code"]=-32601,["message"]="Unknown method: "+method}};
        }
        return new(){["jsonrpc"]="2.0",["id"]=id,["result"]=result};
    }
}
