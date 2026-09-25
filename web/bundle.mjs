// Builds dist/cube-world.html: the whole client in one file that opens from disk (double-click)
// and connects to the servers named in web/config.js. three.js still comes from the CDN.
//   node web/bundle.mjs
import { build } from "esbuild";
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const web = dirname(fileURLToPath(import.meta.url));
const out = join(web, "..", "dist");
mkdirSync(out, { recursive: true });

const { outputFiles } = await build({
  entryPoints: [join(web, "app.js")],
  bundle: true, format: "esm", write: false, minify: false,
  external: ["three", "three/addons/*"],
});
const app = outputFiles[0].text;
const css = readFileSync(join(web, "style.css"), "utf8");
const config = readFileSync(join(web, "config.js"), "utf8");
let html = readFileSync(join(web, "index.html"), "utf8");
html = html.replace('<link rel="stylesheet" href="style.css">', `<style>\n${css}</style>`);
html = html.replace('<script src="config.js"></script>', `<script>\n${config}</script>`);
html = html.replace('<script type="module" src="app.js"></script>', `<script type="module">\n${app.replace(/<\/script/g, "<\\/script")}</script>`);
writeFileSync(join(out, "cube-world.html"), html);
console.log("wrote", join(out, "cube-world.html"), `${(html.length / 1024).toFixed(0)} KB`);
