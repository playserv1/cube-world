#include "PlayServ.h"
#include "Core/PlayServSettings.h"

void PlayServ::Data::Save(UObject* Entity, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetData()->SaveEntity(Entity, MoveTemp(Callback));
}

void PlayServ::Data::Reload(UObject* Entity, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetData()->ReloadEntity(Entity, MoveTemp(Callback));
}

void PlayServ::Data::Delete(UObject* Entity, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetData()->DeleteSelfEntity(Entity, MoveTemp(Callback));
}

void PlayServ::Data::Populate(UObject* Entity, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetData()->ReloadEntity(Entity, MoveTemp(Callback));
}

void PlayServ::Data::PopulateArray(UObject* Entity, const FString& PropertyName, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetData()->PopulateArrayProperty(Entity, PropertyName, MoveTemp(Callback));
}

void PlayServ::Data::BulkSave(const TArray<UObject*>& Entities, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetData()->SaveAllEntities(Entities, MoveTemp(Callback));
}

void PlayServ::Data::BulkDelete(const TArray<UObject*>& Entities, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetData()->DeleteEntities(Entities, MoveTemp(Callback));
}

FString PlayServ::Data::GetRecordId(const UObject* Entity)
{
	return UPlayServData::GetRecordId(Entity);
}

FPlayServSubscriptionHandle PlayServ::Data::Subscribe(UObject* Entity, FOnPlayServObjectChanged Delegate)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		return FPlayServSubscriptionHandle();
	}
	return PS->GetData()->SubscribeEntity(Entity, MoveTemp(Delegate));
}

FPlayServSubscriptionHandle PlayServ::Data::Subscribe(UObject* Entity)
{
	return Subscribe(Entity, FOnPlayServObjectChanged());
}

void PlayServ::Data::Unsubscribe(FPlayServSubscriptionHandle Handle)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		return;
	}
	PS->GetData()->UnsubscribeEntity(Handle);
}

void PlayServ::Auth::LoginAnonymous(FPlayServAuthCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetAuth()->LoginAnonymous(MoveTemp(Callback));
}

void PlayServ::Auth::LoginAnonymous(const FString& DisplayName, FPlayServAuthCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetAuth()->LoginAnonymous(DisplayName, MoveTemp(Callback));
}

void PlayServ::Auth::LoginExternal(const FPlayServExternalCredential& Credential, FPlayServAuthCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetAuth()->LoginExternal(Credential, MoveTemp(Callback));
}

bool PlayServ::Auth::TryGetLauncherCredential(FPlayServExternalCredential& OutCredential)
{
	return UPlayServAuth::TryGetLauncherCredential(OutCredential);
}

bool PlayServ::Auth::TryMakeSteamCredential(TConstArrayView<uint8> TicketBytes, FPlayServExternalCredential& OutCredential)
{
	return UPlayServAuth::TryMakeSteamCredential(TicketBytes, OutCredential);
}

void PlayServ::Auth::LoginWithRefreshToken(const FString& RefreshToken, FPlayServAuthCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetAuth()->LoginWithRefreshToken(RefreshToken, MoveTemp(Callback));
}

void PlayServ::Auth::LoginServer(const FString& ApiKey, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetAuth()->LoginServer(ApiKey, MoveTemp(Callback));
}

void PlayServ::Auth::LoginServer(FPlayServSimpleCallback Callback)
{
	const FString ServerKey = UPlayServSettings::GetServerKey();
	if (ServerKey.IsEmpty())
	{
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown,
			TEXT("No PlayServ server key configured: set ServerKey in Config/DedicatedServerGame.ini, or pass -PlayServServerKey= / PLAYSERV_SERVER_KEY")));
		return;
	}
	LoginServer(ServerKey, MoveTemp(Callback));
}

void PlayServ::Auth::Logout(FPlayServSimpleCallback Callback, EPlayServLogoutMode Mode)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetAuth()->Logout(MoveTemp(Callback), Mode);
}

bool PlayServ::Auth::IsLoggedIn()
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	return PS && PS->GetAuth() && PS->GetAuth()->IsLoggedIn();
}

bool PlayServ::Auth::IsServerSession()
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	return PS && PS->GetAuth() && PS->GetAuth()->IsServerSession();
}

EPlayServSessionType PlayServ::Auth::GetSessionType()
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS || !PS->GetAuth())
	{
		return EPlayServSessionType::None;
	}
	return PS->GetAuth()->GetSessionType();
}

const FString& PlayServ::Auth::GetPlayerId()
{
	static const FString EmptyId;
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS || !PS->GetAuth())
	{
		return EmptyId;
	}
	return PS->GetAuth()->GetPlayerId();
}

void PlayServ::Auth::SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged Handler)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS || !PS->GetAuth())
	{
		return;
	}
	PS->GetAuth()->SetRefreshTokenChangedHandler(MoveTemp(Handler));
}

FPlayServOnSessionLost& PlayServ::Auth::OnSessionLost()
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS || !PS->GetAuth())
	{
		static FPlayServOnSessionLost Sentinel;
		return Sentinel;
	}
	return PS->GetAuth()->OnSessionLost;
}

void PlayServ::Code::Call(const FString& FunctionName, const FPlayServRPCParams& Params, FPlayServRPCCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServRPCResult(), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetCode()->Call(FunctionName, Params, MoveTemp(Callback));
}

void PlayServ::Rooms::StartHosting(FPlayServSimpleCallback OnReady)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		OnReady.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->StartHosting(MoveTemp(OnReady));
}

void PlayServ::Rooms::StopHosting()
{
	if (UPlayServSubsystem* PS = UPlayServSubsystem::Get())
	{
		PS->GetRooms()->StopHosting();
	}
}

bool PlayServ::Rooms::IsHosting()
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	return PS != nullptr && PS->GetRooms()->IsHosting();
}

FString PlayServ::Rooms::GetLaunchRoomName()
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	return PS != nullptr ? PS->GetRooms()->GetLaunchRoomName() : FString();
}

void PlayServ::Rooms::StartRoom(const FPlayServRoomSnapshot& Snapshot, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->StartRoom(Snapshot, MoveTemp(Callback));
}

void PlayServ::Rooms::StartRoomPlayServHosted(FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->StartRoomPlayServHosted(MoveTemp(Callback));
}

bool PlayServ::Rooms::UpdateRoom(const FPlayServRoomSnapshot& Snapshot)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	return PS != nullptr && PS->GetRooms()->UpdateRoom(Snapshot);
}

bool PlayServ::Rooms::GetRoom(const FString& RoomName, FPlayServRoomSnapshot& OutRoom)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	return PS != nullptr && PS->GetRooms()->GetRoom(RoomName, OutRoom);
}

void PlayServ::Rooms::CloseRoom(const FString& RoomName, FPlayServSimpleCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->CloseRoom(RoomName, MoveTemp(Callback));
}

FString PlayServ::Rooms::TicketFromOptions(const FString& Options)
{
	return UPlayServRooms::TicketFromOptions(Options);
}

FPlayServTicketVerdict PlayServ::Rooms::VerifyTicket(const FString& Ticket)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		FPlayServTicketVerdict Verdict;
		Verdict.Reason = TEXT("admission_unavailable");
		Verdict.ErrorMessage = TEXT("Admission unavailable: the PlayServ subsystem is not available");
		return Verdict;
	}
	return PS->GetRooms()->VerifyTicket(Ticket);
}

FString PlayServ::Rooms::GetPlayerId(const APlayerController* Player)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	UPlayServRooms* Rooms = PS != nullptr ? PS->GetRooms() : nullptr;
	return Rooms != nullptr ? Rooms->GetPlayerId(Player) : FString();
}

bool PlayServ::Rooms::RemovePlayer(const FString& RoomName, const FString& PlayerId)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	return PS != nullptr && PS->GetRooms()->RemovePlayer(RoomName, PlayerId);
}

void PlayServ::Rooms::SubscribeData(const FString& Entity, const FString& KeyPath)
{
	if (UPlayServSubsystem* PS = UPlayServSubsystem::Get())
	{
		PS->GetRooms()->SubscribeData(Entity, KeyPath);
	}
}

void PlayServ::Rooms::UnsubscribeData(const FString& Entity)
{
	if (UPlayServSubsystem* PS = UPlayServSubsystem::Get())
	{
		PS->GetRooms()->UnsubscribeData(Entity);
	}
}

void PlayServ::Rooms::Browse(const FString& Slug, const FPlayServRoomFilters& Filters, FPlayServBrowseCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServBrowsePage(), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->Browse(Slug, Filters, MoveTemp(Callback));
}

void PlayServ::Rooms::Browse(const FPlayServRoomFilters& Filters, FPlayServBrowseCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServBrowsePage(), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->Browse(Filters, MoveTemp(Callback));
}

void PlayServ::Rooms::JoinRoom(const FString& Slug, const FString& RoomName, APlayerController* Player, FPlayServJoinCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServJoinResult(), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->JoinRoom(Slug, RoomName, Player, MoveTemp(Callback));
}

void PlayServ::Rooms::JoinRoom(const FString& RoomName, APlayerController* Player, FPlayServJoinCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServJoinResult(), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->JoinRoom(RoomName, Player, MoveTemp(Callback));
}

void PlayServ::Rooms::RequestNewRoom(const TMap<FString, FString>& Attributes, FPlayServRequestNewRoomCallback Callback)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!PS)
	{
		Callback.ExecuteIfBound(false, FPlayServRoomListing(), FPlayServError::SubsystemUnavailable());
		return;
	}
	PS->GetRooms()->RequestNewRoom(Attributes, MoveTemp(Callback));
}

FString PlayServ::Rooms::BuildTravelUrl(const FPlayServRoomTicket& Ticket)
{
	return UPlayServRooms::BuildTravelUrl(Ticket);
}
