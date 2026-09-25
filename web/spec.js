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
