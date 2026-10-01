#include "Rooms/PlayServRooms.h"
#include "Auth/PlayServAuth.h"
#include "Core/PlayServHttp.h"
#include "Core/PlayServJwt.h"
#include "Core/PlayServLog.h"
#include "Core/PlayServSettings.h"
#include "Core/PlayServSubsystem.h"
#include "Rooms/PlayServAdmissionTable.h"
#include "Rooms/PlayServRoomRuntime.h"
#include "Rooms/PlayServRoomsPaths.h"
#include "Rooms/PlayServRoomsValidation.h"
#include "Rooms/PlayServRoomsClientWire.h"
#include "Rooms/PlayServRoomsWire.h"
#include "Rooms/PlayServUplinkClient.h"
#include "Rooms/PlayServUplinkTransport.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreMisc.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	constexpr float MaintenanceIntervalSeconds = 1.0f;
	constexpr int32 UnregisteredRoomFailureBudget = 3;
	constexpr int32 MaxBrowseAttributeFilters = 4;
	constexpr double VerifiedTicketRetentionSeconds = 120.0;

	const TCHAR* EndReasonLifetime = TEXT("lifetime");
	const TCHAR* EndReasonIdle = TEXT("idle");
	const TCHAR* RemovedReasonGraceLapsed = TEXT("reconnect_grace_lapsed");
	const TCHAR* RemovedReasonRemovedByGame = TEXT("removed_by_game");
	const TCHAR* ProblemRoomOwnedByOtherInstance = TEXT("room_owned_by_other_instance");
	const TCHAR* ProblemInstanceIdMismatch = TEXT("instance_id_mismatch");
	const TCHAR* ProblemRoomTypeNotFound = TEXT("room_type_not_found");
	const TCHAR* ProblemFunctionNotEligible = TEXT("function_not_matchmaking_eligible");
	const TCHAR* ProblemRoomQuotaExceeded = TEXT("room_quota_exceeded");
	const TCHAR* ClaimDeploymentId = TEXT("deployment_id");

	FPlayServError NotRunning()
	{
		return FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ rooms: this process is not hosting — call StartHosting and wait for its callback"));
	}

	FString TruncateDetail(const FString& Detail)
	{
		return Detail.Len() > PlayServRoomsWire::MaxDetailLength ? Detail.Left(PlayServRoomsWire::MaxDetailLength) : Detail;
	}
}

void UPlayServRooms::Init(TSharedPtr<FPlayServHttp> InHttp)
{
	Http = InHttp;
	Tickets = MakeShared<FPlayServAdmissionTable>();
	Clock = []()
	{
		return FPlatformTime::Seconds();
	};
	TransportFactory = FPlayServWebSocketUplinkTransport::MakeFactory();
	PlayServRoomsPaths::SetUseMatchmakingTwins(FParse::Param(FCommandLine::Get(), TEXT("PlayServRoomsTwins")));
	Launch = ReadLaunch();
#if !UE_BUILD_SHIPPING
	if (UPlayServSettings::IsAdmissionFailOpenInDevelopment())
	{
		bAdmissionFailOpen = true;
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: admission fails OPEN (bAdmissionFailOpenInDevelopment) — a player with no verifiable ticket is admitted; never ship this"));
	}
#endif
}

void UPlayServRooms::Shutdown()
{
	Joins.Reset();
	if (MaintenanceTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(MaintenanceTickerHandle);
		MaintenanceTickerHandle.Reset();
	}
	StopHosting();
	Tickets.Reset();
	Http.Reset();
}

double UPlayServRooms::Now() const
{
	return Clock ? Clock() : FPlatformTime::Seconds();
}

FPlayServError UPlayServRooms::RequireRoomDefaultSlug(FString& OutSlug)
{
	OutSlug = UPlayServSettings::GetRoomDefaultSlug();
	if (OutSlug.IsEmpty())
	{
		return FPlayServError::Make(EPlayServErrorCode::Unknown,
			TEXT("RoomDefaultSlug is not configured: set it in Config/DefaultGame.ini ([/Script/PlayServRuntime.PlayServSettings]), -PlayServRoomDefaultSlug= or PLAYSERV_EXECUTOR_SLUG"));
	}
	return FPlayServError::Success();
}

FString UPlayServRooms::ResolveCredential(const FString& DeploymentToken, const FString& ServerKey)
{
	return DeploymentToken.IsEmpty() ? ServerKey : DeploymentToken;
}

FString UPlayServRooms::ResolveInstanceId(const FString& DeploymentToken)
{
	FString DeploymentId;
	if (PlayServJwt::TryReadStringClaim(DeploymentToken, ClaimDeploymentId, DeploymentId) && PlayServRoomsValidation::IsValidInstanceId(DeploymentId))
	{
		return DeploymentId;
	}
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
}

UPlayServRooms::FLaunch UPlayServRooms::ReadLaunch()
{
	FLaunch Read;
	Read.RoomName = UPlayServSettings::GetLaunchRoomName();
	Read.ListenPort = UPlayServSettings::GetLaunchListenPort();
	Read.PublicHost = UPlayServSettings::GetLaunchPublicHost();
	const FString AttributesJson = UPlayServSettings::GetLaunchAttributesJson();
	if (!AttributesJson.IsEmpty() && !PlayServRoomsClientWire::ParseAttributesJson(AttributesJson, Read.Attributes))
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: PLAYSERV_ROOM_ATTRIBUTES is not a JSON object — the launch carries no attributes"));
	}
	return Read;
}

FPlayServError UPlayServRooms::SnapshotFromLaunch(const FLaunch& InLaunch, FPlayServRoomSnapshot& OutSnapshot)
{
	if (!InLaunch.IsSet())
	{
		return FPlayServError::Make(EPlayServErrorCode::ValidationFailed, TEXT("not_a_platform_launch: no PLAYSERV_ROOM_NAME — PlayServ hosting did not start this process for a room. A server the studio runs itself names its room and declares its address: StartRoom(Snapshot)."));
	}
	if (InLaunch.ListenPort <= 0)
	{
		return FPlayServError::Make(EPlayServErrorCode::ValidationFailed, FString::Printf(TEXT("launch_incomplete: room %s was launched with no PLAYSERV_ROOM_LISTEN_PORT, so there is no port to advertise"), *InLaunch.RoomName));
	}
	if (InLaunch.PublicHost.IsEmpty())
	{
		return FPlayServError::Make(EPlayServErrorCode::ValidationFailed, FString::Printf(TEXT("launch_incomplete: room %s was launched with no PLAYSERV_PUBLIC_IP, so there is no address to advertise"), *InLaunch.RoomName));
	}
	OutSnapshot.RoomName = InLaunch.RoomName;
	OutSnapshot.Attributes = InLaunch.Attributes;
	OutSnapshot.Connect.Host = InLaunch.PublicHost;
	OutSnapshot.Connect.Port = InLaunch.ListenPort;
	OutSnapshot.Connect.Transport = EPlayServRoomTransport::Udp;

	const FString* RequestedCapacity = InLaunch.Attributes.Find(PlayServ::Rooms::Attributes::Capacity);
	OutSnapshot.Capacity = RequestedCapacity != nullptr ? PlayServRoomsValidation::ParseCapacityAttribute(*RequestedCapacity) : 0;
	return FPlayServError::Success();
}

void UPlayServRooms::StartHosting(FPlayServSimpleCallback OnReady)
{
	if (bHosting && Uplink.IsValid() && Uplink->GetState() == EPlayServUplinkState::Ready)
	{
		OnReady.ExecuteIfBound(bHasRoomConfig, bHasRoomConfig ? FPlayServError::Success()
			: FPlayServError::Make(EPlayServErrorCode::NotFound, TEXT("room_config_missing: the room type has no room configuration; set one in the PlayServ admin before starting rooms")));
		return;
	}

	FPendingStart Pending;
	Pending.Callback = MoveTemp(OnReady);
	PendingStarts.Add(MoveTemp(Pending));
	if (bHosting)
	{
		return;
	}

	if (!Http.IsValid())
	{
		CompletePendingStart(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	if (!IsRunningDedicatedServer())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: Start called in a process that is not a dedicated server"));
	}

	const FPlayServError SlugError = RequireRoomDefaultSlug(RoomDefaultSlug);
	if (SlugError.IsError())
	{
		CompletePendingStart(false, SlugError);
		return;
	}

	const FString Credential = ResolveCredential(UPlayServSettings::GetDeploymentToken(), UPlayServSettings::GetServerKey());
	if (Credential.IsEmpty())
	{
		CompletePendingStart(false, FPlayServError::Make(EPlayServErrorCode::Unauthorized,
			TEXT("no server credential: set ServerKey in Config/DedicatedServerGame.ini (or PLAYSERV_SERVER_KEY), or launch with PLAYSERV_DEPLOYMENT_TOKEN")));
		return;
	}

	if (InstanceId.IsEmpty())
	{
		const FString DeploymentToken = UPlayServSettings::GetDeploymentToken();
		InstanceId = ResolveInstanceId(DeploymentToken);
		FString IssuedId;
		if (!DeploymentToken.IsEmpty() && !PlayServJwt::TryReadStringClaim(DeploymentToken, ClaimDeploymentId, IssuedId))
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: the deployment token carries no readable deployment_id — presenting instance %s, which a platform launch will not accept"), *InstanceId);
		}
	}

	UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get();
	UPlayServAuth* Auth = Subsystem ? Subsystem->GetAuth() : nullptr;
	if (Auth == nullptr)
	{
		CompletePendingStart(false, FPlayServError::SubsystemUnavailable());
		return;
	}
	if (!Auth->IsServerSession())
	{
		bool bLoggedIn = false;
		FPlayServError LoginError;
		Auth->LoginServer(Credential, FPlayServSimpleCallback::CreateLambda([&bLoggedIn, &LoginError](bool bSuccess, const FPlayServError& Error)
		{
			bLoggedIn = bSuccess;
			LoginError = Error;
		}));
		if (!bLoggedIn)
		{
			CompletePendingStart(false, LoginError);
			return;
		}
	}

	bHosting = true;
	BindGameModeEvents();
	if (!MaintenanceTickerHandle.IsValid())
	{
		MaintenanceTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UPlayServRooms::TickMaintenance), MaintenanceIntervalSeconds);
	}
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: starting (executor=%s, instance=%s, credential=%s)"),
		*RoomDefaultSlug, *InstanceId, UPlayServSettings::GetDeploymentToken().IsEmpty() ? TEXT("server key") : TEXT("deployment token"));
	OpenUplink();
}

void UPlayServRooms::OpenUplink()
{
	if (!Uplink.IsValid())
	{
		Uplink = MakeShared<FPlayServUplinkClient>(TransportFactory, Clock);
		TWeakObjectPtr<UPlayServRooms> WeakThis(this);
		Uplink->OnStateChanged.BindLambda([WeakThis](EPlayServUplinkState State)
		{
			if (UPlayServRooms* Self = WeakThis.Get())
			{
				Self->HandleUplinkState(State);
			}
		});
		Uplink->OnReady.BindLambda([WeakThis](const FPlayServUplinkAck& Ack, int32 Generation)
		{
			if (UPlayServRooms* Self = WeakThis.Get())
			{
				Self->HandleUplinkReady(Ack, Generation);
			}
		});
		Uplink->OnFrame.BindLambda([WeakThis](const FString& Type, const TSharedPtr<FJsonObject>& Frame)
		{
			if (UPlayServRooms* Self = WeakThis.Get())
			{
				Self->HandleUplinkFrame(Type, Frame);
			}
		});
		Uplink->OnRefused.BindLambda([WeakThis](const FString& Reason, bool bPermanent)
		{
			if (UPlayServRooms* Self = WeakThis.Get())
			{
				Self->HandleUplinkRefused(Reason, bPermanent);
			}
		});
	}

	FPlayServUplinkHelloParams Hello;
	Hello.ExecutorSlug = RoomDefaultSlug;
	Hello.InstanceId = InstanceId;
	Hello.Capabilities.Add(PlayServRoomsWire::CapabilityAdmissionPush);
	// A platform pool puts a machine into rotation only for a hello that declares room_create (the platform may ask it
	// for a room); without it the machine stays "ready" and is replaced after the start timeout. A server that opens
	// its rooms itself declares it too; a room request it does not serve is answered by the platform's own timeout.
	Hello.Capabilities.Add(PlayServRoomsWire::CapabilityRoomCreate);
	const FString Credential = ResolveCredential(UPlayServSettings::GetDeploymentToken(), UPlayServSettings::GetServerKey());
	Uplink->Start(PlayServRoomsPaths::UplinkUrl(UPlayServSettings::GetBaseURL()), Credential, Hello);
}

void UPlayServRooms::StopHosting()
{
	UnbindGameModeEvents();
	if (MaintenanceTickerHandle.IsValid() && Joins.Num() == 0)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(MaintenanceTickerHandle);
		MaintenanceTickerHandle.Reset();
	}

	TArray<FString> RoomNames;
	Rooms.GetKeys(RoomNames);
	for (const FString& RoomName : RoomNames)
	{
		CloseRoom(RoomName, FPlayServSimpleCallback());
	}

	if (Uplink.IsValid())
	{
		Uplink->Stop();
		Uplink.Reset();
	}
	if (Http.IsValid())
	{
		Http->SetServerBearerOverride(FString());
	}
	if (Tickets.IsValid())
	{
		*Tickets = FPlayServAdmissionTable();
	}
	Verified.Reset();
	DataSubscriptions.Reset();
	DataHeard.Reset();
	UnknownFrames.Reset();
	AdmissionMode = EPlayServAdmissionMode::None;
	bHasRoomConfig = false;
	UplinkGeneration = 0;
	const bool bWasRunning = bHosting;
	bHosting = false;
	CompletePendingStart(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ dedicated server stopped")));
	if (bWasRunning)
	{
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: stopped"));
	}
}

void UPlayServRooms::CompletePendingStart(bool bSuccess, const FPlayServError& Error)
{
	TArray<FPendingStart> Pending = MoveTemp(PendingStarts);
	PendingStarts.Reset();
	for (FPendingStart& Start : Pending)
	{
		Start.Callback.ExecuteIfBound(bSuccess, Error);
	}
}

bool UPlayServRooms::IsHosting() const
{
	return bHosting;
}

EPlayServUplinkState UPlayServRooms::GetUplinkState() const
{
	return Uplink.IsValid() ? Uplink->GetState() : EPlayServUplinkState::Disconnected;
}

bool UPlayServRooms::HasRoomConfig() const
{
	return bHasRoomConfig;
}

FPlayServRoomConfig UPlayServRooms::GetRoomConfig() const
{
	return RoomConfig;
}

EPlayServAdmissionMode UPlayServRooms::GetAdmissionMode() const
{
	return AdmissionMode;
}

FString UPlayServRooms::GetInstanceId() const
{
	return InstanceId;
}

FString UPlayServRooms::GetRoomDefaultSlug() const
{
	return RoomDefaultSlug;
}

FString UPlayServRooms::GetLaunchRoomName() const
{
	return Launch.RoomName;
}

void UPlayServRooms::HandleUplinkState(EPlayServUplinkState State)
{
	if (State != EPlayServUplinkState::Ready && Http.IsValid())
	{
		Http->SetServerBearerOverride(FString());
	}
	OnUplinkStateChanged.Broadcast(State);
}

void UPlayServRooms::HandleUplinkReady(const FPlayServUplinkAck& Ack, int32 Generation)
{
	const bool bNewSocket = Generation != UplinkGeneration;
	UplinkGeneration = Generation;
	if (Http.IsValid())
	{
		Http->SetServerBearerOverride(Ack.SessionToken);
	}
	AdmissionMode = Ack.Admission == PlayServRoomsWire::AdmissionPush ? EPlayServAdmissionMode::Push : EPlayServAdmissionMode::None;
	if (AdmissionMode == EPlayServAdmissionMode::None)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: the platform answered admission=%s; this plugin admits only by pushed ticket, so Admit refuses under the Strict policy"), *Ack.Admission);
	}

	if (Ack.bHasRoomConfig)
	{
		ApplyRoomConfig(Ack.RoomConfig, TEXT("hello"));
	}
	else
	{
		bHasRoomConfig = false;
		UE_LOG(LogPlayServ, Error, TEXT("PlayServ rooms: room_config is null for room type %s — no room can start until a room configuration exists"), *RoomDefaultSlug);
	}

	if (bNewSocket)
	{
		for (TPair<FString, TSharedPtr<FPlayServRoomRuntime>>& Pair : Rooms)
		{
			if (Pair.Value->bRegistered && !Pair.Value->bClosing)
			{
				SendRosterRepair(*Pair.Value);
			}
		}
		// The platform keeps a data subscription for the socket it came on: a new socket needs every one again. A copy,
		// since an OnDataSubscribed handler may subscribe or unsubscribe.
		const TArray<TPair<FString, FString>> Subscriptions = DataSubscriptions.Array();
		for (const TPair<FString, FString>& Subscription : Subscriptions)
		{
			SendDataSubscription(Subscription.Key, Subscription.Value);
		}
	}

	if (bHasRoomConfig)
	{
		CompletePendingStart(true, FPlayServError::Success());
	}
	else
	{
		CompletePendingStart(false, FPlayServError::Make(EPlayServErrorCode::NotFound,
			FString::Printf(TEXT("room_config_missing: room type %s has no room configuration; set one in the PlayServ admin. The uplink stays open, no room can start"), *RoomDefaultSlug)));
	}
}

void UPlayServRooms::HandleUplinkRefused(const FString& Reason, bool bPermanent)
{
	UE_LOG(LogPlayServ, Error, TEXT("PlayServ rooms: uplink refused (%s)%s"), *Reason, bPermanent ? TEXT(" — permanent") : TEXT(""));
	if (bPermanent)
	{
		CompletePendingStart(false, FPlayServError::Make(EPlayServErrorCode::Forbidden, FString::Printf(TEXT("uplink refused: %s"), *Reason)));
	}
}

void UPlayServRooms::HandleUplinkFrame(const FString& Type, const TSharedPtr<FJsonObject>& Frame)
{
	if (Type == PlayServRoomsWire::TypeTicketOffer)
	{
		HandleTicketOffer(Frame);
		return;
	}
	if (Type == PlayServRoomsWire::TypeJoinAck)
	{
		HandleJoinAck(Frame);
		return;
	}
	if (Type == PlayServRoomsWire::TypeDataUpdate)
	{
		HandleDataUpdate(Frame);
		return;
	}
	// The first frame of a type this module does not serve is logged with the reason it gives: a platform that cannot
	// serve what this server asked for (a data subscription, say) shows here.
	if (!UnknownFrames.Contains(Type))
	{
		UnknownFrames.Add(Type);
		FString Reason;
		if (!Frame->TryGetStringField(PlayServRoomsWire::FieldReason, Reason))
		{
			Frame->TryGetStringField(TEXT("message"), Reason);
		}
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: frame '%s' ignored%s%s"), *Type, Reason.IsEmpty() ? TEXT("") : TEXT(": "), *TruncateDetail(Reason));
		return;
	}
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ rooms: frame '%s' ignored"), *Type);
}

void UPlayServRooms::HandleTicketOffer(const TSharedPtr<FJsonObject>& Frame)
{
	FPlayServPushedTicket Ticket;
	Frame->TryGetStringField(PlayServRoomsWire::FieldReservationToken, Ticket.ReservationToken);
	Frame->TryGetStringField(PlayServRoomsWire::FieldRoomName, Ticket.RoomName);
	Frame->TryGetStringField(PlayServRoomsWire::FieldPlayerId, Ticket.PlayerId);
	double ExpiresIn = 0.0;
	Frame->TryGetNumberField(PlayServRoomsWire::FieldExpiresIn, ExpiresIn);
	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (Frame->TryGetObjectField(PlayServRoomsWire::FieldParams, Params))
	{
		Ticket.Params = *Params;
	}
	if (Ticket.ReservationToken.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: ticket_offer without a reservation_token ignored"));
		return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(PlayServRoomsWire::FieldType, PlayServRoomsWire::TypeTicketResult);
	Result->SetStringField(PlayServRoomsWire::FieldReservationToken, Ticket.ReservationToken);

	const TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(Ticket.RoomName);
	if (Room == nullptr || (*Room)->bClosing)
	{
		Result->SetBoolField(PlayServRoomsWire::FieldOk, false);
		Result->SetStringField(PlayServRoomsWire::FieldReason, PlayServRoomsWire::ReasonRoomClosed);
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: ticket for %s refused (%s)"),
			*Ticket.PlayerId, Room == nullptr ? TEXT("no such room here") : TEXT("room closing"));
	}
	else
	{
		FString Detail;
		const bool bAccepted = !OnTicketOffer.IsBound() || OnTicketOffer.Execute(Ticket.RoomName, Ticket.PlayerId, Detail);
		if (bAccepted)
		{
			Ticket.ExpiresAt = Now() + FMath::Max(ExpiresIn, 0.0);
			Tickets->Add(Ticket);
			Result->SetBoolField(PlayServRoomsWire::FieldOk, true);
			UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: ticket accepted for %s into %s (expires in %.0fs)"),
				*Ticket.PlayerId, *Ticket.RoomName, FMath::Max(ExpiresIn, 0.0));
		}
		else
		{
			Result->SetBoolField(PlayServRoomsWire::FieldOk, false);
			Result->SetStringField(PlayServRoomsWire::FieldReason, PlayServRoomsWire::ReasonRoomRefused);
			if (!Detail.IsEmpty())
			{
				Result->SetStringField(PlayServRoomsWire::FieldDetail, TruncateDetail(Detail));
			}
			UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: ticket for %s into %s vetoed by the game%s%s"),
				*Ticket.PlayerId, *Ticket.RoomName,
				Detail.IsEmpty() ? TEXT("") : TEXT(": "), Detail.IsEmpty() ? TEXT("") : *Detail);
		}
	}
	if (Uplink.IsValid())
	{
		Uplink->SendFrame(Result);
	}
}

void UPlayServRooms::HandleJoinAck(const TSharedPtr<FJsonObject>& Frame)
{
	bool bOk = false;
	Frame->TryGetBoolField(PlayServRoomsWire::FieldOk, bOk);
	if (bOk)
	{
		return;
	}
	FString RoomName;
	FString PlayerId;
	FString Reason;
	Frame->TryGetStringField(PlayServRoomsWire::FieldRoomName, RoomName);
	Frame->TryGetStringField(PlayServRoomsWire::FieldPlayerId, PlayerId);
	Frame->TryGetStringField(PlayServRoomsWire::FieldReason, Reason);
	UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: join refused by the platform (room=%s, reason=%s) — removing the player"), *RoomName, *Reason);

	if (TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName))
	{
		(*Room)->Roster.Remove(PlayerId);
		(*Room)->Parked.Remove(PlayerId);
		(*Room)->JoinTokens.Remove(PlayerId);
		if ((*Room)->Roster.Num() == 0)
		{
			(*Room)->IdleSince = Now();
		}
	}
	OnPlayerRemoved.Broadcast(RoomName, PlayerId, Reason.IsEmpty() ? FString(PlayServRoomsWire::ReasonReservationInvalid) : Reason);
}

void UPlayServRooms::SubscribeData(const FString& Entity, const FString& KeyPath)
{
	if (Entity.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: SubscribeData needs an entity"));
		return;
	}
	DataSubscriptions.Add(Entity, KeyPath);
	if (!SendDataSubscription(Entity, KeyPath))
	{
		UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ rooms: the subscription to %s goes out once the uplink is ready"), *Entity);
	}
}

void UPlayServRooms::UnsubscribeData(const FString& Entity)
{
	if (DataSubscriptions.Remove(Entity) == 0 || !Uplink.IsValid() || Uplink->GetState() != EPlayServUplinkState::Ready)
	{
		return;
	}
	TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(PlayServRoomsWire::FieldType, PlayServRoomsWire::TypeUnsubscribeData);
	Frame->SetStringField(PlayServRoomsWire::FieldProjectId, FString());
	Frame->SetStringField(PlayServRoomsWire::FieldClientKey, FString());
	Frame->SetStringField(PlayServRoomsWire::FieldEntity, Entity);
	Uplink->SendFrame(Frame);
}

bool UPlayServRooms::SendDataSubscription(const FString& Entity, const FString& KeyPath)
{
	if (!Uplink.IsValid() || Uplink->GetState() != EPlayServUplinkState::Ready)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(PlayServRoomsWire::FieldType, PlayServRoomsWire::TypeSubscribeData);
	Frame->SetStringField(PlayServRoomsWire::FieldProjectId, FString());
	Frame->SetStringField(PlayServRoomsWire::FieldClientKey, FString());
	Frame->SetStringField(PlayServRoomsWire::FieldEntity, Entity);
	Frame->SetStringField(PlayServRoomsWire::FieldKeyPath, KeyPath);
	if (!Uplink->SendFrame(Frame))
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: the subscription to %s was not sent; it goes out again on the next uplink socket"), *Entity);
		return false;
	}
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: subscribed to %s changes over the uplink (%s)"), *Entity, *KeyPath);
	OnDataSubscribed.Broadcast(Entity);
	return true;
}

void UPlayServRooms::HandleDataUpdate(const TSharedPtr<FJsonObject>& Frame)
{
	FPlayServDataUpdate Update;
	Frame->TryGetStringField(PlayServRoomsWire::FieldEntity, Update.Entity);
	Frame->TryGetStringField(PlayServRoomsWire::FieldId, Update.Id);
	Frame->TryGetStringField(PlayServRoomsWire::FieldOp, Update.Op);
	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (Frame->TryGetObjectField(PlayServRoomsWire::FieldData, Data))
	{
		Update.Data = *Data;
	}
	if (Update.Entity.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: data_update without an entity ignored"));
		return;
	}
	// The first change of each entity is logged with what it carries: the platform's side of SubscribeData, seen once.
	if (!DataHeard.Contains(Update.Entity))
	{
		DataHeard.Add(Update.Entity);
		TArray<FString> Fields;
		if (Update.Data.IsValid())
		{
			for (const auto& Field : Update.Data->Values)
			{
				Fields.Add(FString(*Field.Key));
			}
		}
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: the first %s change arrived over the uplink (op=%s, id=%s, fields: %s)"),
			*Update.Entity, *Update.Op, *Update.Id, Fields.Num() > 0 ? *FString::Join(Fields, TEXT(", ")) : TEXT("none"));
	}
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ rooms: data_update %s %s %s"), *Update.Entity, *Update.Op, *Update.Id);
	OnDataUpdate.Broadcast(Update);
}

void UPlayServRooms::ApplyRoomConfig(const FPlayServRoomConfig& Config, const TCHAR* Source)
{
	if (bHasRoomConfig && Config.Version == RoomConfig.Version)
	{
		return;
	}
	RoomConfig = Config;
	bHasRoomConfig = true;
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: room configuration v%d applied from %s (capacity=%d, ttl=%ds, lifetime=%ds, idle=%s, max_rooms=%d)"),
		Config.Version, Source, Config.Capacity, Config.ReservationTtlSeconds, Config.RoomLifetimeSeconds,
		Config.bHasIdleTimeout ? *FString::Printf(TEXT("%ds"), Config.RoomIdleTimeoutSeconds) : TEXT("none"), Config.MaxRooms);
	OnRoomConfigChanged.Broadcast(Config);
}

void UPlayServRooms::StartRoom(const FPlayServRoomSnapshot& InSnapshot, FPlayServSimpleCallback Callback)
{
	if (!bHosting || !Uplink.IsValid() || Uplink->GetState() != EPlayServUplinkState::Ready)
	{
		Callback.ExecuteIfBound(false, NotRunning());
		return;
	}
	if (!bHasRoomConfig)
	{
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::NotFound,
			FString::Printf(TEXT("room_config_missing: room type %s has no room configuration; no room can start"), *RoomDefaultSlug)));
		return;
	}
	if (Rooms.Contains(InSnapshot.RoomName))
	{
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Conflict,
			FString::Printf(TEXT("room '%s' is already registered by this process"), *InSnapshot.RoomName)));
		return;
	}
	if (RoomConfig.MaxRooms > 0 && Rooms.Num() >= RoomConfig.MaxRooms)
	{
		FPlayServError Error = FPlayServError::Make(EPlayServErrorCode::Conflict,
			FString::Printf(TEXT("room_quota_exceeded: this room type allows %d rooms"), RoomConfig.MaxRooms));
		Error.ProblemCode = ProblemRoomQuotaExceeded;
		Callback.ExecuteIfBound(false, Error);
		return;
	}

	FPlayServRoomSnapshot Snapshot = InSnapshot;
	if (Snapshot.Capacity <= 0)
	{
		Snapshot.Capacity = RoomConfig.Capacity;
	}
	if (Snapshot.Region.IsEmpty())
	{
		Snapshot.Region = UPlayServSettings::GetRoomRegion();
	}
	if (!Snapshot.Connect.IsSet())
	{
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::ValidationFailed,
			TEXT("connect_not_declared: the room names no address players can dial. Set Snapshot.Connect.Host and Snapshot.Connect.Port — the SDK guesses no address. A server PlayServ hosting started registers with StartRoomPlayServHosted, which takes the address from the launch.")));
		return;
	}
	const FPlayServError Validation = PlayServRoomsValidation::ValidateSnapshot(Snapshot);
	if (Validation.IsError())
	{
		Callback.ExecuteIfBound(false, Validation);
		return;
	}

	TSharedPtr<FPlayServRoomRuntime> Room = MakeShared<FPlayServRoomRuntime>();
	Room->Snapshot = Snapshot;
	const double Current = Now();
	Room->StartedAt = Current;
	Room->IdleSince = Current;
	Room->NextBeatAt = Current;
	Rooms.Add(Snapshot.RoomName, Room);
	PendingRoomStarts.Add(Snapshot.RoomName, MoveTemp(Callback));
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: registering room %s (%s:%d %s)"),
		*Snapshot.RoomName, *Snapshot.Connect.Host, Snapshot.Connect.Port, PlayServRoomsValidation::TransportToWire(Snapshot.Connect.Transport));
	SendHeartbeat(Snapshot.RoomName, Current);
}

void UPlayServRooms::StartRoomPlayServHosted(FPlayServSimpleCallback Callback)
{
	FPlayServRoomSnapshot Snapshot;
	const FPlayServError LaunchError = SnapshotFromLaunch(Launch, Snapshot);
	if (LaunchError.IsError())
	{
		Callback.ExecuteIfBound(false, LaunchError);
		return;
	}
	if (Snapshot.Capacity == 0 && Launch.Attributes.Contains(PlayServ::Rooms::Attributes::Capacity))
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: the launch asked for capacity '%s', which is not a seat count 1..%d — room %s takes the room configuration's capacity"), *Launch.Attributes.FindRef(PlayServ::Rooms::Attributes::Capacity), PlayServRoomsValidation::MaxCapacityAttribute, *Snapshot.RoomName);
	}
	if (bHosting && Uplink.IsValid() && Uplink->GetState() == EPlayServUplinkState::Ready)
	{
		StartRoom(Snapshot, MoveTemp(Callback));
		return;
	}

	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: launched for room %s — opening the uplink before registering it"), *Snapshot.RoomName);
	TWeakObjectPtr<UPlayServRooms> WeakThis(this);
	StartHosting(FPlayServSimpleCallback::CreateLambda([WeakThis, Snapshot, Callback](bool bSuccess, const FPlayServError& Error)
	{
		UPlayServRooms* Self = WeakThis.Get();
		if (Self == nullptr)
		{
			Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
			return;
		}
		if (!bSuccess)
		{
			Callback.ExecuteIfBound(false, Error);
			return;
		}
		Self->StartRoom(Snapshot, Callback);
	}));
}

bool UPlayServRooms::UpdateRoom(const FPlayServRoomSnapshot& Snapshot)
{
	TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(Snapshot.RoomName);
	if (Room == nullptr || (*Room)->bClosing)
	{
		return false;
	}
	FPlayServRoomSnapshot Next = Snapshot;
	if (Next.Capacity <= 0)
	{
		Next.Capacity = (*Room)->Snapshot.Capacity;
	}
	if (!Next.Connect.IsSet())
	{
		Next.Connect = (*Room)->Snapshot.Connect;
	}
	if (Next.Region.IsEmpty())
	{
		Next.Region = (*Room)->Snapshot.Region;
	}
	const FPlayServError Validation = PlayServRoomsValidation::ValidateSnapshot(Next);
	if (Validation.IsError())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: UpdateRoom(%s) rejected — %s"), *Snapshot.RoomName, *Validation.Message);
		return false;
	}
	(*Room)->Snapshot = Next;
	return true;
}

bool UPlayServRooms::SetRoomOpen(const FString& RoomName, bool bOpen)
{
	TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName);
	if (Room == nullptr || (*Room)->bClosing)
	{
		return false;
	}
	(*Room)->Snapshot.bOpen = bOpen;
	return true;
}

void UPlayServRooms::CloseRoom(const FString& RoomName, FPlayServSimpleCallback Callback)
{
	TSharedPtr<FPlayServRoomRuntime>* Found = Rooms.Find(RoomName);
	if (Found == nullptr)
	{
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::NotFound, FString::Printf(TEXT("room '%s' is not registered"), *RoomName)));
		return;
	}
	TSharedPtr<FPlayServRoomRuntime> Room = *Found;
	Room->bClosing = true;
	PendingRoomStarts.Remove(RoomName);

	if (Tickets.IsValid())
	{
		TArray<FPlayServPushedTicket> Unredeemed;
		Tickets->RemoveRoom(RoomName, Unredeemed);
		for (const FPlayServPushedTicket& Ticket : Unredeemed)
		{
			ReleaseTicket(Ticket.ReservationToken, PlayServRoomsWire::ReasonRoomClosed, FString(), false);
		}
	}

	if (!Room->bRegistered || !Http.IsValid())
	{
		Rooms.Remove(RoomName);
		Callback.ExecuteIfBound(true, FPlayServError::Success());
		return;
	}

	TWeakObjectPtr<UPlayServRooms> WeakThis(this);
	Http->Request(EPlayServHttpVerb::Post, PlayServRoomsPaths::Close(RoomDefaultSlug, RoomName), nullptr,
		FPlayServV2Callback::CreateLambda([WeakThis, RoomName, Callback](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			if (UPlayServRooms* Self = WeakThis.Get())
			{
				Self->Rooms.Remove(RoomName);
			}
			const bool bClosed = bSuccess || Response.Status == 404;
			if (bClosed)
			{
				UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: room %s closed on the platform"), *RoomName);
			}
			else
			{
				UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: room %s did not close (HTTP %d)"), *RoomName, Response.Status);
			}
			Callback.ExecuteIfBound(bClosed, bClosed ? FPlayServError::Success() : Error);
		}));
}

void UPlayServRooms::EndRoom(const FString& RoomName, const FString& Reason, bool bSendClose)
{
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: room %s ended (%s)"), *RoomName, *Reason);
	if (bSendClose)
	{
		CloseRoom(RoomName, FPlayServSimpleCallback());
	}
	else if (TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName))
	{
		(*Room)->bClosing = true;
		if (Tickets.IsValid())
		{
			TArray<FPlayServPushedTicket> Dropped;
			Tickets->RemoveRoom(RoomName, Dropped);
		}
		PendingRoomStarts.Remove(RoomName);
		Rooms.Remove(RoomName);
	}
	OnRoomEnded.Broadcast(RoomName, Reason);
}

TArray<FString> UPlayServRooms::GetRoomNames() const
{
	TArray<FString> Names;
	Rooms.GetKeys(Names);
	return Names;
}

bool UPlayServRooms::GetRoom(const FString& RoomName, FPlayServRoomSnapshot& OutRoom) const
{
	const TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName);
	if (Room == nullptr || (*Room)->bClosing)
	{
		return false;
	}
	OutRoom = (*Room)->Snapshot;
	return true;
}

EPlayServPlacementState UPlayServRooms::GetRoomPlacement(const FString& RoomName) const
{
	const TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName);
	return Room ? (*Room)->Placement : EPlayServPlacementState::Unknown;
}

int32 UPlayServRooms::GetRoomPlayerCount(const FString& RoomName) const
{
	const TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName);
	return Room ? (*Room)->PlayerCount() : 0;
}

bool UPlayServRooms::TickMaintenance(float DeltaTime)
{
	const double Current = Now();
	if (Uplink.IsValid())
	{
		Uplink->Tick(Current);
	}
	TickRooms(Current);
	const bool bJoinsRunning = TickJoins(Current);
	if (!bHosting && !bJoinsRunning)
	{
		MaintenanceTickerHandle.Reset();
		return false;
	}
	return true;
}

void UPlayServRooms::TickRooms(double Current)
{
	SweepVerified(Current);
	SweepLostControllers();
	if (Tickets.IsValid() && Uplink.IsValid())
	{
		TArray<FPlayServPushedTicket> Expired;
		Tickets->SweepExpired(Current, Expired);
		for (const FPlayServPushedTicket& Ticket : Expired)
		{
			ReleaseTicket(Ticket.ReservationToken, PlayServRoomsWire::ReasonReservationExpired, FString());
		}
	}

	TArray<FString> RoomNames;
	Rooms.GetKeys(RoomNames);
	for (const FString& RoomName : RoomNames)
	{
		TSharedPtr<FPlayServRoomRuntime>* Found = Rooms.Find(RoomName);
		if (Found == nullptr || (*Found)->bClosing)
		{
			continue;
		}
		TSharedPtr<FPlayServRoomRuntime> Room = *Found;

		TArray<FString> Lapsed;
		for (const TPair<FString, double>& Pair : Room->Parked)
		{
			if (Current >= Pair.Value)
			{
				Lapsed.Add(Pair.Key);
			}
		}
		for (const FString& PlayerId : Lapsed)
		{
			ReportPlayerLeft(RoomName, PlayerId);
			OnPlayerRemoved.Broadcast(RoomName, PlayerId, RemovedReasonGraceLapsed);
		}

		if (bHasRoomConfig && RoomConfig.RoomLifetimeSeconds > 0 && Current - Room->StartedAt >= RoomConfig.RoomLifetimeSeconds)
		{
			EndRoom(RoomName, EndReasonLifetime, true);
			continue;
		}
		if (bHasRoomConfig && RoomConfig.bHasIdleTimeout && Room->Roster.Num() == 0 && Room->IdleSince > 0.0
			&& Current - Room->IdleSince >= RoomConfig.RoomIdleTimeoutSeconds)
		{
			EndRoom(RoomName, EndReasonIdle, true);
			continue;
		}

		if (Current >= Room->NextBeatAt && Room->InFlightBeats == 0)
		{
			SendHeartbeat(RoomName, Current);
		}
	}
}

void UPlayServRooms::SendHeartbeat(const FString& RoomName, double Current)
{
	TSharedPtr<FPlayServRoomRuntime>* Found = Rooms.Find(RoomName);
	if (Found == nullptr || !Http.IsValid())
	{
		return;
	}
	TSharedPtr<FPlayServRoomRuntime> Room = *Found;
	Room->NextBeatAt = Current + PlayServRoomsWire::HeartbeatIntervalSeconds;
	++Room->InFlightBeats;

	const TSharedPtr<FJsonObject> Body = PlayServRoomRuntime::BuildUpsertBody(*Room, InstanceId);
	TWeakObjectPtr<UPlayServRooms> WeakThis(this);
	Http->Request(EPlayServHttpVerb::Post, PlayServRoomsPaths::Upsert(RoomDefaultSlug), Body,
		FPlayServV2Callback::CreateLambda([WeakThis, RoomName](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			if (UPlayServRooms* Self = WeakThis.Get())
			{
				Self->HandleHeartbeatAnswer(RoomName, bSuccess, Response.Status, Response.ProblemCode, Response.Json, Error);
			}
		}));
}

void UPlayServRooms::HandleHeartbeatAnswer(const FString& RoomName, bool bSuccess, int32 Status, const FString& ProblemCode, const TSharedPtr<FJsonObject>& Json, const FPlayServError& Error)
{
	TSharedPtr<FPlayServRoomRuntime>* Found = Rooms.Find(RoomName);
	if (Found == nullptr)
	{
		return;
	}
	TSharedPtr<FPlayServRoomRuntime> Room = *Found;
	Room->InFlightBeats = FMath::Max(0, Room->InFlightBeats - 1);

	if (bSuccess)
	{
		const bool bFirstAnswer = !Room->bRegistered;
		Room->bRegistered = true;
		Room->ConsecutiveFailures = 0;

		FPlayServUpsertAck Ack;
		if (FPlayServUpsertAck::Parse(Json, Ack))
		{
			if (Ack.bCreated && !bFirstAnswer)
			{
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: room %s was re-inserted by a heartbeat (evicted while unreachable)"), *RoomName);
			}
			if (Ack.Placement != Room->Placement)
			{
				Room->Placement = Ack.Placement;
				OnRoomPlacementChanged.Broadcast(RoomName, Ack.Placement);
			}
			if (Ack.bHasRoomConfig)
			{
				ApplyRoomConfig(Ack.RoomConfig, TEXT("heartbeat"));
			}
			if (Ack.RosterCheck == EPlayServRosterCheck::Mismatch || Room->RosterSentForGeneration != UplinkGeneration)
			{
				SendRosterRepair(*Room);
			}
		}

		if (FPlayServSimpleCallback* Pending = PendingRoomStarts.Find(RoomName))
		{
			FPlayServSimpleCallback Callback = MoveTemp(*Pending);
			PendingRoomStarts.Remove(RoomName);
			Callback.ExecuteIfBound(true, FPlayServError::Success());
		}
		return;
	}

	++Room->ConsecutiveFailures;
	const bool bOwnershipLost = (Status == 409 && ProblemCode == ProblemRoomOwnedByOtherInstance) || (Status == 422 && ProblemCode == ProblemInstanceIdMismatch);
	const bool bNotARoomType = Status == 404 && (ProblemCode == ProblemRoomTypeNotFound || ProblemCode == ProblemFunctionNotEligible);
	const bool bQuota = Status == 429 && ProblemCode == ProblemRoomQuotaExceeded;
	const bool bPermanent = bOwnershipLost || bNotARoomType || bQuota;

	UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: heartbeat for %s failed (%s)%s"), *RoomName, *Error.Message, bPermanent ? TEXT(" — permanent") : TEXT(""));

	if (bPermanent || (!Room->bRegistered && Room->ConsecutiveFailures >= UnregisteredRoomFailureBudget))
	{
		if (FPlayServSimpleCallback* Pending = PendingRoomStarts.Find(RoomName))
		{
			FPlayServSimpleCallback Callback = MoveTemp(*Pending);
			PendingRoomStarts.Remove(RoomName);
			Callback.ExecuteIfBound(false, Error);
		}
		EndRoom(RoomName, ProblemCode.IsEmpty() ? Error.Message : ProblemCode, false);
	}
}

void UPlayServRooms::SendRosterRepair(FPlayServRoomRuntime& Room)
{
	if (!Uplink.IsValid() || Uplink->GetState() != EPlayServUplinkState::Ready)
	{
		return;
	}
	const TSharedPtr<FJsonObject> Frame = PlayServRoomRuntime::BuildRosterFrame(
		Room.Snapshot.RoomName, Room.NextSeq(PlayServRoomRuntime::UnixMillisecondsNow()), Room.RosterIds());
	if (Uplink->SendFrame(Frame))
	{
		Room.RosterSentForGeneration = UplinkGeneration;
	}
}

bool UPlayServRooms::SendPresenceDelta(FPlayServRoomRuntime& Room, const TCHAR* Event, const FString& PlayerId, const FString& ReservationToken)
{
	if (!Uplink.IsValid() || Uplink->GetState() != EPlayServUplinkState::Ready)
	{
		UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ rooms: %s for %s not sent — uplink not ready; the roster repair after reconnect carries it"), Event, *PlayerId);
		return false;
	}
	if (AdmissionMode == EPlayServAdmissionMode::Push && FCString::Strcmp(Event, PlayServRoomsWire::EventJoin) == 0 && ReservationToken.IsEmpty())
	{
		UE_LOG(LogPlayServ, Error, TEXT("PlayServ rooms: the join for %s carries no reservation token; on the pushed-ticket path the platform REFUSES such a join with join_ack ok=false reason=reservation_invalid and the player never appears in the room's roster"), *PlayerId);
	}
	const TSharedPtr<FJsonObject> Frame = PlayServRoomRuntime::BuildPresenceDelta(
		Room.Snapshot.RoomName, Room.NextSeq(PlayServRoomRuntime::UnixMillisecondsNow()), Event, PlayerId, ReservationToken);
	return Uplink->SendFrame(Frame);
}

void UPlayServRooms::AdmitToRoom(const FString& RoomName, const FString& PlayerId, const FString& ReservationToken)
{
	TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName);
	if (Room == nullptr || (*Room)->bClosing || PlayerId.IsEmpty())
	{
		return;
	}
	(*Room)->Roster.Add(PlayerId);
	(*Room)->Parked.Remove(PlayerId);
	(*Room)->IdleSince = 0.0;
	if (!ReservationToken.IsEmpty())
	{
		(*Room)->JoinTokens.Add(PlayerId, ReservationToken);
	}
	SendPresenceDelta(**Room, PlayServRoomsWire::EventJoin, PlayerId, ReservationToken);
}

bool UPlayServRooms::ReportPlayerLeft(const FString& RoomName, const FString& PlayerId)
{
	TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName);
	if (Room == nullptr || PlayerId.IsEmpty())
	{
		return false;
	}
	const bool bWasMember = (*Room)->Roster.Remove(PlayerId) > 0;
	(*Room)->Parked.Remove(PlayerId);
	(*Room)->JoinTokens.Remove(PlayerId);
	if ((*Room)->Roster.Num() == 0)
	{
		(*Room)->IdleSince = Now();
	}
	if (bWasMember && !(*Room)->bClosing)
	{
		SendPresenceDelta(**Room, PlayServRoomsWire::EventLeave, PlayerId, FString());
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: %s left %s — reported to the platform"), *PlayerId, *RoomName);
	}
	return bWasMember;
}

bool UPlayServRooms::RemovePlayer(const FString& RoomName, const FString& PlayerId)
{
	const bool bWasMember = ReportPlayerLeft(RoomName, PlayerId);
	if (bWasMember)
	{
		for (TMap<TWeakObjectPtr<const APlayerController>, FString>::TIterator It(AdmittedPlayers); It; ++It)
		{
			if (It.Value() == PlayerId)
			{
				It.RemoveCurrent();
			}
		}
		OnPlayerRemoved.Broadcast(RoomName, PlayerId, RemovedReasonRemovedByGame);
	}
	return bWasMember;
}

void UPlayServRooms::SetReconnectGraceSeconds(float Seconds)
{
	ReconnectGraceSeconds = FMath::Max(0.0f, Seconds);
}

float UPlayServRooms::GetReconnectGraceSeconds() const
{
	return ReconnectGraceSeconds;
}

FString UPlayServRooms::GetPlayerId(const APlayerController* Player) const
{
	return ResolvePlayerId(Player);
}

void UPlayServRooms::BindGameModeEvents()
{
	if (PostLoginHandle.IsValid())
	{
		return;
	}
	PostLoginHandle = FGameModeEvents::OnGameModePostLoginEvent().AddUObject(this, &UPlayServRooms::HandleGameModePostLogin);
	LogoutHandle = FGameModeEvents::OnGameModeLogoutEvent().AddUObject(this, &UPlayServRooms::HandleGameModeLogout);
	PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &UPlayServRooms::HandlePreLoadMap);
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UPlayServRooms::HandlePostLoadMap);
}

void UPlayServRooms::UnbindGameModeEvents()
{
	if (PostLoginHandle.IsValid())
	{
		FGameModeEvents::OnGameModePostLoginEvent().Remove(PostLoginHandle);
		PostLoginHandle.Reset();
	}
	if (LogoutHandle.IsValid())
	{
		FGameModeEvents::OnGameModeLogoutEvent().Remove(LogoutHandle);
		LogoutHandle.Reset();
	}
	if (PreLoadMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PreLoadMap.Remove(PreLoadMapHandle);
		PreLoadMapHandle.Reset();
	}
	if (PostLoadMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
		PostLoadMapHandle.Reset();
	}
	LoadStartedAt = 0.0;
	AdmittedPlayers.Reset();
}

FString UPlayServRooms::RequestUrlOf(const APlayerController* Player)
{
	if (Player == nullptr)
	{
		return FString();
	}
	const UNetConnection* Connection = Player->GetNetConnection();
	return Connection != nullptr ? Connection->RequestURL : FString();
}

TArray<const APlayerController*> UPlayServRooms::HostedPlayerControllers()
{
	TArray<const APlayerController*> Controllers;
	if (GEngine == nullptr)
	{
		return Controllers;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		const UWorld* World = Context.World();
		if (World == nullptr || (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE))
		{
			continue;
		}
		const ENetMode NetMode = World->GetNetMode();
		if (NetMode != NM_DedicatedServer && NetMode != NM_ListenServer)
		{
			continue;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* Controller = It->Get();
			if (Controller != nullptr)
			{
				Controllers.Add(Controller);
			}
		}
	}
	return Controllers;
}

void UPlayServRooms::HandlePreLoadMap(const FString& MapName)
{
	if (bHosting)
	{
		LoadStartedAt = Now();
	}
}

void UPlayServRooms::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (!bHosting)
	{
		return;
	}
	const double Loading = LoadStartedAt > 0.0 ? Now() - LoadStartedAt : 0.0;
	LoadStartedAt = 0.0;
	int32 Held = 0;
	for (const TPair<FString, TSharedPtr<FPlayServRoomRuntime>>& Pair : Rooms)
	{
		for (TPair<FString, double>& Seat : Pair.Value->Parked)
		{
			Seat.Value += Loading;
			++Held;
		}
	}
	if (Held > 0 && Loading > 0.0)
	{
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: the map took %.1f s to load — %d held seat(s) get that time back on their reconnect grace"), Loading, Held);
	}
	SweepLostControllers();
}

FString UPlayServRooms::ResolvePlayerId(const APlayerController* Player) const
{
	if (Player == nullptr)
	{
		return FString();
	}
	if (const FString* Admitted = AdmittedPlayers.Find(Player))
	{
		return *Admitted;
	}
	const FString Token = TicketFromOptions(ConnectionUrlOf(Player));
	FString RoomName;
	FString PlayerId;
	return !Token.IsEmpty() && FindMemberByTicket(Token, RoomName, PlayerId) ? PlayerId : FString();
}

bool UPlayServRooms::FindMemberByTicket(const FString& ReservationToken, FString& OutRoomName, FString& OutPlayerId) const
{
	if (ReservationToken.IsEmpty())
	{
		return false;
	}
	for (const TPair<FString, TSharedPtr<FPlayServRoomRuntime>>& Pair : Rooms)
	{
		if (Pair.Value->bClosing)
		{
			continue;
		}
		for (const TPair<FString, FString>& Member : Pair.Value->JoinTokens)
		{
			if (Member.Value == ReservationToken && Pair.Value->Roster.Contains(Member.Key))
			{
				OutRoomName = Pair.Key;
				OutPlayerId = Member.Key;
				return true;
			}
		}
	}
	return false;
}

const APlayerController* UPlayServRooms::FindLiveController(const FString& PlayerId, const FString& ReservationToken, const APlayerController* Except) const
{
	for (const TPair<TWeakObjectPtr<const APlayerController>, FString>& Pair : AdmittedPlayers)
	{
		const APlayerController* Admitted = Pair.Key.Get();
		if (Admitted != nullptr && Admitted != Except && Pair.Value == PlayerId)
		{
			return Admitted;
		}
	}
	if (ReservationToken.IsEmpty())
	{
		return nullptr;
	}
	for (const APlayerController* Controller : LivePlayerControllers())
	{
		if (Controller != nullptr && Controller != Except && !Controller->IsActorBeingDestroyed() && TicketFromOptions(ConnectionUrlOf(Controller)) == ReservationToken)
		{
			return Controller;
		}
	}
	return nullptr;
}

void UPlayServRooms::HoldSeat(const FString& PlayerId, const APlayerController* Dropped)
{
	for (const TPair<FString, TSharedPtr<FPlayServRoomRuntime>>& Pair : Rooms)
	{
		FPlayServRoomRuntime& Room = *Pair.Value;
		if (!Room.Roster.Contains(PlayerId))
		{
			continue;
		}
		const FString* Token = Room.JoinTokens.Find(PlayerId);
		if (const APlayerController* Live = FindLiveController(PlayerId, Token != nullptr ? *Token : FString(), Dropped))
		{
			AdmittedPlayers.Add(Live, PlayerId);
			UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ rooms: %s is still connected to %s through another controller — seat kept"), *PlayerId, *Pair.Key);
			return;
		}
		const FString RoomName = Pair.Key;
		if (ReconnectGraceSeconds <= 0.0f)
		{
			ReportPlayerLeft(RoomName, PlayerId);
			OnPlayerRemoved.Broadcast(RoomName, PlayerId, RemovedReasonGraceLapsed);
			return;
		}
		Room.Parked.Add(PlayerId, Now() + static_cast<double>(ReconnectGraceSeconds));
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: %s dropped out of %s — seat held for %.0f s"), *PlayerId, *RoomName, ReconnectGraceSeconds);
		return;
	}
}

void UPlayServRooms::SweepLostControllers()
{
	TArray<FString> Lost;
	for (TMap<TWeakObjectPtr<const APlayerController>, FString>::TIterator It(AdmittedPlayers); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			Lost.AddUnique(It.Value());
			It.RemoveCurrent();
		}
	}
	for (const FString& PlayerId : Lost)
	{
		UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ rooms: the controller %s was admitted with is gone without a logout"), *PlayerId);
		HoldSeat(PlayerId, nullptr);
	}
}

const UPlayServRooms::FVerifiedTicket* UPlayServRooms::FindVerified(const FString& ReservationToken) const
{
	return Verified.Find(ReservationToken);
}

void UPlayServRooms::HandleGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer)
{
	if (NewPlayer != nullptr)
	{
		AdmitLogin(NewPlayer, ConnectionUrlOf(NewPlayer));
	}
}

void UPlayServRooms::HandleGameModeLogout(AGameModeBase* GameMode, AController* Exiting)
{
	DropLogin(Cast<APlayerController>(Exiting));
}

void UPlayServRooms::AdmitLogin(const APlayerController* Player, const FString& RequestUrl)
{
	if (!bHosting || Player == nullptr)
	{
		return;
	}

	if (RequestUrl.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: a player logged in over no network connection (a local player on a listen server?) — nothing is reported to the platform"));
		return;
	}

	const FString Token = TicketFromOptions(RequestUrl);
	if (Token.IsEmpty())
	{
		if (bAdmissionFailOpen)
		{
			UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: a connection logged in with no platform ticket (fail-open development admission) — not reported to the platform"));
			return;
		}
		UE_LOG(LogPlayServ, Error, TEXT("PlayServ rooms: a connection logged in with no ?rsv= ticket — PreLogin must call VerifyTicket and refuse; this player is NOT in the platform's roster"));
		return;
	}

	const FVerifiedTicket* Ticket = FindVerified(Token);
	if (Ticket == nullptr)
	{
		UE_LOG(LogPlayServ, Error, TEXT("PlayServ rooms: a connection logged in with a ticket this server never verified — PreLogin must call VerifyTicket; this player is NOT in the platform's roster"));
		return;
	}

	const FString PlayerId = Ticket->PlayerId;
	const FString RoomName = Ticket->RoomName;
	const bool bResume = Ticket->bResume;
	Verified.Remove(Token);
	if (bResume)
	{
		TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(RoomName);
		if (Room == nullptr || (*Room)->bClosing || !(*Room)->Roster.Contains(PlayerId))
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: %s came back to %s after their seat was released — this player is NOT in the platform's roster"), *PlayerId, *RoomName);
			return;
		}
		AdmittedPlayers.Add(Player, PlayerId);
		(*Room)->Parked.Remove(PlayerId);
		(*Room)->IdleSince = 0.0;
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: %s resumed their seat in %s"), *PlayerId, *RoomName);
		return;
	}
	AdmittedPlayers.Add(Player, PlayerId);
	AdmitToRoom(RoomName, PlayerId, Token);
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: %s joined %s"), *PlayerId, *RoomName);
}

void UPlayServRooms::DropLogin(const APlayerController* Player)
{
	if (!bHosting || Player == nullptr)
	{
		return;
	}
	const FString PlayerId = ResolvePlayerId(Player);
	if (PlayerId.IsEmpty())
	{
		return;
	}
	AdmittedPlayers.Remove(Player);
	HoldSeat(PlayerId, Player);
}

FString UPlayServRooms::TicketFromOptions(const FString& Options)
{
	return FPlayServAdmissionTable::ParseTravelOption(Options, TEXT("rsv"));
}

FPlayServTicketVerdict UPlayServRooms::VerifyTicket(const FString& Ticket)
{
	FPlayServTicketVerdict Verdict;
	const double Current = Now();

	if (AdmissionMode == EPlayServAdmissionMode::Push)
	{
		if (Ticket.IsEmpty())
		{
			Verdict.Reason = PlayServRoomsWire::ReasonReservationInvalid;
			Verdict.ErrorMessage = TEXT("No room ticket presented (missing ?rsv= travel option)");
			return Verdict;
		}
		FPlayServPushedTicket Pushed;
		FString Reason;
		if (!Tickets.IsValid() || !Tickets->TryRedeem(Ticket, Current, FString(), Pushed, Reason))
		{
			FString MemberRoom;
			FString MemberId;
			if (FindMemberByTicket(Ticket, MemberRoom, MemberId) && FindLiveController(MemberId, Ticket, nullptr) == nullptr)
			{
				Verdict.bAccepted = true;
				Verdict.PlayerId = MemberId;
				Verdict.ReservationToken = Ticket;
				Verdict.RoomName = MemberRoom;
				RememberVerified(Verdict, Current, true);
				UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: %s is back in %s with the ticket they were admitted with — resuming their seat"), *MemberId, *MemberRoom);
				return Verdict;
			}
			Verdict.ReservationToken = Ticket;
			Verdict.Reason = Reason.IsEmpty() ? FString(PlayServRoomsWire::ReasonReservationInvalid) : Reason;
			Verdict.ErrorMessage = FString::Printf(TEXT("Room ticket refused (%s)"), *Verdict.Reason);
			return Verdict;
		}
		const TSharedPtr<FPlayServRoomRuntime>* Room = Rooms.Find(Pushed.RoomName);
		if (Room == nullptr || (*Room)->bClosing)
		{
			Verdict.ReservationToken = Ticket;
			Verdict.RoomName = Pushed.RoomName;
			Verdict.Reason = PlayServRoomsWire::ReasonRoomClosed;
			Verdict.ErrorMessage = TEXT("Room ticket refused (room_closed)");
			return Verdict;
		}
		Verdict.bAccepted = true;
		Verdict.PlayerId = Pushed.PlayerId;
		Verdict.ReservationToken = Ticket;
		Verdict.RoomName = Pushed.RoomName;
		RememberVerified(Verdict, Current);
		return Verdict;
	}

#if !UE_BUILD_SHIPPING
	if (bAdmissionFailOpen)
	{
		Verdict.bAccepted = true;
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: admitting a connection with no platform ticket (bAdmissionFailOpenInDevelopment) — it will NOT appear in the room's roster"));
		return Verdict;
	}
#endif

	Verdict.ReservationToken = Ticket;
	Verdict.Reason = PlayServRoomsWire::ReasonAdmissionUnavailable;
	Verdict.ErrorMessage = bHosting
		? TEXT("Admission unavailable: the platform confirmed no pushed-ticket admission for this server")
		: TEXT("Admission unavailable: this server is not connected to the platform");
	return Verdict;
}

void UPlayServRooms::SweepVerified(double Current)
{
	for (TMap<FString, FVerifiedTicket>::TIterator It(Verified); It; ++It)
	{
		if (Current - It.Value().VerifiedAt >= VerifiedTicketRetentionSeconds)
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: %s passed PreLogin but never completed the login; %s"), *It.Value().PlayerId, It.Value().bResume ? TEXT("the seat they came back to stays held for the reconnect grace") : TEXT("the seat is dropped"));
			It.RemoveCurrent();
		}
	}
}

void UPlayServRooms::RememberVerified(const FPlayServTicketVerdict& Verdict, double Current, bool bResume)
{
	FVerifiedTicket Entry;
	Entry.PlayerId = Verdict.PlayerId;
	Entry.RoomName = Verdict.RoomName;
	Entry.VerifiedAt = Current;
	Entry.bResume = bResume;
	Verified.Add(Verdict.ReservationToken, MoveTemp(Entry));
}

bool UPlayServRooms::ReleaseTicket(const FString& ReservationToken, const FString& Reason, const FString& Detail, bool bForget)
{
	if (ReservationToken.IsEmpty())
	{
		return false;
	}
	if (bForget && Tickets.IsValid())
	{
		Tickets->Remove(ReservationToken);
	}
	Verified.Remove(ReservationToken);
	if (!Uplink.IsValid() || Uplink->GetState() != EPlayServUplinkState::Ready)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(PlayServRoomsWire::FieldType, PlayServRoomsWire::TypeTicketRelease);
	Frame->SetStringField(PlayServRoomsWire::FieldReservationToken, ReservationToken);
	const FString ReleaseReason = Reason.IsEmpty() ? FString(PlayServRoomsWire::ReasonRoomRefused) : Reason;
	Frame->SetStringField(PlayServRoomsWire::FieldReason, ReleaseReason);
	if (!Detail.IsEmpty())
	{
		Frame->SetStringField(PlayServRoomsWire::FieldDetail, TruncateDetail(Detail));
	}
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: released an unredeemed ticket (%s)"), *ReleaseReason);
	return Uplink->SendFrame(Frame);
}

struct FPlayServJoinRound
{
	int32 Id = 0;
	FString Slug;
	FString RoomName;
	TWeakObjectPtr<APlayerController> Player;
	FPlayServJoinCallback Callback;

	double StartedAt = 0.0;
	double NextStepAt = 0.0;
	double ConnectWaitStartedAt = 0.0;
	int32 Retries = 0;

	bool bInFlight = false;
	bool bFinished = false;

	FPlayServRoomTicket Ticket;
};

namespace
{
	PlayServRoomsClientWire::FJoinLoopState LoopStateOf(const FPlayServJoinRound& Round, double Current)
	{
		PlayServRoomsClientWire::FJoinLoopState State;
		State.ElapsedSeconds = Current - Round.StartedAt;
		State.Retries = Round.Retries;
		State.ConnectWaitedSeconds = Round.ConnectWaitStartedAt > 0.0 ? Current - Round.ConnectWaitStartedAt : 0.0;
		return State;
	}

	PlayServRoomsClientWire::FJoinAnswer AnswerOf(bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
	{
		PlayServRoomsClientWire::FJoinAnswer Answer;
		Answer.bSuccess = bSuccess;
		Answer.ProblemCode = Response.ProblemCode;
		Answer.RetryAfterSeconds = Response.RetryAfterSeconds;
		Answer.Error = Error;
		return Answer;
	}
}

FPlayServError UPlayServRooms::RequireClientSession() const
{
	const UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get();
	const UPlayServAuth* Auth = Subsystem != nullptr ? Subsystem->GetAuth() : nullptr;
	if (Auth == nullptr)
	{
		return FPlayServError::SubsystemUnavailable();
	}
	if (!Auth->IsLoggedIn() || Auth->GetSessionType() != EPlayServSessionType::Client)
	{
		return FPlayServError::Make(EPlayServErrorCode::Unauthorized,
			TEXT("Rooms browse, join and create need a signed-in player: log in with PlayServ::Auth before calling them"));
	}
	return FPlayServError::Success();
}

void UPlayServRooms::Browse(const FString& Slug, const FPlayServRoomFilters& Filters, FPlayServBrowseCallback Callback)
{
	const FPlayServError SessionError = RequireClientSession();
	if (SessionError.IsError())
	{
		Callback.ExecuteIfBound(false, FPlayServBrowsePage(), SessionError);
		return;
	}
	if (!Http.IsValid())
	{
		Callback.ExecuteIfBound(false, FPlayServBrowsePage(), FPlayServError::SubsystemUnavailable());
		return;
	}
	if (Filters.Attributes.Num() > MaxBrowseAttributeFilters)
	{
		Callback.ExecuteIfBound(false, FPlayServBrowsePage(), FPlayServError::Make(EPlayServErrorCode::ValidationFailed,
			FString::Printf(TEXT("bad_request: browse accepts at most %d attributes.<key> filters"), MaxBrowseAttributeFilters)));
		return;
	}

	const FString Path = PlayServRoomsPaths::Browse(Slug) + PlayServRoomsClientWire::BuildBrowseQuery(Filters);
	Http->Request(EPlayServHttpVerb::Get, Path, nullptr,
		FPlayServV2Callback::CreateLambda([Callback](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			if (!bSuccess)
			{
				Callback.ExecuteIfBound(false, FPlayServBrowsePage(), Error);
				return;
			}
			FPlayServBrowsePage Page;
			if (!PlayServRoomsClientWire::ParseBrowsePage(Response.Json, Page))
			{
				Callback.ExecuteIfBound(false, FPlayServBrowsePage(), FPlayServError::Make(EPlayServErrorCode::ContractMismatch,
					TEXT("The browse answer carried no `data` array")));
				return;
			}
			Callback.ExecuteIfBound(true, Page, FPlayServError::Success());
		}));
}

void UPlayServRooms::Browse(const FPlayServRoomFilters& Filters, FPlayServBrowseCallback Callback)
{
	FString Slug;
	const FPlayServError SlugError = RequireRoomDefaultSlug(Slug);
	if (SlugError.IsError())
	{
		Callback.ExecuteIfBound(false, FPlayServBrowsePage(), SlugError);
		return;
	}
	Browse(Slug, Filters, MoveTemp(Callback));
}

void UPlayServRooms::JoinRoom(const FString& Slug, const FString& RoomName, APlayerController* Player, FPlayServJoinCallback Callback)
{
	const FPlayServError SessionError = RequireClientSession();
	if (SessionError.IsError())
	{
		Callback.ExecuteIfBound(false, FPlayServJoinResult(), SessionError);
		return;
	}

	TSharedPtr<FPlayServJoinRound> Round = MakeShared<FPlayServJoinRound>();
	Round->Id = NextJoinId++;
	Round->Slug = Slug;
	Round->RoomName = RoomName;
	Round->Player = Player;
	Round->Callback = MoveTemp(Callback);
	StartJoinRound(Round);
}

void UPlayServRooms::JoinRoom(const FString& RoomName, APlayerController* Player, FPlayServJoinCallback Callback)
{
	FString Slug;
	const FPlayServError SlugError = RequireRoomDefaultSlug(Slug);
	if (SlugError.IsError())
	{
		Callback.ExecuteIfBound(false, FPlayServJoinResult(), SlugError);
		return;
	}
	JoinRoom(Slug, RoomName, Player, MoveTemp(Callback));
}

void UPlayServRooms::RequestNewRoom(const TMap<FString, FString>& Attributes, FPlayServRequestNewRoomCallback Callback)
{
	FString Slug;
	const FPlayServError SlugError = RequireRoomDefaultSlug(Slug);
	if (SlugError.IsError())
	{
		Callback.ExecuteIfBound(false, FPlayServRoomListing(), SlugError);
		return;
	}
	RequestNewRoomOfType(Slug, Attributes, MoveTemp(Callback));
}

void UPlayServRooms::RequestNewRoomOfType(const FString& Slug, const TMap<FString, FString>& Attributes, FPlayServRequestNewRoomCallback Callback)
{
	const int32 AttributesBytes = PlayServRoomsValidation::AttributesByteSize(Attributes);
	if (AttributesBytes > PlayServRoomsValidation::MaxAttributesBytes)
	{
		Callback.ExecuteIfBound(false, FPlayServRoomListing(), FPlayServError::Make(EPlayServErrorCode::ValidationFailed, FString::Printf(TEXT("attributes serialize to %d bytes; the platform accepts at most %d (attributes_too_large)"), AttributesBytes, PlayServRoomsValidation::MaxAttributesBytes)));
		return;
	}
	const FString* RequestedCapacity = Attributes.Find(PlayServ::Rooms::Attributes::Capacity);
	if (RequestedCapacity != nullptr && PlayServRoomsValidation::ParseCapacityAttribute(*RequestedCapacity) == 0)
	{
		Callback.ExecuteIfBound(false, FPlayServRoomListing(), FPlayServError::Make(EPlayServErrorCode::ValidationFailed, FString::Printf(TEXT("the `capacity` attribute must be a whole number of seats, 1..%d — got '%s' (capacity_invalid)"), PlayServRoomsValidation::MaxCapacityAttribute, **RequestedCapacity)));
		return;
	}
	const FPlayServError SessionError = RequireClientSession();
	if (SessionError.IsError())
	{
		Callback.ExecuteIfBound(false, FPlayServRoomListing(), SessionError);
		return;
	}
	if (!Http.IsValid())
	{
		Callback.ExecuteIfBound(false, FPlayServRoomListing(), FPlayServError::SubsystemUnavailable());
		return;
	}

	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: asking for a new %s room (%d attribute(s)) — this waits for its server to start"), *Slug, Attributes.Num());
	Http->Request(EPlayServHttpVerb::Post, PlayServRoomsPaths::Host(Slug), PlayServRoomsClientWire::BuildHostBody(Attributes), FPlayServV2Callback::CreateLambda([Slug, Callback](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			if (!bSuccess)
			{
				UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: no new %s room — %s"), *Slug, *Error.Message);
				Callback.ExecuteIfBound(false, FPlayServRoomListing(), Error);
				return;
			}
			FPlayServRoomListing Room;
			if (!PlayServRoomsClientWire::ParseHostedRoom(Response.Json, Room))
			{
				Callback.ExecuteIfBound(false, FPlayServRoomListing(), FPlayServError::Make(EPlayServErrorCode::ContractMismatch, TEXT("The platform answered `:host` without a room_name")));
				return;
			}
			UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: room %s is up (%s:%d) — join it to take a seat"), *Room.RoomName, *Room.Connect.Host, Room.Connect.Port);
			Callback.ExecuteIfBound(true, Room, FPlayServError::Success());
		}), {}, EPlayServPlayerBearer::Attach, PlayServRoomsClientWire::HostTimeoutSeconds);
}

void UPlayServRooms::StartJoinRound(TSharedPtr<FPlayServJoinRound> Round)
{
	if (!Http.IsValid())
	{
		Round->Callback.ExecuteIfBound(false, FPlayServJoinResult(), FPlayServError::SubsystemUnavailable());
		return;
	}
	Round->StartedAt = Now();
	Joins.Add(Round->Id, Round);
	if (!MaintenanceTickerHandle.IsValid())
	{
		MaintenanceTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UPlayServRooms::TickMaintenance), MaintenanceIntervalSeconds);
	}
	SendNamedJoin(Round);
}

TSharedPtr<FPlayServJoinRound> UPlayServRooms::FindJoin(int32 Id) const
{
	const TSharedPtr<FPlayServJoinRound>* Round = Joins.Find(Id);
	return Round != nullptr ? *Round : TSharedPtr<FPlayServJoinRound>();
}

void UPlayServRooms::SendNamedJoin(TSharedPtr<FPlayServJoinRound> Round)
{
	Round->bInFlight = true;

	TWeakObjectPtr<UPlayServRooms> WeakThis(this);
	const int32 Id = Round->Id;
	Http->Request(EPlayServHttpVerb::Post, PlayServRoomsPaths::Join(Round->Slug, Round->RoomName), MakeShared<FJsonObject>(),
		FPlayServV2Callback::CreateLambda([WeakThis, Id](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			UPlayServRooms* Self = WeakThis.Get();
			TSharedPtr<FPlayServJoinRound> Round = Self != nullptr ? Self->FindJoin(Id) : nullptr;
			if (Self == nullptr || !Round.IsValid())
			{
				return;
			}
			Round->bInFlight = false;

			const double Current = Self->Now();
			PlayServRoomsClientWire::FJoinAnswer Answer = AnswerOf(bSuccess, Response, Error);
			FPlayServRoomTicket Ticket;
			if (bSuccess)
			{
				Answer.bHasTicket = PlayServRoomsClientWire::ParseMatched(Response.Json, Response.Date, Current, Ticket);
				Answer.bTicketHasConnect = Ticket.Connect.IsSet();
			}
			if (Answer.bHasTicket)
			{
				Round->Ticket = Ticket;
				Round->RoomName = Ticket.RoomName.IsEmpty() ? Round->RoomName : Ticket.RoomName;
			}
			Self->ApplyJoinDecision(Round, PlayServRoomsClientWire::DecideAfterJoin(LoopStateOf(*Round, Current), Answer), Current);
		}));
}

void UPlayServRooms::ApplyJoinDecision(TSharedPtr<FPlayServJoinRound> Round, const PlayServRoomsClientWire::FJoinStepDecision& Decision, double Current)
{
	if (Decision.bSpendsRetry)
	{
		++Round->Retries;
	}

	switch (Decision.Step)
	{
	case PlayServRoomsClientWire::EJoinStep::Deliver:
		HandleMatched(Round, Round->Ticket);
		return;

	case PlayServRoomsClientWire::EJoinStep::Stop:
		FinishJoin(Round, FPlayServRoomTicket(), false, Decision.Error);
		return;

	case PlayServRoomsClientWire::EJoinStep::JoinNamedRoom:
		if (Round->ConnectWaitStartedAt <= 0.0 && Round->Ticket.IsSet() && !Round->Ticket.Connect.IsSet())
		{
			Round->ConnectWaitStartedAt = Current;
		}
		break;
	}

	Round->NextStepAt = Current + Decision.DelaySeconds;
	if (Decision.DelaySeconds <= 0.0)
	{
		TakeJoinStep(Round);
	}
}

void UPlayServRooms::TakeJoinStep(TSharedPtr<FPlayServJoinRound> Round)
{
	Round->NextStepAt = 0.0;
	SendNamedJoin(Round);
}

void UPlayServRooms::HandleMatched(TSharedPtr<FPlayServJoinRound> Round, const FPlayServRoomTicket& Ticket)
{
	const double Current = Now();
	if (Ticket.IsExpired(Current))
	{
		if (Round->Retries < PlayServRoomsClientWire::MaxRetries)
		{
			++Round->Retries;
			Round->Ticket = FPlayServRoomTicket();
			Round->ConnectWaitStartedAt = 0.0;
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ rooms: the ticket for %s expired before travel; asking again"), *Round->RoomName);
			TakeJoinStep(Round);
			return;
		}
		FinishJoin(Round, FPlayServRoomTicket(), false,
			FPlayServError::Make(EPlayServErrorCode::Timeout,
				TEXT("The room ticket expired before the client could travel with it")));
		return;
	}

	APlayerController* Player = Round->Player.Get();
	if (Player == nullptr)
	{
		FinishJoin(Round, Ticket, false, FPlayServError::Success());
		return;
	}

	const FString TravelUrl = BuildTravelUrl(Ticket);
	if (TravelUrl.IsEmpty())
	{
		FinishJoin(Round, Ticket, false,
			FPlayServError::Make(EPlayServErrorCode::ContractMismatch,
				TEXT("The joined room carried no address to travel to")));
		return;
	}
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ rooms: travelling to room %s"), *Ticket.RoomName);
	Player->ClientTravel(TravelUrl, TRAVEL_Absolute);
	FinishJoin(Round, Ticket, true, FPlayServError::Success());
}

void UPlayServRooms::FinishJoin(TSharedPtr<FPlayServJoinRound> Round, const FPlayServRoomTicket& Ticket, bool bTravelled, const FPlayServError& Error)
{
	if (Round->bFinished)
	{
		return;
	}
	Round->bFinished = true;

	FPlayServJoinResult Result;
	Result.Ticket = Ticket;
	Result.bTravelled = bTravelled;

	FPlayServJoinCallback Callback = MoveTemp(Round->Callback);
	Joins.Remove(Round->Id);
	Callback.ExecuteIfBound(Ticket.IsSet(), Result, Error);
}

bool UPlayServRooms::TickJoins(double Current)
{
	if (Joins.Num() == 0)
	{
		return false;
	}
	TArray<int32> Ids;
	Joins.GetKeys(Ids);
	for (const int32 Id : Ids)
	{
		TSharedPtr<FPlayServJoinRound> Round = FindJoin(Id);
		if (!Round.IsValid() || Round->bFinished || Round->bInFlight)
		{
			continue;
		}
		if (Round->NextStepAt > 0.0 && Current >= Round->NextStepAt)
		{
			TakeJoinStep(Round);
		}
	}
	return Joins.Num() > 0;
}

FString UPlayServRooms::BuildTravelUrl(const FPlayServRoomTicket& Ticket)
{
	return PlayServRoomsClientWire::BuildTravelUrl(Ticket);
}
