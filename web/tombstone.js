// A tombstone where a player died, standing until they respawn: a grey headstone with a rounded top on a
// low base, "R.I.P." and the player's name carved on its face.

import * as THREE from "three";

const stone = new THREE.MeshLambertMaterial({ color: "#8d9096" });
const base = new THREE.MeshLambertMaterial({ color: "#6b6e73" });

function face(name) {
  const canvas = document.createElement("canvas");
  canvas.width = 128; canvas.height = 160;
  const g = canvas.getContext("2d");
  g.fillStyle = "#8d9096"; g.fillRect(0, 0, 128, 160);
  for (let i = 0; i < 90; i++) { g.fillStyle = i % 2 ? "#7f8288" : "#9a9da3"; g.fillRect((i * 37) % 128, (i * 53) % 160, 4, 4); }
  g.fillStyle = "#3f4145"; g.textAlign = "center";
  g.font = "bold 26px system-ui"; g.fillText("R.I.P.", 64, 52);
  let size = 22;
  do { g.font = `bold ${size}px system-ui`; size -= 2; } while (g.measureText(name).width > 112 && size > 10);
  g.fillText(name, 64, 100);
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  return new THREE.MeshLambertMaterial({ map: texture });
}

export function buildTombstone(name) {
  const tomb = new THREE.Group();
  const w = 0.62, h = 0.78, d = 0.16;
  const front = face(name);
  const slab = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), [stone, stone, stone, stone, front, front]);
  slab.position.y = 0.12 + h / 2;
  const top = new THREE.Mesh(new THREE.CylinderGeometry(w / 2, w / 2, d, 20, 1, false, -Math.PI / 2, Math.PI), stone);
  top.rotation.x = -Math.PI / 2;
  top.position.y = 0.12 + h;
  const plinth = new THREE.Mesh(new THREE.BoxGeometry(w + 0.24, 0.12, d + 0.3), base);
  plinth.position.y = 0.06;
  tomb.add(slab, top, plinth);
  return tomb;
}
