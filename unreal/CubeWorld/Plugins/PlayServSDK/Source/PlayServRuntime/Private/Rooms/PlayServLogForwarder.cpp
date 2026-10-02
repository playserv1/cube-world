#include "Rooms/PlayServLogForwarder.h"

namespace
{
	// Set while Drain sends: a line the sending itself logs, on that thread, is not caught again.
	thread_local bool bDraining = false;
}

FString FPlayServLogForwarder::Redacted(const FString& Line)
{
	// A room ticket in a travel URL (`?rsv=<token>`) seats whoever presents it while its player is in the room.
	static const FString Marker = TEXT("rsv=");
	int32 From = Line.Find(Marker, ESearchCase::IgnoreCase);
	if (From == INDEX_NONE) return Line;
	FString Out;
	int32 Copied = 0;
	while (From != INDEX_NONE)
	{
		const int32 ValueAt = From + Marker.Len();
		int32 End = ValueAt;
		while (End < Line.Len() && !FChar::IsWhitespace(Line[End]) && Line[End] != TEXT('?') && Line[End] != TEXT('&') && Line[End] != TEXT('"') && Line[End] != TEXT('\'')) End++;
		Out += Line.Mid(Copied, ValueAt - Copied);
		Out += TEXT("<redacted>");
		Copied = End;
		From = Line.Find(Marker, ESearchCase::IgnoreCase, ESearchDir::FromStart, End);
	}
	Out += Line.Mid(Copied);
	return Out;
}

FPlayServLogForwarder::FPlayServLogForwarder(const FPlayServLogForwarding& InRules, TFunction<double()> InClock)
	: Rules(InRules)
	, Clock(MoveTemp(InClock))
{
}

void FPlayServLogForwarder::SetRules(const FPlayServLogForwarding& InRules)
{
	FScopeLock Guard(&Lock);
	Rules = InRules;
}

bool FPlayServLogForwarder::Wanted(const FPlayServLogForwarding& InRules, const FName& Category, ELogVerbosity::Type Verbosity, EPlayServLogLevel& OutLevel)
{
	const ELogVerbosity::Type Plain = (ELogVerbosity::Type)(Verbosity & ELogVerbosity::VerbosityMask);
	if (Plain == ELogVerbosity::NoLogging) return false;
	const ELogVerbosity::Type* Named = InRules.Categories.Find(Category);
	// The smaller the verbosity, the worse the line: Fatal 1, Error 2, Warning 3, Display 4, Log 5.
	if (Plain > (Named ? *Named : InRules.Everything)) return false;
	OutLevel = Plain <= ELogVerbosity::Error ? EPlayServLogLevel::Error
		: Plain == ELogVerbosity::Warning ? EPlayServLogLevel::Warn
		: Plain <= ELogVerbosity::Log ? EPlayServLogLevel::Info
		: EPlayServLogLevel::Debug;
	return true;
}

void FPlayServLogForwarder::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (bDraining || V == nullptr || *V == 0) return;
	FScopeLock Guard(&Lock);
	EPlayServLogLevel Level;
	if (!Wanted(Rules, Category, Verbosity, Level)) return;
	const double Now = Clock();
	if (WindowStartedAt < 0 || Now - WindowStartedAt >= 10.0)
	{
		WindowStartedAt = Now;
		LinesInWindow = 0;
	}
	if (++LinesInWindow > FMath::Max(1, Rules.MaxLinesPerTenSeconds))
	{
		OverRate++;
		return;
	}
	if (Waiting.Num() >= MaxWaitingLines)
	{
		Waiting.RemoveAt(0);
		Overflow++;
	}
	Waiting.Add({ Redacted(FString::Printf(TEXT("%s: %s"), *Category.ToString(), V)), Level });
}

void FPlayServLogForwarder::Drain(TFunctionRef<void(const FString&, EPlayServLogLevel)> Send)
{
	TArray<FLine> Lines;
	int32 Rated = 0, Overflowed = 0, Allowed = 0;
	{
		FScopeLock Guard(&Lock);
		Lines = MoveTemp(Waiting);
		Waiting.Reset();
		Rated = OverRate;
		Overflowed = Overflow;
		Allowed = Rules.MaxLinesPerTenSeconds;
		OverRate = 0;
		Overflow = 0;
	}
	if (Lines.Num() == 0 && Rated == 0 && Overflowed == 0) return;
	TGuardValue<bool> Sending(bDraining, true);
	for (const FLine& Line : Lines) Send(Line.Text, Line.Level);
	if (Rated > 0)
		Send(FString::Printf(TEXT("PlayServ: %d log line(s) not sent: more than %d came in ten seconds"), Rated, Allowed), EPlayServLogLevel::Warn);
	if (Overflowed > 0)
		Send(FString::Printf(TEXT("PlayServ: %d log line(s) not sent: more than %d waited for the game thread"), Overflowed, MaxWaitingLines), EPlayServLogLevel::Warn);
}
