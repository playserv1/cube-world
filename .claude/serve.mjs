import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join } from "node:path";
const root = process.argv[2];
const types = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".json": "application/json", ".svg": "image/svg+xml" };
createServer(async (req, res) => {
  const path = decodeURIComponent(req.url.split("?")[0]);
  const file = path === "/" ? "index.html" : path;
  try {
    const data = await readFile(join(root, file));
    res.writeHead(200, { "Content-Type": types[extname(file)] ?? "application/octet-stream", "Cache-Control": "no-store" });
    res.end(data);
  } catch { res.writeHead(404); res.end("not found"); }
}).listen(process.env.PORT ?? 5173, () => console.log("serving", root, "on", process.env.PORT ?? 5173));
