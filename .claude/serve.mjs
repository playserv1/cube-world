import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join } from "node:path";
const root = process.argv[2];
const types = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".json": "application/json" };
createServer(async (req, res) => {
  const path = decodeURIComponent(req.url.split("?")[0]);
  try {
    const data = await readFile(join(root, path === "/" ? "index.html" : path));
    res.writeHead(200, { "Content-Type": types[extname(path)] ?? "application/octet-stream", "Cache-Control": "no-store" });
    res.end(data);
  } catch { res.writeHead(404); res.end("not found"); }
}).listen(process.env.PORT ?? 5173, () => console.log("serving", root, "on", process.env.PORT ?? 5173));
