// The canonical numbers, all from the Minecraft Wiki (SPEC.md names the page for each one).
// Mirrors CubeWorld.Server/Spec.cs. Distances are in blocks, velocities in blocks per tick.

export const TPS = 20;
export const TICK_MS = 1000 / TPS;

// Entity motion: gravity 0.08 per tick, vertical drag 0.98, jump 0.42 up (1.2522 blocks high),
// an extra 0.2 forward when sprint-jumping, ten ticks before the next jump while the key is held.
export const GRAVITY = 0.08;
export const VERTICAL_DRAG = 0.98;
export const JUMP_VELOCITY = 0.42;
export const SPRINT_JUMP_BOOST = 0.2;
export const JUMP_DELAY_TICKS = 10;

// Horizontal motion: acceleration 0.1 per tick on the ground (0.02 in the air), sprinting +30 %, sneaking ×0.3,
// keyboard input scaled by 0.98, friction 0.91 × block slipperiness 0.6 on the ground and 0.91 in the air.
// This gives walking 4.317 m/s, sprinting 5.612 m/s and sneaking 1.295 m/s.
export const WALK_ACCELERATION = 0.1;
export const AIR_ACCELERATION = 0.02;
export const SPRINT_MULTIPLIER = 1.3;
export const SNEAK_MULTIPLIER = 0.3;
export const INPUT_SCALE = 0.98;
export const GROUND_FRICTION = 0.91 * 0.6;
export const AIR_FRICTION = 0.91;
export const MIN_VELOCITY = 0.003;

// Player hitbox 0.6 × 1.8 (1.5 sneaking), eyes at 1.62 (1.27 sneaking), step height 0.6.
export const WIDTH = 0.6;
export const HEIGHT = 1.8;
export const SNEAK_HEIGHT = 1.5;
export const EYE_HEIGHT = 1.62;
export const SNEAK_EYE_HEIGHT = 1.27;
export const STEP_HEIGHT = 0.6;

// Reach: blocks 4.5, entities 3.
export const BLOCK_REACH = 4.5;
export const ENTITY_REACH = 3;

// Health 20 (ten hearts); knockback lifts a grounded victim by at most 0.4; entities push each other by 0.05 a tick.
export const MAX_HEALTH = 20;
export const KNOCKBACK_LIFT = 0.4;
export const PUSH = 0.05;
export const HURT_TICKS = 10;

// Five ticks between finishing one block and starting the next.
export const DIG_COOLDOWN_TICKS = 5;

// Field of view 70°, ×1.15 while sprinting.
export const FOV = 70;
export const SPRINT_FOV = 1.15;

// Rendering: a texture is 16 × 16 pixels, the player model is drawn at 15/16 of its pixel size.
export const TEXTURE_SIZE = 16;
export const MODEL_SCALE = 0.9375;

// Bombs (CubeWorld.Server/Spec.cs): thrown at 1 block a tick, drag 0.99, gravity 0.05 (a thrown potion's);
// a creeper's power of 3; a parachute comes down at 0.1 a tick from 32 up; picked up within 1 block of the hitbox.
export const THROW_SPEED = 1.0;
export const PROJECTILE_DRAG = 0.99;
export const PROJECTILE_GRAVITY = 0.05;
export const BOMB_POWER = 3;
// The blast hurts players within 3 blocks, half of Minecraft's 2 × power. It breaks blocks with a power
// of 1, not 3: the block under it and one around, a 3 × 3 patch of the top layer on flat ground.
export const BLAST_REACH = 3;
export const CRATER_POWER = 1;
// Every block a bomb can break takes it as dirt does, so it breaks at the first go; only bedrock stands.
export const CRATER_RESISTANCE = 0.5;
export const OWNER_IMMUNITY_TICKS = 4;
export const PARACHUTE_SPEED = 0.1;
export const DROP_HEIGHT = 32;
export const PICKUP_REACH = 1;
export const PICKUP_REACH_UP = 0.5;
