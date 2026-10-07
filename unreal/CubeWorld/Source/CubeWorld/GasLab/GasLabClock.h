// One clock for every server and client of the lab: seconds since a cluster epoch.
//
// GAS stamps active effects with the game state's server time and keeps those stamps as 32-bit floats, so the epoch
// must stay recent: a float holds 8 ms at 18 hours and 125 ms at 12 days, but 128 s at Unix time. The epoch comes
// from -gasepoch=<unix seconds> (the run script passes its own start), else the start of the current UTC day.
// Servers on one machine read the same wall clock; on a real cluster NTP or the platform keeps them within a few ms.
#pragma once

#include "CoreMinimal.h"

namespace GasLabClock
{
	/** Seconds since the epoch, wall clock. */
	double Now();
	/** The epoch, Unix seconds. */
	int64 Epoch();
	/** -gasnoclock: each server keeps its own world time, as the stock engine does (the lab's control run). */
	bool IsShared();
}
