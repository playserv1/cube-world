#include "S2STypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY(LogS2S);

UE_DEFINE_GAMEPLAY_TAG(TAG_S2S_SetByCaller_Damage, "SetByCaller.S2S.Damage");
UE_DEFINE_GAMEPLAY_TAG(TAG_S2S_State_Burning, "S2S.State.Burning");
UE_DEFINE_GAMEPLAY_TAG(TAG_S2S_Cue_Loop, "GameplayCue.S2S.Loop");
UE_DEFINE_GAMEPLAY_TAG(TAG_S2S_Cue_Burst, "GameplayCue.S2S.Burst");
UE_DEFINE_GAMEPLAY_TAG(TAG_S2S_Cue_FromB, "GameplayCue.S2S.FromB");
UE_DEFINE_GAMEPLAY_TAG(TAG_S2S_Cue_FromBLocal, "GameplayCue.S2S.FromBLocal");

namespace S2SStats
{
	namespace
	{
		struct FChannel
		{
			TArray<double> Window;
			TArray<double> All;
		};
		TMap<FName, FChannel>& Channels()
		{
			static TMap<FName, FChannel> Map;
			return Map;
		}

		FString Describe(TArray<double> Values)
		{
			if (Values.IsEmpty()) return TEXT("n=0");
			Values.Sort();
			double Sum = 0;
			for (double V : Values) Sum += V;
			auto At = [&Values](double Q) { return Values[FMath::Clamp(FMath::FloorToInt(Q * Values.Num()), 0, Values.Num() - 1)]; };
			return FString::Printf(TEXT("n=%d mean=%.1fms p50=%.1fms p95=%.1fms p99=%.1fms min=%.1fms max=%.1fms"),
				Values.Num(), 1000.0 * Sum / Values.Num(), 1000.0 * At(0.50), 1000.0 * At(0.95), 1000.0 * At(0.99),
				1000.0 * Values[0], 1000.0 * Values.Last());
		}
	}

	void RecordLatency(FName Channel, double Seconds)
	{
		FChannel& C = Channels().FindOrAdd(Channel);
		C.Window.Add(Seconds);
		C.All.Add(Seconds);
	}

	void LogLatency(const TCHAR* Prefix, bool bCumulative)
	{
		for (TPair<FName, FChannel>& Pair : Channels())
		{
			UE_LOG(LogS2S, Display, TEXT("[S2S %s] %s latency %s: %s"), *Who(), Prefix, *Pair.Key.ToString(),
				*Describe(bCumulative ? Pair.Value.All : Pair.Value.Window));
			Pair.Value.Window.Reset();
		}
	}

	FString RoleName(ENetRole Role)
	{
		switch (Role)
		{
		case ROLE_None: return TEXT("None");
		case ROLE_SimulatedProxy: return TEXT("SimulatedProxy");
		case ROLE_AutonomousProxy: return TEXT("AutonomousProxy");
		case ROLE_Authority: return TEXT("Authority");
		default: return TEXT("?");
		}
	}

	const FString& Who()
	{
		static const FString Name = []()
		{
			FString Out;
			if (FParse::Value(FCommandLine::Get(), TEXT("-s2sname="), Out)) return Out;
			return FString(IsRunningDedicatedServer() ? TEXT("server") : TEXT("client"));
		}();
		return Name;
	}
}
