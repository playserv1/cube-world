// The border layer of the GAS lab: everything a server does about its neighbours, under GAS rather than in it.
//
// The lab's world is a 2 x 2 grid of regions, each 24 m square, one server per region (-region=, -peers= as in
// Cube World's offline mode); the four meet at (2400, 2400). Each server owns the players standing in its region and
// the creepers it threw. Over a server-to-server link (GasLabLink.h) it:
//   - sends its players near a border to that neighbour, 20 times a second; the neighbour keeps a ghost of each
//     (a character and player state of its own whose ability system mirrors health, tags and looping cues), which
//     its clients see like any player, and which game code can target like any player;
//   - forwards an effect applied to a ghost to the server that owns the player, which builds an equal spec and
//     applies it there (GasLabWire.h), and sends the credit (damage, kills) back to the instigator's server;
//   - mirrors its creepers to the neighbours, and in area mode lets the creeper carry its effects to them;
//   - hands a player who walks into a neighbour's region over to that server: the whole GAS state goes ahead of the
//     player (attribute bases, active effects with time left, stacks and period phase); a client is told to travel,
//     a bot moves server to server on its own; effects that meet the player on the way are sent after it.
// Every event is logged as a "GASLAB ..." line with shared-clock times, for Scripts/AnalyzeGasLab.py.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GameplayTagContainer.h"
#include "GasLab/GasLabLink.h"
#include "GasLab/GasLabWire.h"
#include "GasLabBorder.generated.h"

class AGasLabCharacter;
class AGasLabCreeper;
class UGasLabAbilitySystemComponent;
struct FGameplayEffectSpec;

UENUM()
enum class EGasLabMode : uint8
{
	/** The owner applies the blast to every ability system in range it can see; ghosts forward to their owners. */
	Forward,
	/** The owner applies it to its own players; the creeper carries the effects to each neighbour, which applies them to its own. */
	Area,
};

UCLASS()
class CUBEWORLD_API UGasLabBorder : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	static UGasLabBorder* Get(const UObject* WorldContext);
	/** -gaslab on the command line: this process plays the lab. */
	static bool IsLab();
	/** Granted while a player is handed to another server; abilities refuse to start under it. */
	static FGameplayTag HandoffTag();

	static constexpr float RegionSize = 2400.f;
	static constexpr int32 Columns = 2;
	static constexpr int32 Rows = 2;
	static int32 RegionAt(const FVector& P);
	static FBox2D RegionBox(int32 Region);
	static FVector RegionCenter(int32 Region);
	static float DistanceToRegion(const FVector& P, int32 Region);
	static FLinearColor RegionColor(int32 Region);

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return Link.IsValid(); }

	int32 GetRegion() const { return Region; }
	/** Every neighbour's link is up (scenarios start their clock from here). */
	bool AllConnected() const;
	/** The lowest region picks the scenario's start (shared clock) once every link is up and tells the others. */
	double GetScenarioStart() const { return ScenarioStart; }
	void AnnounceStart(double At);
	EGasLabMode GetMode() const { return Mode; }
	/** "host:port" of the server holding Region, for a client's travel. */
	FString AddressOf(int32 OtherRegion) const;

	void ForwardEffect(UGasLabAbilitySystemComponent& Target, const FGameplayEffectSpec& Spec);
	void NoteDamage(UGasLabAbilitySystemComponent& Target, const FGameplayEffectSpec& Spec, float Lost, bool bKilled);
	void ShareCreeper(const AGasLabCreeper& Creeper);

	/** Starts moving a player or bot of this server to the server of ToRegion. */
	void HandOff(AGasLabCharacter& Who, int32 ToRegion);
	/** The state a neighbour sent ahead of a client that is coming here, if it has come. */
	bool TakeArrival(const FString& Id, FGasLabWirePlayer& Out);
	bool HasArrival(const FString& Id) const { return Arrivals.Contains(Id); }
	/** Restores State into ASC and applies what was sent after the player meanwhile; logs how it went. */
	void Restore(UGasLabAbilitySystemComponent& ASC, const FGasLabWirePlayer& State);

	UGasLabAbilitySystemComponent* FindLocal(const FString& Id) const;
	AGasLabCharacter* FindCharacter(const FString& Id, bool bGhost) const;

private:
	void Send(int32 To, EGasLabMsg Type, TFunctionRef<void(FArchive&)> Body);
	void Hear(int32 From, const TArray<uint8>& Bytes);
	void SendGhosts();
	void CheckCrossings();
	void ExpireGhosts();
	void HearGhosts(int32 From, TArray<FGasLabWireGhost>& Ghosts);
	void HearCreeper(int32 From, FGasLabWireCreeper& Creeper);
	void HearEffect(int32 From, const FString& TargetId, FGasLabWireSpec& Spec, double SentAt);
	void HearCredit(int32 From, FGasLabWireCredit& Credit);
	void HearHandoff(int32 From, FGasLabWirePlayer& Player, int32 Bytes);
	void HearAck(int32 From, const FString& Id, double SentAt);
	void DropGhost(const FString& Id);
	void LogStats();

	int32 Region = -1;
	EGasLabMode Mode = EGasLabMode::Forward;
	float GhostBand = 1500.f;
	TArray<FGasLabPeer> Peers;
	TUniquePtr<IGasLabLink> Link;

	TMap<FString, double> GhostHeard;
	/** Where a player who left went, so late effects and credit follow them. */
	TMap<FString, int32> Departed;
	/** Bots handed off, waiting for the receiving server's ack before they go here. */
	TMap<FString, double> BotsLeaving;
	TMap<FString, FGasLabWirePlayer> Arrivals;
	TMap<FString, double> ArrivalHeard;
	/** Effects that came for a player whose state is here but who is not yet. */
	TMap<FString, TArray<FGasLabWireSpec>> EffectsWaiting;

	double NextGhosts = 0, NextProbe = 0, NextStats = 0;
	double ScenarioStart = -1.0;
	uint64 StatBytesOut = 0, StatBytesIn = 0, StatFramesOut = 0, StatFramesIn = 0;
	double StatAt = 0;
};
