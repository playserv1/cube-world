// The oaks as the servers generate them (Spec.BuildTrees, web/voxels.js buildTreeMap), keyed in the server's
// coordinates "x:y:z". web/voxels.js cannot be imported here: it pulls in three.js.

export function buildTreeMap(trees) {
  const map = new Map();
  const k = (x, y, z) => `${x}:${y}:${z}`;
  for (const { x: tx, y: ty } of trees) {
    for (let dz = 0; dz < 5; dz++) map.set(k(tx, ty, dz), "wood");
    for (let dx = -2; dx <= 2; dx++)
      for (let dy = -2; dy <= 2; dy++) {
        const corner = Math.abs(dx) === 2 && Math.abs(dy) === 2, trunk = dx === 0 && dy === 0;
        for (let dz = 3; dz <= 4; dz++) if (!corner && !trunk && !map.has(k(tx + dx, ty + dy, dz))) map.set(k(tx + dx, ty + dy, dz), "leaves");
        if (Math.abs(dx) <= 1 && Math.abs(dy) <= 1 && !map.has(k(tx + dx, ty + dy, 5))) map.set(k(tx + dx, ty + dy, 5), "leaves");
        if (Math.abs(dx) + Math.abs(dy) <= 1 && !map.has(k(tx + dx, ty + dy, 6))) map.set(k(tx + dx, ty + dy, 6), "leaves");
      }
  }
  return map;
}
