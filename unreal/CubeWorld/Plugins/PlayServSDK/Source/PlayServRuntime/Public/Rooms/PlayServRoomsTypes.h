#pragma once

#include "CoreMinimal.h"
#include "Core/PlayServTypes.h"
#include "PlayServRoomsTypes.generated.h"

/** The transport a room's address speaks. */
UENUM(BlueprintType)
enum class EPlayServRoomTransport : uint8
{
	Udp,
	Tcp,
	Ws,
	/** WebSocket over TLS: what a platform pool machine's front offers, and what the C# servers register. */
	Wss
};

/** Where players connect to a room: the dedicated server declares it, and players read it from Browse and join results. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServRoomConnect
{
	GENERATED_BODY()

	/** Public host, an IP address or DNS name. PlayServ refuses private, loopback and link-local addresses unless the environment is set up for local development. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	FString Host;

	/** Public port, 1..65535. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	int32 Port = 0;

	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	EPlayServRoomTransport Transport = EPlayServRoomTransport::Udp;

	/** Optional connect string of your own, such as a travel URL prefix; PlayServ keeps it as is. At most 512 characters. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	FString ConnectString;

	/** Optional region label of this address, at most 32 characters. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	FString Region;

	bool IsSet() const { return !Host.IsEmpty() && Port > 0; }
};

/** The room configuration PlayServ holds for a room type. A hosting server gets it on connecting and again whenever an operator changes it. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServRoomConfig
{
	GENERATED_BODY()

	/** Most players a room holds at once. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 Capacity = 0;

	/** Seconds a room ticket stays valid before the player must use it. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 ReservationTtlSeconds = 0;

	/** Seconds a room may live; 0 for no limit. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 RoomLifetimeSeconds = 0;

	/** True when an empty room closes after RoomIdleTimeoutSeconds. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	bool bHasIdleTimeout = false;

	/** Seconds a room may stay without players, bots not counted, before it closes. Used only when bHasIdleTimeout. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 RoomIdleTimeoutSeconds = 0;

	/** Most rooms of this type open at once in this environment. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 MaxRooms = 0;

	/** Goes up with every change an operator makes. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 Version = 0;
};

/** What a dedicated server declares about one of its rooms. There is no player count: the SDK counts the players it admitted. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServRoomSnapshot
{
	GENERATED_BODY()

	/** The room's id, unique within its room type, matching ^[A-Za-z0-9][A-Za-z0-9:._-]{0,63}$. Not a display name: put that in Attributes. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	FString RoomName;

	/** Seats the room offers. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	int32 Capacity = 0;

	/** Your own state string, such as lobby or in_progress; PlayServ does not read it. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	FString State;

	/** False stops PlayServ placing new players in the room; players who already have a seat still get in. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	bool bOpen = true;

	/** Your attributes, such as display name, map or mode: text values, at most 2048 bytes as JSON. Browse filters match them by key. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	TMap<FString, FString> Attributes;

	/** The address players connect to. Required: StartRoom refuses a room without Host and Port (connect_not_declared). The SDK never works it out for you. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	FPlayServRoomConnect Connect;

	/** Optional region label of the room, at most 32 characters. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Rooms")
	FString Region;
};

/** Whether PlayServ places players in a room. */
UENUM(BlueprintType)
enum class EPlayServPlacementState : uint8
{
	Unknown,
	Open,
	Full,
	SelfClosed,
	Draining,
	SessionClosing
};

/** The hosting server's connection to PlayServ. */
UENUM(BlueprintType)
enum class EPlayServUplinkState : uint8
{
	Disconnected,
	Connecting,
	HelloSent,
	Ready
};

/** How room tickets reach this server. Only Push is supported: under None, VerifyTicket refuses every player with admission_unavailable. */
UENUM(BlueprintType)
enum class EPlayServAdmissionMode : uint8
{
	/** Not connected to PlayServ yet, or PlayServ offered a way this SDK does not support. */
	None,
	/** PlayServ sends each ticket to this server before the player arrives; VerifyTicket checks it locally. */
	Push
};

/** The verdict on one room ticket, for AGameModeBase::PreLogin. PlayServ::Rooms::VerifyTicket returns it at once, with no network call. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServTicketVerdict
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	bool bAccepted = false;

	/** The admitted player's PlayServ id (plr_*). Empty when refused, and when a development build admitted a player without a ticket. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString PlayerId;

	/** The room the ticket named. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString RoomName;

	/** The ticket (rsv_*) the player presented. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString ReservationToken;

	/** Why the ticket was refused: reservation_invalid, reservation_expired, reservation_consumed, room_mismatch, room_closed or admission_unavailable. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString Reason;

	/** The text for PreLogin's ErrorMessage; empty when accepted. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString ErrorMessage;
};

/** One room as Browse lists it. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServRoomListing
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString RoomName;

	/** Players in the room at its last report. Can be above Capacity after the capacity was lowered; 0 while the room's server is starting. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 Players = 0;

	/** The limit for new players joining, not a bound on Players. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 Capacity = 0;

	/** The state string the room's server declared. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString State;

	/** Whether PlayServ places players in the room now. Enable a Join button on this, not on State. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	EPlayServPlacementState PlacementState = EPlayServPlacementState::Unknown;

	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString Region;

	/** The room's attributes as text; a nested object or array arrives as JSON. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	TMap<FString, FString> Attributes;

	/** Where the room listens; unset (Host empty) while the room's server is starting. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FPlayServRoomConnect Connect;
};

/** One page of Browse: the rooms plus the cursor that fetches the next page. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServBrowsePage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	TArray<FPlayServRoomListing> Rooms;

	/** Pass verbatim as FPlayServRoomFilters::Cursor to fetch the next page. Empty on the last one. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString NextCursor;

	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	bool bHasMore = false;
};

/** A seat in a room, held for a limited time. The deadline runs on FPlatformTime::Seconds(), so a wrong clock on the player's machine does not shorten it. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServRoomTicket
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString RoomName;

	/** The ticket token (rsv_*), carried to the server as the ?rsv= option. Whoever holds it can take the seat, so do not log it; the SDK masks it in its own logs. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString ReservationToken;

	/** Seconds the ticket had left when the platform answered. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	int32 ExpiresInSeconds = 0;

	/** The deadline on FPlatformTime::Seconds(), from ExpiresInSeconds when the ticket arrived. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	double ExpiresAtMonotonic = 0.0;

	/** Where to travel; unset while the room's server is starting, and the SDK waits for it. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FPlayServRoomConnect Connect;

	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FString Region;

	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	TMap<FString, FString> Attributes;

	bool IsSet() const { return !ReservationToken.IsEmpty(); }

	/** True once the deadline has passed. The SDK never travels with an expired ticket. */
	bool IsExpired(double MonotonicNow) const { return ExpiresAtMonotonic > 0.0 && MonotonicNow >= ExpiresAtMonotonic; }
};

/** The outcome of a join: the ticket, and whether the SDK already travelled with it. On failure the callback's error carries PlayServ's reason. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServJoinResult
{
	GENERATED_BODY()

	/** Set on success. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	FPlayServRoomTicket Ticket;

	/** True when the SDK already called ClientTravel; false when the game travels itself. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ|Rooms")
	bool bTravelled = false;
};

/** Browse filters; every field is optional. */
struct FPlayServRoomFilters
{
	/** Unset lists every state. Set to Open for "joinable right now". */
	TOptional<EPlayServPlacementState> PlacementState;

	FString Region;

	/** Rooms whose attribute equals this text, per key. At most 4 keys: with more, Browse fails (bad_request). */
	TMap<FString, FString> Attributes;

	/** The previous page's NextCursor, verbatim. Empty starts at the first page. */
	FString Cursor;
};

/** How bad a line in a game server's logs on the platform is: the C# SDK's levels on the wire. */
UENUM(BlueprintType)
enum class EPlayServLogLevel : uint8
{
	Debug,
	Info,
	Warn,
	Error
};

/**
 * Which of this process's own UE_LOG lines ForwardLogs sends to the platform's logs. A line goes when its verbosity is
 * at least as bad as its category's entry in Categories, or, for a category not named there, as Everything.
 */
struct FPlayServLogForwarding
{
	/** Every category's lines from this verbosity on. The default sends errors (and fatal lines) only. */
	ELogVerbosity::Type Everything = ELogVerbosity::Error;

	/** Named categories and the verbosity each one's lines go from, for example the game's own category at Log. */
	TMap<FName, ELogVerbosity::Type> Categories;

	/** At most this many lines in any ten seconds; the rest are counted, and the count goes out instead. */
	int32 MaxLinesPerTenSeconds = 200;
};

/** Attribute keys with an agreed meaning. PlayServ reads no attribute itself; the SDK and your hosting image act on these. */
namespace PlayServ::Rooms::Attributes
{
	/** Seats the room offers, "1".."1000". StartRoomPlayServHosted registers the room with it; without a usable value the room configuration's capacity applies. */
	inline constexpr const TCHAR* Capacity = TEXT("capacity");

	/** The map a hosted server starts on. Your server image gets it as PLAYSERV_ROOM_ATTR_MAP and puts it on the engine command line; the SDK loads no map. */
	inline constexpr const TCHAR* Map = TEXT("map");

	/** The room's display name for a room browser. The room name itself is an id, not a title. */
	inline constexpr const TCHAR* Name = TEXT("name");
}
