#include "Core/PlayServSettings.h"
#include "Core/PlayServLog.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreMisc.h"
#include "Misc/Parse.h"

const UPlayServSettings* UPlayServSettings::Get()
{
	return GetDefault<UPlayServSettings>();
}

FString UPlayServSettings::GetBaseURL()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->BaseURL : FString();
}

FString UPlayServSettings::GetClientKey()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->ClientKey : FString();
}

FString UPlayServSettings::GetServerKey()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->ServerKey : FString();
}

FString UPlayServSettings::GetRoomDefaultSlug()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->RoomDefaultSlug : FString();
}

FString UPlayServSettings::GetDeploymentToken()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->DeploymentToken : FString();
}

FString UPlayServSettings::GetLaunchRoomName()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->LaunchRoomName : FString();
}

int32 UPlayServSettings::GetLaunchListenPort()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->LaunchListenPort : 0;
}

FString UPlayServSettings::GetLaunchPublicHost()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->LaunchPublicHost : FString();
}

FString UPlayServSettings::GetLaunchAttributesJson()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->LaunchAttributesJson : FString();
}

FString UPlayServSettings::GetRoomRegion()
{
	const UPlayServSettings* Settings = Get();
	return Settings ? Settings->RoomRegion : FString();
}

bool UPlayServSettings::IsAdmissionFailOpenInDevelopment()
{
	const UPlayServSettings* Settings = Get();
	return Settings != nullptr && Settings->bAdmissionFailOpenInDevelopment;
}

FString UPlayServSettings::ResolveServerKey(const FString& CommandLineValue, const FString& EnvironmentValue, const FString& IniValue)
{
	if (!CommandLineValue.IsEmpty())
	{
		return CommandLineValue;
	}
	if (!EnvironmentValue.IsEmpty())
	{
		return EnvironmentValue;
	}
	return IniValue;
}

FString UPlayServSettings::ResolveLaunchValue(const FString& CommandLineValue, const FString& EnvironmentValue)
{
	return CommandLineValue.IsEmpty() ? EnvironmentValue : CommandLineValue;
}

int32 UPlayServSettings::ParseLaunchPort(const FString& Text)
{
	const FString Trimmed = Text.TrimStartAndEnd().TrimQuotes().TrimStartAndEnd();
	if (Trimmed.IsEmpty() || Trimmed.Len() > 5)
	{
		return 0;
	}
	for (const TCHAR Character : Trimmed)
	{
		if (!FChar::IsDigit(Character))
		{
			return 0;
		}
	}
	const int32 Port = FCString::Atoi(*Trimmed);
	return Port >= 1 && Port <= 65535 ? Port : 0;
}

void UPlayServSettings::ApplyCommandLineOverrides()
{
	UPlayServSettings* Settings = GetMutableDefault<UPlayServSettings>();

	FString Value;
	if (FParse::Value(FCommandLine::Get(), TEXT("-PlayServBaseURL="), Value))
	{
		Settings->BaseURL = Value.TrimQuotes();
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ: override BaseURL = %s"), *Settings->BaseURL);
	}
	else
	{
		const FString EnvironmentBaseURL = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_API_URL")).TrimQuotes();
		if (!EnvironmentBaseURL.IsEmpty())
		{
			Settings->BaseURL = EnvironmentBaseURL;
			UE_LOG(LogPlayServ, Display, TEXT("PlayServ: override BaseURL from PLAYSERV_API_URL = %s"), *Settings->BaseURL);
		}
	}

	if (FParse::Value(FCommandLine::Get(), TEXT("-PlayServClientKey="), Value))
	{
		Settings->ClientKey = Value.TrimQuotes();
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ: override ClientKey (value not logged)"));
	}

	FString CommandLineServerKey;
	FParse::Value(FCommandLine::Get(), TEXT("-PlayServServerKey="), CommandLineServerKey);
	CommandLineServerKey = CommandLineServerKey.TrimQuotes();
	const FString EnvironmentServerKey = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_SERVER_KEY")).TrimQuotes();
	const FString ResolvedServerKey = ResolveServerKey(CommandLineServerKey, EnvironmentServerKey, Settings->ServerKey);
	if (ResolvedServerKey != Settings->ServerKey)
	{
		Settings->ServerKey = ResolvedServerKey;
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ: override ServerKey from %s (value not logged)"),
			CommandLineServerKey.IsEmpty() ? TEXT("PLAYSERV_SERVER_KEY") : TEXT("-PlayServServerKey="));
	}

	FString CommandLineRoomSlug;
	FParse::Value(FCommandLine::Get(), TEXT("-PlayServRoomDefaultSlug="), CommandLineRoomSlug);
	const FString ResolvedRoomSlug = ResolveServerKey(CommandLineRoomSlug.TrimQuotes(),
		FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_EXECUTOR_SLUG")).TrimQuotes(), Settings->RoomDefaultSlug);
	if (ResolvedRoomSlug != Settings->RoomDefaultSlug)
	{
		Settings->RoomDefaultSlug = ResolvedRoomSlug;
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ: override RoomDefaultSlug = %s"), *Settings->RoomDefaultSlug);
	}

	// Warning: the deployment token comes from the environment only; a command line is visible to other processes.
	Settings->DeploymentToken = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_DEPLOYMENT_TOKEN")).TrimQuotes();
	if (!Settings->DeploymentToken.IsEmpty())
	{
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ: deployment token found in PLAYSERV_DEPLOYMENT_TOKEN (value not logged)"));
	}

	FString CommandLineRoomName;
	FParse::Value(FCommandLine::Get(), TEXT("-PlayServRoomName="), CommandLineRoomName);
	Settings->LaunchRoomName = ResolveLaunchValue(CommandLineRoomName.TrimQuotes(),
		FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_ROOM_NAME")).TrimQuotes());
	if (!Settings->LaunchRoomName.IsEmpty())
	{
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ: launch room name = %s"), *Settings->LaunchRoomName);
	}

	const FString ListenPort = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_ROOM_LISTEN_PORT"));
	Settings->LaunchListenPort = ParseLaunchPort(ListenPort);
	if (Settings->LaunchListenPort > 0)
	{
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ: launch listen port = %d"), Settings->LaunchListenPort);
	}
	else if (!ListenPort.TrimStartAndEnd().IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: PLAYSERV_ROOM_LISTEN_PORT '%s' is not a port — ignored"), *ListenPort);
	}
	Settings->LaunchPublicHost = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_PUBLIC_IP")).TrimQuotes().TrimStartAndEnd();
	if (!Settings->LaunchPublicHost.IsEmpty())
	{
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ: launch public host = %s"), *Settings->LaunchPublicHost);
	}
	Settings->LaunchAttributesJson = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_ROOM_ATTRIBUTES")).TrimStartAndEnd();
}

void UPlayServSettings::ValidateSettings()
{
	const UPlayServSettings* Settings = Get();
	if (!Settings)
	{
		return;
	}
	if (Settings->BaseURL.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: BaseURL is empty. Configure it in Config/DefaultGame.ini ([/Script/PlayServRuntime.PlayServSettings]) or via -PlayServBaseURL="));
	}
	if (Settings->ClientKey.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: ClientKey is empty. Configure it in Config/DefaultGame.ini ([/Script/PlayServRuntime.PlayServSettings]) or via -PlayServClientKey="));
	}
	if (!Settings->ServerKey.IsEmpty() && !IsRunningDedicatedServer())
	{
		UE_LOG(LogPlayServ, Error, TEXT("PlayServ: a ServerKey is configured in a process that is not a dedicated server. It belongs in Config/DedicatedServerGame.ini (or -PlayServServerKey= / PLAYSERV_SERVER_KEY on the server) and must never ship in a client build."));
	}
	if (!Settings->DeploymentToken.IsEmpty() && !IsRunningDedicatedServer())
	{
		UE_LOG(LogPlayServ, Error, TEXT("PlayServ: PLAYSERV_DEPLOYMENT_TOKEN is set in a process that is not a dedicated server."));
	}
	if (Settings->RoomRegion.Len() > 32)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: RoomRegion exceeds 32 characters; the platform refuses it."));
	}
}

#if WITH_EDITOR
void UPlayServSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ValidateSettings();
}
#endif
