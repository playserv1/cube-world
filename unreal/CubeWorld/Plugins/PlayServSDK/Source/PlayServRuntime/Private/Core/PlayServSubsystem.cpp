#include "Core/PlayServSubsystem.h"
#include "Core/PlayServLog.h"
#include "Core/PlayServSettings.h"
#include "Core/PlayServVersion.h"
#include "Core/PlayServHttp.h"
#include "Auth/PlayServAuth.h"
#include "Data/PlayServData.h"
#include "Code/PlayServCode.h"
#include "Rooms/PlayServRooms.h"
#include "Engine/Engine.h"

void UPlayServSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UPlayServSettings::ApplyCommandLineOverrides();
	UPlayServSettings::ValidateSettings();

	Http = MakeShared<FPlayServHttp>();

	Auth = NewObject<UPlayServAuth>(this);
	checkf(Auth, TEXT("PlayServ: Failed to create Auth module during initialization."));
	Auth->Init(Http);

	Http->SetAuth(Auth);

	Data = NewObject<UPlayServData>(this);
	checkf(Data, TEXT("PlayServ: Failed to create Data module during initialization."));
	Data->Init(Http);
	Auth->OnLogoutEndsRealtime.BindUObject(Data.Get(), &UPlayServData::EndRealtimeSession);

	Code = NewObject<UPlayServCode>(this);
	checkf(Code, TEXT("PlayServ: Failed to create Code module during initialization."));
	Code->Init(Http);

	Rooms = NewObject<UPlayServRooms>(this);
	checkf(Rooms, TEXT("PlayServ: Failed to create Rooms module during initialization."));
	Rooms->Init(Http);

	const UPlayServSettings* Settings = UPlayServSettings::Get();
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ SDK %s initialized (BaseURL=%s)"),
		PLAYSERV_SDK_VERSION,
		Settings ? *Settings->BaseURL : TEXT("<unset>"));
}

void UPlayServSubsystem::Deinitialize()
{
	if (Rooms)
	{
		Rooms->Shutdown();
	}

	if (Auth)
	{
		Auth->Shutdown();
	}

	if (Data)
	{
		Data->Shutdown();
	}

	if (Code)
	{
		Code->Shutdown();
	}

	Http.Reset();

	UE_LOG(LogPlayServ, Display, TEXT("PlayServ SDK deinitialized"));

	Super::Deinitialize();
}

UPlayServSubsystem* UPlayServSubsystem::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UPlayServSubsystem>() : nullptr;
}

UPlayServAuth* UPlayServSubsystem::GetAuth() const
{
	return Auth;
}

UPlayServData* UPlayServSubsystem::GetData() const
{
	return Data;
}

UPlayServCode* UPlayServSubsystem::GetCode() const
{
	return Code;
}

UPlayServRooms* UPlayServSubsystem::GetRooms() const
{
	return Rooms;
}
