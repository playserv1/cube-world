# Cube World — the Minecraft numbers

Every mechanic below follows the [Minecraft Wiki](https://minecraft.wiki). The constants live in
`CubeWorld.Server/Spec.cs` (server) and `web/spec.js` (client); the tests in `CubeWorld.Tests` and
`web/physics.test.js` check the numbers that can be checked.

## Time

| What | Value | Wiki |
|---|---|---|
| Game tick | 20 per second (50 ms) | [Tick](https://minecraft.wiki/w/Tick) |

## Player and movement

| What | Value | Wiki |
|---|---|---|
| Hitbox | 0.6 × 1.8 blocks, 1.5 tall sneaking | [Player](https://minecraft.wiki/w/Player) |
| Eye height | 1.62, 1.27 sneaking | [Player](https://minecraft.wiki/w/Player) |
| Walking | 4.317 m/s (acceleration 0.1/tick, keyboard input × 0.98, friction 0.91 × slipperiness 0.6) | [Walking](https://minecraft.wiki/w/Walking), [Entity § Motion](https://minecraft.wiki/w/Entity) |
| Sprinting | 5.612 m/s (+30 %), Shift (see the deviations), needs forward input, FOV × 1.15 | [Sprinting](https://minecraft.wiki/w/Sprinting) |
| Sneaking | 1.295 m/s (× 0.3), Ctrl (see the deviations), the player cannot walk off an edge, the name tag hides | [Sneaking](https://minecraft.wiki/w/Sneaking) |
| Air control | acceleration 0.02/tick, friction 0.91 | [Entity § Motion](https://minecraft.wiki/w/Entity) |
| Gravity | 0.08 blocks/tick², vertical drag 0.98, terminal velocity 3.92 blocks/tick (78.4 m/s) | [Entity § Motion](https://minecraft.wiki/w/Entity) |
| Jump | 0.42 blocks/tick up, 1.2522 blocks high, +0.2 forward when sprinting, 10 ticks between jumps while the key is held | [Jumping](https://minecraft.wiki/w/Jumping) |
| Step height | 0.6: a full block is never stepped over, it is jumped | [Player](https://minecraft.wiki/w/Player) |
| Diagonal input | normalised, no faster than straight | [Walking](https://minecraft.wiki/w/Walking) |
| Field of view | 70° | [Options](https://minecraft.wiki/w/Options) |
| Entities push each other | 0.05 blocks/tick apart when hitboxes overlap | [Entity](https://minecraft.wiki/w/Entity) |

## Health and damage

| What | Value | Wiki |
|---|---|---|
| Health | 20 (ten hearts), shown as hearts above the hotbar | [Health](https://minecraft.wiki/w/Health) |
| Fall damage | 1 per block fallen beyond the third, from the highest point since last on the ground | [Damage § Fall damage](https://minecraft.wiki/w/Damage) |
| Hitting with an empty hand | 1 damage, attack speed 4 (recharges in 5 ticks), a hit before that deals 20 % + 80 % × charge² | [Damage § Attack cooldown](https://minecraft.wiki/w/Damage), [Attribute](https://minecraft.wiki/w/Attribute) |
| Knockback | motion halved, 0.4 away from the attacker, +0.5 when sprinting, a grounded victim is lifted by up to 0.4 | [Knockback](https://minecraft.wiki/w/Knockback) |
| Invulnerability after a hit | 10 ticks | [Damage § Immunity](https://minecraft.wiki/w/Damage) |
| Natural regeneration | 1 every 80 ticks (as with food at 18 or more) | [Health § Regeneration](https://minecraft.wiki/w/Health) |
| Hurt | the victim flashes red, the camera vignettes red | [Damage](https://minecraft.wiki/w/Damage) |
| Death | a death screen with a Respawn button; the player returns to the spawn with full health | [Death](https://minecraft.wiki/w/Death) |
| Reach | 4.5 blocks for blocks, 3 for players, whichever is nearer wins; the server allows 1 more for latency | [Attribute § Block interaction range](https://minecraft.wiki/w/Attribute) |

## Blocks

| What | Value | Wiki |
|---|---|---|
| Block | a 1 m cube, 16 × 16 pixel textures, grass has a green top and dirt bottom, a log has ringed ends | [Block](https://minecraft.wiki/w/Block), [Grass Block](https://minecraft.wiki/w/Grass_Block), [Log](https://minecraft.wiki/w/Log) |
| Face lighting | top 1.0, north and south 0.8, east and west 0.6, bottom 0.5 | [Light](https://minecraft.wiki/w/Light) |
| Placing | right click puts the block against the clicked face; never inside a player | [Controls](https://minecraft.wiki/w/Controls), [Block](https://minecraft.wiki/w/Block) |
| Breaking | hold left click; damage per tick is 1/(hardness × 30), or 1/(hardness × 100) when the block needs a tool the player has not got; ten crack stages; five ticks before the next block | [Breaking § Speed](https://minecraft.wiki/w/Breaking) |
| Hardness | grass block 0.6, dirt 0.5, sand 0.5, stone 1.5 (pickaxe), oak log 2, bricks 2 (pickaxe), glass 0.3, block of gold 3 (pickaxe), bedrock unbreakable | [Breaking § Blocks by hardness](https://minecraft.wiki/w/Breaking) |
| Drops by hand | grass block → dirt, dirt, sand and log → themselves; stone, bricks, gold and glass → nothing | [Grass Block](https://minecraft.wiki/w/Grass_Block), [Stone](https://minecraft.wiki/w/Stone), [Glass](https://minecraft.wiki/w/Glass) |
| Gravity blocks | sand falls when nothing is under it, at 0.04 blocks/tick² | [Sand](https://minecraft.wiki/w/Sand), [Falling Block](https://minecraft.wiki/w/Falling_Block) |
| Glass | transparent, faces between two glass blocks are not drawn | [Glass](https://minecraft.wiki/w/Glass) |
| Terrain | Superflat "Classic Flat": bedrock, dirt, dirt, grass block; the world is 72 × 24 blocks and 68 high (z −4 to 63) | [Superflat](https://minecraft.wiki/w/Superflat) |
| Trees | oaks, four per region: five logs, two 5 × 5 leaf layers without corners, a 3 × 3 layer and a cross on top; logs and leaves can be cut | [Tree](https://minecraft.wiki/w/Tree), [Oak](https://minecraft.wiki/w/Oak) |
| Leaves | hardness 0.2, transparent, drop nothing by hand | [Leaves](https://minecraft.wiki/w/Leaves) |
| Grass colour | tinted per region as Minecraft tints grass per biome: red region warm, blue region cool, green plain | [Color § Biome colors](https://minecraft.wiki/w/Color) |
| Hotbar | 9 slots, keys 1–9 and the mouse wheel, stacks of 64 | [Hotbar](https://minecraft.wiki/w/Hotbar), [Item § Stacking](https://minecraft.wiki/w/Item) |
| Block outline | a thin black box around the targeted block | [Block](https://minecraft.wiki/w/Block) |

## Player model

| What | Value | Wiki |
|---|---|---|
| Model | head 8 × 8 × 8, body 8 × 12 × 4, arms and legs 4 × 12 × 4 pixels, 16 pixels to the block, drawn at 15/16 | [Player § Model](https://minecraft.wiki/w/Player) |
| Skin | 64 × 64 in the standard layout (head, body, right arm, right leg, left arm, left leg), painted per player from their id | [Skin](https://minecraft.wiki/w/Skin) |
| Animation | arms and legs swing in opposition with the distance walked, the head follows the pitch, sneaking bends the body forward | [Player](https://minecraft.wiki/w/Player) |
| Name tag | above the head, hidden while sneaking | [Sneaking](https://minecraft.wiki/w/Sneaking) |

## Server authority

As Minecraft's server, the game server decides. The client simulates its own movement and reports it;
the server clamps positions to the world, works out fall damage from them, checks reach and the
face a block is placed against, times every dig on its own 20 Hz tick, and deals damage, knockback
and death. Every world change goes through platform data so the other servers see it.

## Where this world differs, on purpose

- The keys for sprinting and sneaking are swapped on request: Shift sprints, Ctrl sneaks (Minecraft: Ctrl sprints, Shift sneaks).
- Players hold no tools, so stone, bricks and gold take the by-hand time and drop nothing.
- No hunger: regeneration runs as if food were full. No day and night, mobs, crafting, water or redstone.
- A hit on a player another server hosts travels through platform data (WorldHit), so anyone within reach can be hit, whichever region they stand in.
- The world is a small flat slab with a ceiling at 64, and its floor colours mark the three servers.
- Every player starts with 64 of each block and a cloud function adds one of each per minute.
