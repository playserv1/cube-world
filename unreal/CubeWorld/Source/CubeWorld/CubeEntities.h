// The platform tables the servers share the world through, as PlayServ SDK entity classes. One class per table,
// named after it (UWorldCube -> WorldCube), one UPROPERTY per field, spelled as the table spells it. The same
// tables the C# server uses (CubeWorld.Server/World.cs, Bomb.cs); WorldCube carries one extra field here, `at`,
// the time of the last change, which is how a server asks the platform for the blocks changed since it last looked.
// Storage keeps x, y on the ground and z up.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CubeEntities.generated.h"

/** A block that overrides the generated terrain. Kind "air" is a generated block someone dug out. */
UCLASS(PlayServEntity)
class UWorldCube : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() FString key;
	UPROPERTY() int32 x = 0;
	UPROPERTY() int32 y = 0;
	UPROPERTY() int32 z = 0;
	UPROPERTY() FString kind;
	UPROPERTY() FString placed_by;
	UPROPERTY() FString placed_on;
	/** Unix milliseconds of the last change. */
	UPROPERTY() int64 at = 0;
};

/** What a player carries, per block kind: `stacks` is a JSON object of kind -> count. */
UCLASS(PlayServEntity)
class UCubeInventory : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() FString player_id;
	UPROPERTY() int32 cubes = 0;
	UPROPERTY() FString stacks;
};

/** Where a player is and how they are, written by their server five times a second and read by the others. */
UCLASS(PlayServEntity)
class UWorldPresence : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() FString player_id;
	UPROPERTY() FString name;
	UPROPERTY() FString server;
	UPROPERTY() FString color;
	UPROPERTY() double x = 0;
	UPROPERTY() double y = 0;
	UPROPERTY() double z = 0;
	UPROPERTY() double yaw = 0;
	UPROPERTY() double pitch = 0;
	UPROPERTY() double health = 20;
	UPROPERTY() int32 sneaking = 0;
	UPROPERTY() int32 sprinting = 0;
	UPROPERTY() int64 seen_at = 0;
};

/** A hit on a player another server hosts: written by the attacker's server, applied and deleted by the victim's. */
UCLASS(PlayServEntity)
class UWorldHit : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() FString hit_id;
	UPROPERTY() FString victim;
	UPROPERTY() FString attacker;
	UPROPERTY() double damage = 0;
	UPROPERTY() double kx = 0;
	UPROPERTY() double ky = 0;
	UPROPERTY() double strength = 0;
	UPROPERTY() int64 at = 0;
};

/** Which server holds which region, refreshed every five seconds; a claim older than 30 s is free. */
UCLASS(PlayServEntity)
class UWorldRegion : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() FString region;
	UPROPERTY() FString server;
	UPROPERTY() FString color;
	UPROPERTY() FString room;
	/** The room type the room is registered under: the C# servers' or the Unreal servers'. */
	UPROPERTY() FString slug;
	UPROPERTY() int64 seen_at = 0;
};

/**
 * A bomb. The drop function writes it "free" high above the world; it comes down under a parachute, a player picks it
 * up ("held"), throws it ("flying") and it explodes where it lands ("exploded"); a surplus one goes up in smoke
 * ("fizzled"). Every state change is a new row; the row furthest on is the bomb.
 */
UCLASS(PlayServEntity)
class UWorldBomb : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() FString bomb_id;
	UPROPERTY() FString state;
	UPROPERTY() FString holder;
	UPROPERTY() double x = 0;
	UPROPERTY() double y = 0;
	UPROPERTY() double z = 0;
	UPROPERTY() double vx = 0;
	UPROPERTY() double vy = 0;
	UPROPERTY() double vz = 0;
	UPROPERTY() int64 dropped_at = 0;
	UPROPERTY() int64 at = 0;
};
