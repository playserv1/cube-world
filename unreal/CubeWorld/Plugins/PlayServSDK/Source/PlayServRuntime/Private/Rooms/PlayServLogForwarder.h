#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDevice.h"
#include "Rooms/PlayServRoomsTypes.h"

/**
 * Catches this process's own UE_LOG lines for UPlayServRooms::ForwardLogs. Serialize runs on whichever thread logs; the
 * lines wait here until Drain hands them to the game thread. A line logged while Drain is sending on that thread (the
 * send logging about itself) is not caught again.
 */
class PLAYSERVRUNTIME_API FPlayServLogForwarder : public FOutputDevice
{
public:
	struct FLine
	{
		FString Text;
		EPlayServLogLevel Level = EPlayServLogLevel::Info;
	};

	/** How many caught lines wait at most for the next Drain; older ones are counted as dropped. */
	static constexpr int32 MaxWaitingLines = 1000;

	explicit FPlayServLogForwarder(const FPlayServLogForwarding& InRules, TFunction<double()> InClock);

	void SetRules(const FPlayServLogForwarding& InRules);

	/** The line with every room ticket in it (`rsv=<token>`, as a travel URL carries it) blanked out. */
	static FString Redacted(const FString& Line);

	/** The level a line of Category at Verbosity goes out with, or false when the rules leave it out. */
	static bool Wanted(const FPlayServLogForwarding& Rules, const FName& Category, ELogVerbosity::Type Verbosity, EPlayServLogLevel& OutLevel);

	/** Hands every line caught since the last call to Send, in order, then one line for any that were dropped. */
	void Drain(TFunctionRef<void(const FString& /*Text*/, EPlayServLogLevel /*Level*/)> Send);

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

private:
	FCriticalSection Lock;
	FPlayServLogForwarding Rules;
	TFunction<double()> Clock;
	TArray<FLine> Waiting;
	/** Lines left out because more than the rules allow came in ten seconds, and because too many waited. */
	int32 OverRate = 0;
	int32 Overflow = 0;
	double WindowStartedAt = -1.0;
	int32 LinesInWindow = 0;
};
