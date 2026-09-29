// What every client in a room sees of the server, replicated over Iris: which server and region this is, the
// live regions of the world, everyone's pose and health (players here and on the other servers), and the
// events that happen to everyone at once: blocks changing, digs, hits, deaths, bombs. One actor per server,
// always relevant. What only one player gets (the welcome, the world, the inventory) goes through their pawn.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "CubeWorldState.generated.h"

class UCubeWorldGameInstance;

USTRUCT()
struct FCubeRegionRep
{
	GENERATED_BODY()
	UPROPERTY() int32 Region = -1;
	UPROPERTY() FString Room;
	UPROPERTY() FString Color;
	UPROPERTY() FString Server;
	UPROPERTY() FString Slug;
};

USTRUCT()
struct FCubePresenceRep
{
	GENERATED_BODY()
	UPROPERTY() FString Id;
	UPROPERTY() FString Name;
	UPROPERTY() FString Server;
	UPROPERTY() FString Color;
	UPROPERTY() float X = 0;
	UPROPERTY() float Y = 0;
	UPROPERTY() float Z = 0;
	UPROPERTY() float Yaw = 0;
	UPROPERTY() float Pitch = 0;
	UPROPERTY() float Health = 20;
	UPROPERTY() bool bSneaking = false;
	UPROPERTY() bool bSprinting = false;
};

/** A bomb as the server tells it: the WorldBomb record, how long ago it entered its state, and the height of a free one now. */
USTRUCT()
struct FCubeBombRep
{
	GENERATED_BODY()
	UPROPERTY() FString Id;
	UPROPERTY() FString State;
	UPROPERTY() FString Holder;
	UPROPERTY() float X = 0;
	UPROPERTY() float Y = 0;
	UPROPERTY() float Z = 0;
	UPROPERTY() float VX = 0;
	UPROPERTY() float VY = 0;
	UPROPERTY() float VZ = 0;
	UPROPERTY() int32 AgeMs = 0;
	UPROPERTY() bool bHasHeight = false;
	UPROPERTY() float Height = 0;
};

/** One block on the wire: its column and height, and its kind as an index into CubeSpec::Blocks(). */
USTRUCT()
struct FCubeCellRep
{
	GENERATED_BODY()
	UPROPERTY() int16 X = 0;
	UPROPERTY() int16 Y = 0;
	UPROPERTY() int16 Z = 0;
	UPROPERTY() uint8 Kind = 0;
};

USTRUCT()
struct FCubeChangeRep
{
	GENERATED_BODY()
	UPROPERTY() int16 X = 0;
	UPROPERTY() int16 Y = 0;
	UPROPERTY() int16 Z = 0;
	UPROPERTY() uint8 Kind = 0;
	/** The server the change was made on. */
	UPROPERTY() FString On;
};

USTRUCT()
struct FCubeFallRep
{
	GENERATED_BODY()
	UPROPERTY() uint8 Kind = 0;
	UPROPERTY() int16 X = 0;
	UPROPERTY() int16 Y = 0;
	UPROPERTY() int16 FromZ = 0;
	UPROPERTY() int16 ToZ = 0;
};

USTRUCT()
struct FCubeStackRep
{
	GENERATED_BODY()
	UPROPERTY() uint8 Kind = 0;
	UPROPERTY() int32 Count = 0;
};

UCLASS()
class CUBEWORLD_API ACubeWorldState : public AInfo
{
	GENERATED_BODY()

public:
	ACubeWorldState();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;

	UPROPERTY(ReplicatedUsing=OnRep_Info) FString Server;
	UPROPERTY(ReplicatedUsing=OnRep_Info) FString Color;
	UPROPERTY(ReplicatedUsing=OnRep_Info) int32 Region = -1;
	UPROPERTY(ReplicatedUsing=OnRep_Regions) TArray<FCubeRegionRep> Regions;
	UPROPERTY(ReplicatedUsing=OnRep_Players) TArray<FCubePresenceRep> Players;

	/** Blocks that changed together, and the sand that fell. bRemote: the change was made on another server. */
	UFUNCTION(NetMulticast, Reliable) void MulticastCubes(const TArray<FCubeChangeRep>& Changes, const TArray<FCubeFallRep>& Falls, bool bRemote);
	/** A dig's crack stage 0 to 9; -1 clears it. */
	UFUNCTION(NetMulticast, Reliable) void MulticastDig(const FString& PlayerId, int32 X, int32 Y, int32 Z, int32 Stage);
	UFUNCTION(NetMulticast, Reliable) void MulticastHurt(const FString& PlayerId, float Health, float KX, float KY, float Strength, const FString& By);
	UFUNCTION(NetMulticast, Reliable) void MulticastDeath(const FString& PlayerId, const FString& By);
	UFUNCTION(NetMulticast, Reliable) void MulticastBomb(const FCubeBombRep& Bomb);

private:
	UFUNCTION() void OnRep_Info();
	UFUNCTION() void OnRep_Regions();
	UFUNCTION() void OnRep_Players();
	UCubeWorldGameInstance* Game() const;
};
