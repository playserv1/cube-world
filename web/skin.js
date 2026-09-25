// The player: Minecraft's model (head 8×8×8, body 8×12×4, arms and legs 4×12×4 pixels, drawn at 15/16
// of a block per 16 pixels) wearing a 64×64 skin in the standard layout. The skin is painted here from
// the player's id, so every player looks different and nothing is copied from Mojang.

import * as THREE from "three";
import { MODEL_SCALE } from "./spec.js";

const P = 1 / 16;

// Standard 64×64 skin layout: box (u, v) with size w × h × d unfolds as top and bottom above, then
// right, front, left and back side by side.
const LAYOUT = {
  head: { u: 0, v: 0, w: 8, h: 8, d: 8 },
  body: { u: 16, v: 16, w: 8, h: 12, d: 4 },
  rightArm: { u: 40, v: 16, w: 4, h: 12, d: 4 },
  leftArm: { u: 32, v: 48, w: 4, h: 12, d: 4 },
  rightLeg: { u: 0, v: 16, w: 4, h: 12, d: 4 },
  leftLeg: { u: 16, v: 48, w: 4, h: 12, d: 4 },
};

function rects({ u, v, w, h, d }) {
  return {
    top: [u + d, v, w, d], bottom: [u + d + w, v, w, d],
    right: [u, v + d, d, h], front: [u + d, v + d, w, h], left: [u + d + w, v + d, d, h], back: [u + d + w + d, v + d, w, h],
  };
}

function hash(text) {
  let h = 2166136261;
  for (const ch of text) { h ^= ch.charCodeAt(0); h = Math.imul(h, 16777619) >>> 0; }
  return h;
}

function hsl(h, s, l) { return `hsl(${h},${s}%,${l}%)`; }

export function paintSkin(playerId) {
  const seed = hash(playerId);
  const pick = (n, k) => ((seed >>> (k * 5)) % n);
  const skinTones = [[28, 45, 72], [28, 40, 62], [26, 45, 48], [24, 40, 36], [22, 38, 26]];
  const [sh, ss, sl] = skinTones[pick(skinTones.length, 0)];
  const shirtHue = pick(360, 1), pantsHue = pick(360, 2), hairL = 12 + pick(30, 3);
  const skin = hsl(sh, ss, sl), skinDark = hsl(sh, ss, sl - 10), hair = hsl(25 + pick(20, 4), 45, hairL);
  const shirt = hsl(shirtHue, 55, 45), shirtDark = hsl(shirtHue, 55, 36), pants = hsl(pantsHue, 45, 38), shoes = "#3a3a3a";
  const eye = ["#3b6bd6", "#4c9a4c", "#6b4a2b"][pick(3, 5)];

  const c = document.createElement("canvas"); c.width = 64; c.height = 64;
  const g = c.getContext("2d");
  const fill = (rect, colour) => { g.fillStyle = colour; g.fillRect(...rect); };
  const speckle = (rect, colour, n = 6) => {
    const r = rng(seed + rect[0] * 7 + rect[1]);
    g.fillStyle = colour;
    for (let i = 0; i < n; i++) g.fillRect(rect[0] + (r() * rect[2] | 0), rect[1] + (r() * rect[3] | 0), 1, 1);
  };

  // Head: skin all round, hair on top and the upper rows, a fringe on the front.
  const head = rects(LAYOUT.head);
  for (const face of Object.values(head)) fill(face, skin);
  fill(head.top, hair);
  for (const side of [head.right, head.left, head.back]) fill([side[0], side[1], side[2], 4], hair);
  fill([head.front[0], head.front[1], 8, 2], hair);
  fill([head.front[0] + 2, head.front[1] + 4, 1, 1], "#ffffff"); fill([head.front[0] + 3, head.front[1] + 4, 1, 1], eye);
  fill([head.front[0] + 4, head.front[1] + 4, 1, 1], eye); fill([head.front[0] + 5, head.front[1] + 4, 1, 1], "#ffffff");
  fill([head.front[0] + 3, head.front[1] + 5, 2, 1], skinDark);
  fill([head.front[0] + 3, head.front[1] + 6, 2, 1], hsl(sh, ss, sl - 22));

  // Body: the shirt, with a darker hem.
  const body = rects(LAYOUT.body);
  for (const face of Object.values(body)) { fill(face, shirt); speckle(face, shirtDark); }
  fill([body.front[0], body.front[1] + 11, 8, 1], shirtDark);
  fill([body.back[0], body.back[1] + 11, 8, 1], shirtDark);

  // Arms: skin, with a short sleeve. Legs: trousers with shoes.
  for (const name of ["rightArm", "leftArm"]) {
    const arm = rects(LAYOUT[name]);
    for (const face of Object.values(arm)) fill(face, skin);
    for (const face of [arm.right, arm.front, arm.left, arm.back]) fill([face[0], face[1], face[2], 3], shirt);
    fill(arm.top, shirt);
  }
  for (const name of ["rightLeg", "leftLeg"]) {
    const leg = rects(LAYOUT[name]);
    for (const face of Object.values(leg)) { fill(face, pants); speckle(face, hsl(pantsHue, 45, 30), 4); }
    for (const face of [leg.right, leg.front, leg.left, leg.back]) fill([face[0], face[1] + 10, face[2], 2], shoes);
    fill(leg.bottom, shoes);
  }
  return c;
}

function rng(seed) {
  let a = seed >>> 0;
  return () => { a = (a + 0x6d2b79f5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}

// A box whose six faces read from the skin rectangles. BoxGeometry face order: +x, -x, +y, -y, +z, -z.
function skinBox(part, material) {
  const { w, h, d } = LAYOUT[part];
  const r = rects(LAYOUT[part]);
  const geometry = new THREE.BoxGeometry(w * P, h * P, d * P);
  const uv = geometry.attributes.uv;
  const order = [r.left, r.right, r.top, r.bottom, r.front, r.back];
  order.forEach(([x, y, rw, rh], face) => {
    const u0 = x / 64, u1 = (x + rw) / 64, vTop = 1 - y / 64, vBottom = 1 - (y + rh) / 64;
    const values = [u0, vTop, u1, vTop, u0, vBottom, u1, vBottom];
    for (let i = 0; i < 4; i++) uv.setXY(face * 4 + i, values[i * 2], values[i * 2 + 1]);
  });
  uv.needsUpdate = true;
  return new THREE.Mesh(geometry, material);
}

// Pivots in pixels from the feet: neck at 24, shoulders at 22, hips at 12. The model faces +z.
export function buildPlayerModel(playerId) {
  const texture = new THREE.CanvasTexture(paintSkin(playerId));
  texture.magFilter = THREE.NearestFilter; texture.minFilter = THREE.NearestFilter; texture.colorSpace = THREE.SRGBColorSpace;
  const material = new THREE.MeshLambertMaterial({ map: texture });

  const part = (name, pivot, offset) => {
    const group = new THREE.Group();
    group.position.set(pivot[0] * P, pivot[1] * P, pivot[2] * P);
    const mesh = skinBox(name, material);
    mesh.position.set(offset[0] * P, offset[1] * P, offset[2] * P);
    group.add(mesh);
    return group;
  };
  const parts = {
    head: part("head", [0, 24, 0], [0, 4, 0]),
    body: part("body", [0, 24, 0], [0, -6, 0]),
    rightArm: part("rightArm", [-6, 22, 0], [0, -4, 0]),
    leftArm: part("leftArm", [6, 22, 0], [0, -4, 0]),
    rightLeg: part("rightLeg", [-2, 12, 0], [0, -6, 0]),
    leftLeg: part("leftLeg", [2, 12, 0], [0, -6, 0]),
  };
  const model = new THREE.Group();
  model.scale.setScalar(MODEL_SCALE);
  model.add(...Object.values(parts));
  model.userData.parts = parts;
  model.userData.material = material;
  model.userData.swing = 0;
  model.userData.amount = 0;
  return model;
}

// Walking swings arms and legs in opposition; the head follows the pitch; sneaking bends the body;
// a hurt player flashes red.
export function animatePlayer(model, { distance, pitch, sneaking, hurt }) {
  const u = model.userData, p = u.parts;
  u.swing += distance * 4;
  u.amount += (Math.min(1, distance * 4) - u.amount) * 0.4;
  const angle = Math.cos(u.swing * 0.6662), other = Math.cos(u.swing * 0.6662 + Math.PI);
  p.rightArm.rotation.x = other * 2 * u.amount * 0.5;
  p.leftArm.rotation.x = angle * 2 * u.amount * 0.5;
  p.rightLeg.rotation.x = angle * 1.4 * u.amount;
  p.leftLeg.rotation.x = other * 1.4 * u.amount;
  p.head.rotation.x = pitch;

  p.body.rotation.x = sneaking ? 0.5 : 0;
  p.head.position.y = (sneaking ? 24 - 4.2 : 24) * P;
  p.rightArm.position.y = p.leftArm.position.y = (sneaking ? 22 - 3.2 : 22) * P;
  p.body.position.y = (sneaking ? 24 - 3.2 : 24) * P;
  if (sneaking) { p.rightArm.rotation.x += 0.4; p.leftArm.rotation.x += 0.4; }

  u.material.color.setRGB(1, hurt ? 0.45 : 1, hurt ? 0.45 : 1);
}
