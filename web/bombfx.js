// What a bomb looks like: a creeper head that flashes white as a creeper does before it blows, a striped
// parachute while it comes down, an explosion of fire, debris and smoke, and the light puff of smoke of a bomb
// that fizzles out.

import * as THREE from "three";

// The head is 8 × 8 × 8 pixels: mottled green all round, the face on one side (the unreal client paints the same).
const GREENS = ["#4c9a3a", "#5cb247", "#6fc452", "#3f8a31", "#85d16b"];
const FACE = [
  "........",
  "........",
  ".##..##.",
  ".##..##.",
  "...##...",
  "..####..",
  "..####..",
  "..#..#..",
];

function headTexture(face) {
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = 8;
  const g = canvas.getContext("2d");
  for (let y = 0; y < 8; y++)
    for (let x = 0; x < 8; x++) {
      g.fillStyle = face && FACE[y][x] === "#" ? "#101410" : GREENS[(x * 7 + y * 13 + (face ? 3 : 0)) % GREENS.length];
      g.fillRect(x, y, 1, 1);
    }
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.magFilter = THREE.NearestFilter;
  texture.minFilter = THREE.NearestFilter;
  return texture;
}

let headMaterials;

export function buildBomb(radius = 0.25) {
  if (!headMaterials) {
    const skin = new THREE.MeshLambertMaterial({ map: headTexture(false) });
    const face = new THREE.MeshLambertMaterial({ map: headTexture(true) });
    // BoxGeometry's faces: +x, −x, +y, −y, +z, −z; the face looks along +z, at the holder's camera.
    headMaterials = [skin, skin, skin, skin, face, skin];
  }
  const bomb = new THREE.Mesh(new THREE.BoxGeometry(radius * 2, radius * 2, radius * 2), headMaterials);
  bomb.position.y = radius;
  const holder = new THREE.Group();
  holder.add(bomb);
  holder.userData.bomb = bomb;
  return holder;
}

function stripes() {
  const canvas = document.createElement("canvas");
  canvas.width = 128; canvas.height = 8;
  const g = canvas.getContext("2d");
  for (let i = 0; i < 8; i++) { g.fillStyle = i % 2 ? "#f8fafc" : "#ef4444"; g.fillRect(i * 16, 0, 16, 8); }
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

let canopyMaterial;

export function buildParachute() {
  canopyMaterial ??= new THREE.MeshLambertMaterial({ map: stripes(), side: THREE.DoubleSide });
  const parachute = new THREE.Group();
  const canopy = new THREE.Mesh(new THREE.SphereGeometry(1.1, 16, 6, 0, Math.PI * 2, 0, Math.PI * 0.42), canopyMaterial);
  canopy.position.y = 1.35;
  const rim = 1.1 * Math.sin(Math.PI * 0.42), rimY = 1.35 + 1.1 * Math.cos(Math.PI * 0.42);
  const points = [];
  for (let i = 0; i < 8; i++) {
    const a = i / 8 * Math.PI * 2;
    points.push(new THREE.Vector3(0, 0.55, 0), new THREE.Vector3(Math.cos(a) * rim, rimY, Math.sin(a) * rim));
  }
  const lines = new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(points), new THREE.LineBasicMaterial({ color: "#e5e7eb" }));
  parachute.add(canopy, lines);
  return parachute;
}

// The head flashes white and swells a little, as a creeper about to blow.
export function animateBomb(holder, now) {
  const flash = Math.max(0, Math.sin(now / 160));
  holder.userData.bomb.scale.setScalar(1 + 0.06 * flash);
  for (const m of new Set(headMaterials)) m.emissive.setScalar(0.55 * flash * flash);
}

// ── effects ──────────────────────────────────────────────────────────────────────────────────────

const effects = [];
const cube = new THREE.BoxGeometry(1, 1, 1);
const puff = new THREE.IcosahedronGeometry(1, 1);

function particle(scene, geometry, color, position, velocity, life, size, grow = 0, opacity = 1, gravity = 0) {
  const material = new THREE.MeshBasicMaterial({ color, transparent: true, opacity, depthWrite: false });
  const mesh = new THREE.Mesh(geometry, material);
  mesh.position.copy(position);
  mesh.scale.setScalar(size);
  scene.add(mesh);
  effects.push({ mesh, velocity, life, age: 0, size, grow, opacity, gravity, scene });
}

const rand = (a, b) => a + Math.random() * (b - a);

function around(spread) {
  const v = new THREE.Vector3(rand(-1, 1), rand(-1, 1), rand(-1, 1));
  return v.normalize().multiplyScalar(rand(0.2, 1) * spread);
}

export function spawnExplosion(scene, at) {
  const light = new THREE.PointLight("#ffb347", 60, 16, 2);
  light.position.copy(at);
  scene.add(light);
  effects.push({ light, life: 0.35, age: 0, scene });
  particle(scene, puff, "#fff4c2", at, new THREE.Vector3(), 0.18, 0.6, 20, 0.95);
  for (let i = 0; i < 26; i++) particle(scene, puff, ["#ffd166", "#ff9f1c", "#f25c05"][i % 3], at.clone().add(around(0.6)), around(9), rand(0.25, 0.5), rand(0.35, 0.7), 1.2, 0.95);
  for (let i = 0; i < 18; i++) particle(scene, cube, ["#4b3621", "#5c5c5c", "#2f2f2f"][i % 3], at.clone(), around(12).add(new THREE.Vector3(0, 6, 0)), rand(0.6, 1.1), rand(0.1, 0.22), 0, 1, 26);
  for (let i = 0; i < 16; i++) particle(scene, puff, ["#3f3f46", "#52525b", "#71717a"][i % 3], at.clone().add(around(1.2)), around(2.2).add(new THREE.Vector3(0, 1.8, 0)), rand(1.2, 2), rand(0.5, 0.9), 1.1, 0.7);
}

// A bomb that is not needed any more: a small light-grey puff that rises and thins out. Nothing is hurt.
export function spawnSmoke(scene, at) {
  for (let i = 0; i < 10; i++)
    particle(scene, puff, ["#e5e7eb", "#d4d4d8", "#f4f4f5"][i % 3], at.clone().add(around(0.25)), around(0.6).add(new THREE.Vector3(0, 1.1, 0)), rand(0.9, 1.4), rand(0.12, 0.22), 0.5, 0.75);
}

export function tickEffects(dt) {
  for (let i = effects.length - 1; i >= 0; i--) {
    const e = effects[i];
    e.age += dt;
    const t = Math.min(1, e.age / e.life);
    if (e.light) e.light.intensity = 60 * (1 - t);
    else {
      e.velocity.y -= e.gravity * dt;
      e.velocity.multiplyScalar(e.gravity ? 1 : Math.max(0, 1 - 3 * dt));
      e.mesh.position.addScaledVector(e.velocity, dt);
      e.mesh.scale.setScalar(e.size * (1 + e.grow * t));
      e.mesh.material.opacity = e.opacity * (1 - t) * (1 - t);
    }
    if (t < 1) continue;
    e.scene.remove(e.light ?? e.mesh);
    e.mesh?.material.dispose();
    effects.splice(i, 1);
  }
}
