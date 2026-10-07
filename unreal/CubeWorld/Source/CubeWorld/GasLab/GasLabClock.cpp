#include "GasLab/GasLabClock.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"

namespace GasLabClock
{
	int64 Epoch()
	{
		static const int64 Value = []()
		{
			int64 Given = 0;
			if (FParse::Value(FCommandLine::Get(), TEXT("-gasepoch="), Given) && Given > 0) return Given;
			const FDateTime Today = FDateTime::UtcNow().GetDate();
			return Today.ToUnixTimestamp();
		}();
		return Value;
	}

	double Now()
	{
		const FDateTime UtcNow = FDateTime::UtcNow();
		return double(UtcNow.ToUnixTimestamp() - Epoch()) + UtcNow.GetMillisecond() / 1000.0
			+ double(UtcNow.GetTicks() % ETimespan::TicksPerMillisecond) / ETimespan::TicksPerSecond;
	}

	bool IsShared()
	{
		static const bool bShared = !FParse::Param(FCommandLine::Get(), TEXT("gasnoclock"));
		return bShared;
	}
}
