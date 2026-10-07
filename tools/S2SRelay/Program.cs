// A mock of a PlayServ platform feature that does not exist yet: a server-to-server channel on the game-server uplink.
// Game servers keep one WebSocket here (?region=N) and send frames shaped like the uplink's own:
//   in   {"type":"s2s_send","to":4,"data":"<base64>"}
//   out  {"type":"s2s_message","from":3,"data":"<base64>"}
// Frames to one server leave in the order they came. The relay adds no meaning to the bytes; it only routes them.
//
//   dotnet run --project tools/S2SRelay -- [port=7900] [delayMs=0]
// delayMs holds every frame that long before it is forwarded, to stand in for the hops to and from a platform region.
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;

var port = args.Length > 0 ? int.Parse(args[0]) : 7900;
var delayMs = args.Length > 1 ? int.Parse(args[1]) : 0;
var listener = new HttpListener();
listener.Prefixes.Add($"http://127.0.0.1:{port}/");
listener.Start();
Console.WriteLine($"s2s relay on ws://127.0.0.1:{port}/s2s, delay {delayMs} ms");

var servers = new ConcurrentDictionary<int, Peer>();
long frames = 0, bytes = 0, dropped = 0;
var clock = Stopwatch.StartNew();
_ = Task.Run(async () =>
{
    while (true)
    {
        await Task.Delay(5000);
        var f = Interlocked.Exchange(ref frames, 0);
        var b = Interlocked.Exchange(ref bytes, 0);
        Console.WriteLine($"{DateTime.UtcNow:HH:mm:ss} servers={servers.Count} frames/s={f / 5.0:F1} bytes/s={b / 5.0:F0} dropped={Interlocked.Read(ref dropped)}");
    }
});

while (true)
{
    var context = await listener.GetContextAsync();
    if (!context.Request.IsWebSocketRequest)
    {
        context.Response.StatusCode = 400;
        context.Response.Close();
        continue;
    }
    _ = Task.Run(() => Serve(context));
}

async Task Serve(HttpListenerContext context)
{
    var region = int.TryParse(context.Request.QueryString["region"], out var r) ? r : -1;
    WebSocketContext socketContext;
    try { socketContext = await context.AcceptWebSocketAsync(null); }
    catch (Exception e) { Console.WriteLine($"accept failed: {e.Message}"); return; }
    var peer = new Peer(region, socketContext.WebSocket, delayMs);
    servers[region] = peer;
    Console.WriteLine($"region {region} connected");
    var buffer = new byte[1 << 16];
    var message = new MemoryStream();
    try
    {
        while (peer.Socket.State == WebSocketState.Open)
        {
            message.SetLength(0);
            WebSocketReceiveResult result;
            do
            {
                result = await peer.Socket.ReceiveAsync(buffer, CancellationToken.None);
                if (result.MessageType == WebSocketMessageType.Close) break;
                message.Write(buffer, 0, result.Count);
            } while (!result.EndOfMessage);
            if (result.MessageType == WebSocketMessageType.Close) break;
            Route(region, message.ToArray());
        }
    }
    catch (Exception e) { Console.WriteLine($"region {region}: {e.Message}"); }
    servers.TryRemove(new KeyValuePair<int, Peer>(region, peer));
    Console.WriteLine($"region {region} left");
}

void Route(int from, byte[] frame)
{
    using var json = JsonDocument.Parse(frame);
    var root = json.RootElement;
    if (root.GetProperty("type").GetString() != "s2s_send") return;
    var to = root.GetProperty("to").GetInt32();
    var data = root.GetProperty("data").GetString();
    if (!servers.TryGetValue(to, out var target)) { Interlocked.Increment(ref dropped); return; }
    var outgoing = Encoding.UTF8.GetBytes($"{{\"type\":\"s2s_message\",\"from\":{from},\"data\":\"{data}\"}}");
    Interlocked.Increment(ref frames);
    Interlocked.Add(ref bytes, frame.Length);
    target.Enqueue(outgoing);
}

sealed class Peer
{
    public readonly int Region;
    public readonly WebSocket Socket;
    private readonly Channel<(long Due, byte[] Frame)> _outbox = Channel.CreateUnbounded<(long, byte[])>(new UnboundedChannelOptions { SingleReader = true });
    private readonly int _delayMs;

    public Peer(int region, WebSocket socket, int delayMs)
    {
        Region = region;
        Socket = socket;
        _delayMs = delayMs;
        _ = Task.Run(Pump);
    }

    public void Enqueue(byte[] frame) => _outbox.Writer.TryWrite((Environment.TickCount64 + _delayMs, frame));

    // One writer per server: frames leave in order, each when its delay is up.
    private async Task Pump()
    {
        await foreach (var (due, frame) in _outbox.Reader.ReadAllAsync())
        {
            var wait = due - Environment.TickCount64;
            if (wait > 0) await Task.Delay((int)wait);
            if (Socket.State != WebSocketState.Open) continue;
            try { await Socket.SendAsync(frame, WebSocketMessageType.Text, true, CancellationToken.None); }
            catch { }
        }
    }
}
