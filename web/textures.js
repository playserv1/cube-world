// Block textures: 16 × 16 pixel tiles drawn here, in the Minecraft style but our own pixels
// (Mojang's assets are copyrighted). Grass has three faces, a log two, everything else one.

import * as THREE from "three";
import { TEXTURE_SIZE as T } from "./spec.js";

export const FACES = {
  grass: { top: "grass_top", side: "grass_side", bottom: "dirt" },
  dirt: { top: "dirt", side: "dirt", bottom: "dirt" },
  sand: { top: "sand", side: "sand", bottom: "sand" },
  stone: { top: "stone", side: "stone", bottom: "stone" },
  wood: { top: "log_top", side: "log_side", bottom: "log_top" },
  brick: { top: "brick", side: "brick", bottom: "brick" },
  glass: { top: "glass", side: "glass", bottom: "glass" },
  gold: { top: "gold", side: "gold", bottom: "gold" },
  bedrock: { top: "bedrock", side: "bedrock", bottom: "bedrock" },
};

const CRACKS = Array.from({ length: 10 }, (_, i) => `crack${i}`);
export const TILES = ["grass_top", "grass_side", "dirt", "sand", "stone", "log_side", "log_top", "brick", "glass", "gold", "bedrock", ...CRACKS];
const COLUMNS = 8;

function rng(seed) {
  let a = seed >>> 0;
  return () => { a = (a + 0x6d2b79f5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}

function shade([r, g, b], k) {
  return `rgb(${Math.round(Math.min(255, r * k))},${Math.round(Math.min(255, g * k))},${Math.round(Math.min(255, b * k))})`;
}

// Fills a tile with a base colour jittered per pixel: the grainy look every block has.
function noise(g, x0, y0, base, spread, random) {
  for (let y = 0; y < T; y++) for (let x = 0; x < T; x++) {
    g.fillStyle = shade(base, 1 - spread / 2 + random() * spread);
    g.fillRect(x0 + x, y0 + y, 1, 1);
  }
}

const painters = {
  grass_top: (g, x, y, r) => noise(g, x, y, [93, 160, 60], 0.28, r),
  dirt: (g, x, y, r) => { noise(g, x, y, [121, 85, 58], 0.3, r); for (let i = 0; i < 14; i++) { g.fillStyle = shade([90, 62, 40], 1); g.fillRect(x + (r() * T | 0), y + (r() * T | 0), 1, 1); } },
  sand: (g, x, y, r) => noise(g, x, y, [219, 205, 160], 0.14, r),
  stone: (g, x, y, r) => { noise(g, x, y, [126, 126, 126], 0.22, r); for (let i = 0; i < 6; i++) { g.fillStyle = shade([100, 100, 100], 1); g.fillRect(x + (r() * 14 | 0), y + (r() * 14 | 0), 2, 1); } },
  bedrock: (g, x, y, r) => { noise(g, x, y, [80, 80, 80], 0.9, r); },
  grass_side: (g, x, y, r) => {
    painters.dirt(g, x, y, r);
    for (let px = 0; px < T; px++) {
      const depth = 2 + (r() * 3 | 0);
      for (let py = 0; py < depth; py++) { g.fillStyle = shade([93, 160, 60], 0.85 + r() * 0.3); g.fillRect(x + px, y + py, 1, 1); }
    }
  },
  log_side: (g, x, y, r) => {
    for (let px = 0; px < T; px++) {
      const tone = px % 4 === 0 ? 0.75 : px % 4 === 2 ? 1.05 : 0.92;
      for (let py = 0; py < T; py++) { g.fillStyle = shade([104, 78, 46], tone * (0.92 + r() * 0.16)); g.fillRect(x + px, y + py, 1, 1); }
    }
  },
  log_top: (g, x, y, r) => {
    noise(g, x, y, [104, 78, 46], 0.2, r);
    g.fillStyle = shade([176, 138, 85], 1); g.fillRect(x + 2, y + 2, 12, 12);
    g.fillStyle = shade([150, 114, 66], 1); g.fillRect(x + 4, y + 4, 8, 8);
    g.fillStyle = shade([176, 138, 85], 1); g.fillRect(x + 6, y + 6, 4, 4);
    g.fillStyle = shade([150, 114, 66], 1); g.fillRect(x + 7, y + 7, 2, 2);
  },
  brick: (g, x, y, r) => {
    noise(g, x, y, [188, 176, 166], 0.1, r);
    for (let row = 0; row < 4; row++) {
      const offset = row % 2 ? 4 : 0;
      for (let col = -1; col < 3; col++) {
        const bx = col * 8 + offset;
        for (let py = 0; py < 3; py++) for (let px = 0; px < 7; px++) {
          const X = bx + px; if (X < 0 || X >= T) continue;
          g.fillStyle = shade([150, 72, 58], 0.85 + r() * 0.3); g.fillRect(x + X, y + row * 4 + py, 1, 1);
        }
      }
    }
  },
  glass: (g, x, y) => {
    g.clearRect(x, y, T, T);
    g.fillStyle = "rgba(255,255,255,0.9)";
    g.fillRect(x, y, T, 1); g.fillRect(x, y + T - 1, T, 1); g.fillRect(x, y, 1, T); g.fillRect(x + T - 1, y, 1, T);
    g.fillStyle = "rgba(225,240,255,0.55)";
    for (let i = 0; i < 6; i++) g.fillRect(x + 11 - i, y + 2 + i, 1, 1);
    for (let i = 0; i < 3; i++) g.fillRect(x + 6 - i, y + 9 + i, 1, 1);
  },
  gold: (g, x, y, r) => {
    noise(g, x, y, [232, 190, 50], 0.1, r);
    g.fillStyle = shade([255, 236, 140], 1); g.fillRect(x + 1, y + 1, 14, 1); g.fillRect(x + 1, y + 1, 1, 14);
    g.fillStyle = shade([170, 128, 20], 1); g.fillRect(x + 1, y + 14, 14, 1); g.fillRect(x + 14, y + 1, 1, 14);
    g.fillStyle = shade([255, 225, 96], 1); g.fillRect(x + 4, y + 4, 8, 8);
    g.fillStyle = shade([232, 190, 50], 1); g.fillRect(x + 6, y + 6, 4, 4);
  },
};

// The ten crack stages a block goes through while it is being dug.
function crack(g, x, y, stage) {
  const r = rng(77);
  g.clearRect(x, y, T, T);
  g.fillStyle = "rgba(0,0,0,0.55)";
  const lines = 2 + stage * 2;
  for (let i = 0; i < lines; i++) {
    let px = 3 + (r() * 10 | 0), py = 3 + (r() * 10 | 0);
    const length = 3 + (r() * (4 + stage) | 0);
    for (let s = 0; s < length; s++) {
      g.fillRect(x + px, y + py, 1, 1);
      px += Math.sign(r() - 0.5) * (r() < 0.6 ? 1 : 0); py += Math.sign(r() - 0.5) * (r() < 0.6 ? 1 : 0);
      px = Math.max(0, Math.min(T - 1, px)); py = Math.max(0, Math.min(T - 1, py));
    }
  }
}

export function buildAtlas() {
  const rows = Math.ceil(TILES.length / COLUMNS);
  const canvas = document.createElement("canvas");
  canvas.width = COLUMNS * T; canvas.height = rows * T;
  const g = canvas.getContext("2d");
  const random = rng(20260925);
  TILES.forEach((name, i) => {
    const x = (i % COLUMNS) * T, y = Math.floor(i / COLUMNS) * T;
    if (name.startsWith("crack")) crack(g, x, y, Number(name.slice(5)));
    else painters[name](g, x, y, random);
  });
  const texture = new THREE.CanvasTexture(canvas);
  texture.magFilter = THREE.NearestFilter;
  texture.minFilter = THREE.NearestFilter;
  texture.generateMipmaps = false;
  texture.colorSpace = THREE.SRGBColorSpace;

  const uv = name => {
    const i = TILES.indexOf(name);
    const col = i % COLUMNS, row = Math.floor(i / COLUMNS);
    return { u0: col / COLUMNS, u1: (col + 1) / COLUMNS, v0: 1 - (row + 1) / rows, v1: 1 - row / rows };
  };
  const tile = name => {
    const i = TILES.indexOf(name);
    const c = document.createElement("canvas"); c.width = T; c.height = T;
    c.getContext("2d").drawImage(canvas, (i % COLUMNS) * T, Math.floor(i / COLUMNS) * T, T, T, 0, 0, T, T);
    return c;
  };
  return { texture, canvas, uv, tile };
}

// The hotbar icon: the block drawn as Minecraft draws items, an isometric cube lit from the top.
export function blockIcon(atlas, kind, size = 40) {
  const faces = FACES[kind];
  const c = document.createElement("canvas"); c.width = size; c.height = size;
  const g = c.getContext("2d");
  g.imageSmoothingEnabled = false;
  const s = size * 0.42, cx = size / 2, top = size * 0.08;
  const draw = (tileName, matrix, dark) => {
    g.setTransform(...matrix);
    g.drawImage(atlas.tile(tileName), 0, 0, T, T, 0, 0, 1, 1);
    if (dark) { g.fillStyle = `rgba(0,0,0,${dark})`; g.fillRect(0, 0, 1, 1); }
  };
  draw(faces.top, [s, s / 2, -s, s / 2, cx, top], 0);
  draw(faces.side, [s, s / 2, 0, s, cx - s, top + s / 2], 0.2);
  draw(faces.side, [s, -s / 2, 0, s, cx, top + s], 0.4);
  g.setTransform(1, 0, 0, 1, 0, 0);
  return c.toDataURL();
}
