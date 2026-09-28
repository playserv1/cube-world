#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "Core/PlayServTypes.h"
#include "PlayServSubsystem.generated.h"

class UPlayServAuth;
class UPlayServData;
class UPlayServCode;
class UPlayServRooms;

/**
 * The PlayServ SDK's engine subsystem: one per process, alive across PIE sessions. Most games use the PlayServ
 * namespace (PlayServ.h) instead of calling it.
 */
UCLASS()
class PLAYSERVRUNTIME_API UPlayServSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The subsystem; null before the engine has started it and after shutdown. */
	UFUNCTION(BlueprintPure, Category="PlayServ")
	static UPlayServSubsystem* Get();

	/** Sign-in and sessions. */
	UFUNCTION(BlueprintPure, Category="PlayServ")
	UPlayServAuth* GetAuth() const;

	/** Entities: loading, saving, queries and live updates. */
	UFUNCTION(BlueprintPure, Category="PlayServ")
	UPlayServData* GetData() const;

	/** Cloud-function calls. */
	UFUNCTION(BlueprintPure, Category="PlayServ")
	UPlayServCode* GetCode() const;

	/** Rooms: hosting them on a dedicated server, and joining them from a client. */
	UFUNCTION(BlueprintPure, Category="PlayServ")
	UPlayServRooms* GetRooms() const;

private:
	UPROPERTY()
	TObjectPtr<UPlayServAuth> Auth;

	UPROPERTY()
	TObjectPtr<UPlayServData> Data;

	UPROPERTY()
	TObjectPtr<UPlayServCode> Code;

	UPROPERTY()
	TObjectPtr<UPlayServRooms> Rooms;

	TSharedPtr<class FPlayServHttp> Http;
};
