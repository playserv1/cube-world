# Rooms

A **room** is one running match on the platform's registry. Your dedicated server *hosts* rooms: it opens the platform's uplink itself, registers the rooms it runs, admits players by the tickets the platform sent it, and reports who is in each one, over the same protocol PlayServ's own hosted game servers use. Your client *joins* one: it browses the rooms of a room type, asks for a seat in the one the player picked, and travels with the ticket the platform issues.

Both halves are one module, `UPlayServRooms` (`GetRooms()`, [Getting started](GettingStarted.md)), with the `PlayServ::Rooms::` namespace as the entry point. The verbs say which role: `StartHosting` / `StartRoom` / `StartRoomPlayServHosted` / `VerifyTicket` host; `Browse` / `JoinRoom` / `RequestNewRoom` join. The hosting calls are meaningful only in a dedicated-server process; the joining calls need a signed-in player.

A room's server runs one of two ways, and admission, presence and joining are the same for both:

- **You run it**, on your own machines or a host you choose, and it registers its rooms with `StartHosting` and `StartRoom` (below).
- **PlayServ hosting runs it**: a player asks for a room with `RequestNewRoom`, the platform starts one server process for that room on your room type's machine pool, and the process registers it with the single call `StartRoomPlayServHosted` ("Rooms PlayServ hosting starts").

> **Not in this version:** matchmaking (there is no `JoinGame`: the platform has no matchmaking in service, so build your room browser on `Browse`), and writing or reading game data over the uplink: a hosting server writes and reads through `PlayServ::Data`, and hears other writers' changes with `SubscribeData` ("Hearing data changes" below).

## Prerequisites (hosting)

- A `game_server` function in your platform project with a **room configuration** (capacity, ticket lifetime, room lifetime, idle timeout, max rooms); the platform requires a room lifetime or an idle timeout, so every room ends by itself. Its slug names the **room type**: every `game_server` function is one. With the PlayServ CLI: `playserv functions declare <slug> --kind game_server`, then `playserv rooms config set <slug> --capacity … --reservation-ttl … --lifetime … --max-rooms …` (the [README](../README.md) has an example); the admin console sets the room configuration too. Without a room configuration the uplink opens but no room can start (`room_config_missing`).
- A server credential: the server key in `Config/DedicatedServerGame.ini` ([Configuration](Configuration.md)), or, in a server PlayServ hosting started, the deployment token it sets as `PLAYSERV_DEPLOYMENT_TOKEN`.
- `RoomDefaultSlug` in `Config/DefaultGame.ini` ([Configuration](Configuration.md)). Every room must also declare the address players connect to ("The room’s address is yours to declare" below).

## Configuration

| Setting (`UPlayServSettings`) | Command line | Environment | Purpose |
|---|---|---|---|
| `RoomDefaultSlug` (in `DefaultGame.ini`) | `-PlayServRoomDefaultSlug=` | `PLAYSERV_EXECUTOR_SLUG` | The room type: the `game_server` slug rooms register under, and the one `Browse` and `JoinRoom` use without a slug |
| `RoomRegion` | — | — | Default region label for rooms that declare none, at most 32 characters |
| `bAdmissionFailOpenInDevelopment` | — | — | Development only ("Admitting players"); ignored in Shipping builds |
| — | — | `PLAYSERV_DEPLOYMENT_TOKEN` | The credential of a server PlayServ hosting started (environment only) |
| — | `-PlayServRoomName=` | `PLAYSERV_ROOM_NAME` | The room PlayServ hosting started this process for; read it with `GetLaunchRoomName()` |
| — | — | `PLAYSERV_ROOM_LISTEN_PORT` | The game port PlayServ hosting gave this process; `StartRoomPlayServHosted` advertises it |
| — | — | `PLAYSERV_PUBLIC_IP` | The public address of the machine the process runs on; `StartRoomPlayServHosted` advertises it |
| — | — | `PLAYSERV_ROOM_ATTRIBUTES` | The attributes the requesting player chose, as a JSON object; `StartRoomPlayServHosted` registers them |
| `BaseURL` ([Configuration](Configuration.md)) | `-PlayServBaseURL=` | `PLAYSERV_API_URL` | The platform host |
| — | `-PlayServRoomsTwins` | — | Diagnostics: use the platform's older `/matchmaking/…` room paths |

A command-line value wins over the environment, which wins over the ini. `RoomDefaultSlug`'s environment variable is `PLAYSERV_EXECUTOR_SLUG`, not a `ROOM` name, because that is the variable PlayServ hosting sets in the servers it starts. The port, the address, the attributes and the deployment token are set only by PlayServ hosting and have no command-line form.

### The room’s address is yours to declare

Every room must carry the address players connect to: set both `Snapshot.Connect.Host` and
`Snapshot.Connect.Port` before `StartRoom`. For `StartRoom` the SDK uses what you give it and works
nothing out: it reads no environment, has no host setting, and never picks a local adapter. A room
with no address is refused with `ValidationFailed` (`connect_not_declared`), so it is never
registered under an address you did not choose.

Where the address comes from depends on your deployment, and takes a few lines of your own code:

```cpp
FString ResolveRoomHost()
{
    // A container host that publishes the public address in its environment: read the variable
    // your host documents (MY_HOST_PUBLIC_IP stands in for it here).
    const FString Public = FPlatformMisc::GetEnvironmentVariable(TEXT("MY_HOST_PUBLIC_IP"));
    if (!Public.IsEmpty()) { return Public; }

    // A studio-run server: your own config value, or this machine’s first non-loopback adapter.
    bool bCanBindAll = false;
    ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    return Sockets ? Sockets->GetLocalHostAddr(*GLog, bCanBindAll)->ToString(false) : FString();
}
```

The port is usually your listen port, `GetWorld()->URL.Port` in Unreal. On a host that maps an
internal port to a different external one, advertise the external port: that is the one clients
connect to. The platform refuses a private, loopback or link-local host unless your environment is
set up for local development.

## Hosting a room

```cpp
// Once per process, e.g. from your GameMode's InitGame on the server.
PlayServ::Rooms::StartHosting(FPlayServSimpleCallback::CreateLambda(
    [](bool bOk, const FPlayServError& Err)
    {
        if (!bOk) { /* no server credential or room type configured, a refused hello, or room_config_missing */ return; }

        FPlayServRoomSnapshot Room;
        Room.RoomName = TEXT("arena-1");     // ^[A-Za-z0-9][A-Za-z0-9:._-]{0,63}$: an id, not a display name
        Room.State = TEXT("lobby");          // your own state; PlayServ does not read it
        Room.Attributes.Add(PlayServ::Rooms::Attributes::Name, TEXT("Arena 1"));   // keys: "Room attributes" below
        Room.Attributes.Add(PlayServ::Rooms::Attributes::Map, TEXT("arena"));
        // Room.Capacity 0 means the room configuration's capacity. Room.Connect is required
        // ("The room’s address is yours to declare" above); a room without it is refused.
        PlayServ::Rooms::StartRoom(Room, FPlayServSimpleCallback::CreateLambda(
            [](bool bRegistered, const FPlayServError& RegisterErr) { /* live on the platform */ }));
    }));

// What changed rides the next heartbeat (every 5 s):
PlayServ::Rooms::UpdateRoom(Room);
// Stop placing new players; players who hold a seat still get in:
UPlayServSubsystem::Get()->GetRooms()->SetRoomOpen(TEXT("arena-1"), false);
// At the end of the match, CloseRoom. When the process ends, StopHosting, but not on a map
// change ("Map changes" below).
PlayServ::Rooms::CloseRoom(TEXT("arena-1"), FPlayServSimpleCallback());
PlayServ::Rooms::StopHosting();
```

There is no player count on the snapshot: the SDK keeps the room's roster itself (below) and that roster is what the heartbeat reports.

Behind these calls: the uplink socket (`wss://<BaseURL host>/uplink`) is opened before the first room and kept for the life of the process, answering the platform's pings and reconnecting on its own; each room heartbeats every 5 s with its roster, capacity, state, attributes, address and a roster hash; the platform's room configuration is applied live when the operator changes it (`OnRoomConfigChanged`); a room whose configured lifetime or idle timeout passes is closed by the SDK (`OnRoomEnded`). `StartRoom` refuses locally, before any request, a room name outside the grammar, attributes over 2048 bytes, a region over 32 characters, or more rooms than the configuration allows.

## Admitting players

Players arrive with a travel URL of the form `host:port?rsv=<ticket>`. PlayServ hands your server the ticket before the player travels, so admission is a local lookup, with no request during the login. It is also the only thing your game has to write:

```cpp
void AMyGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
    Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
    if (!ErrorMessage.IsEmpty()) { return; }

    const FPlayServTicketVerdict Verdict =
        PlayServ::Rooms::VerifyTicket(PlayServ::Rooms::TicketFromOptions(Options));
    if (!Verdict.bAccepted)
    {
        // Verdict.Reason: reservation_invalid | reservation_expired | reservation_consumed |
        //                 room_closed | admission_unavailable
        ErrorMessage = Verdict.ErrorMessage;
    }
}
```

That is all. **Do not write a `PostLogin` or a `Logout` hook for PlayServ**: the module listens to the engine's own `FGameModeEvents` while hosting, so a player who completes the login is added to the room's roster and reported to the platform with the ticket that admits them, and a player whose connection drops keeps their seat for the reconnect grace (30 s by default, `SetReconnectGraceSeconds`) before the SDK reports them gone and fires `OnPlayerRemoved`. A player who reconnects inside the grace arrives with a new ticket and resumes their seat; after a map change, they arrive with the ticket they were first admitted with ("Map changes" below).

Two calls remain for decisions only your game can make:

```cpp
// The player is gone because your game says so (a kick, a ban, a quit): reported at once, with no grace.
PlayServ::Rooms::RemovePlayer(RoomName, PlayerId);

// The PlayServ player id behind a connection (empty for anything not admitted by ticket).
const FString PlayerId = PlayServ::Rooms::GetPlayerId(PlayerController);
```

**A player who comes in some other way.** The engine's login events cover Unreal network logins only. A player your server lets in through its own transport (a WebSocket door for browsers, a beacon, your own protocol) presents the same ticket, which you check with the same `VerifyTicket`; then admit them yourself, and report their leave yourself:

```cpp
const FPlayServTicketVerdict Verdict = PlayServ::Rooms::VerifyTicket(Ticket);
if (!Verdict.bAccepted) { /* close the connection with Verdict.Reason */ return; }
PlayServ::Rooms::AdmitVerified(Verdict);   // the platform hears the join; the player is in the roster
// ... and when that connection closes:
PlayServ::Rooms::RemovePlayer(Verdict.RoomName, Verdict.PlayerId);
```

Without `AdmitVerified` such a player plays, but the platform never hears of them: they are missing from the room's roster, the admin's player list and the operator's **Remove player**.

`OnTicketOffer` on the module class lets you refuse a player before the platform issues the ticket: return `false` with a detail string, and the platform answers the player `403 room_refused`. That, not a refusal in `PreLogin`, is the place for "this player is banned from my server": it frees the seat before anyone travels.

**When PlayServ sends your server no tickets** (`GetAdmissionMode()` is `None`), `VerifyTicket` refuses every player with `admission_unavailable`. `bAdmissionFailOpenInDevelopment` in `Config/DedicatedServerGame.ini` admits them anyway, so you can play locally against a platform that sends none; it is ignored in Shipping builds.

> **A player id always comes from PlayServ.** A player admitted that way has an empty `PlayerId` and is reported to nobody: the SDK neither invents an identity nor reads one from the travel URL, where the client could write any id. Such a player is in your game but not in the room's roster, so local play works without telling the platform anything untrue about who is in the room.

## Joining a room

The joining calls need a client session (a signed-in player, [Authentication](Authentication.md)); without one they fail with `Unauthorized`.

```cpp
// A server browser. Filters are optional; PlacementState Open lists the rooms you can join now.
FPlayServRoomFilters Filters;
Filters.PlacementState = EPlayServPlacementState::Open;
Filters.Attributes.Add(PlayServ::Rooms::Attributes::Map, TEXT("arena"));   // at most 4 attribute filters

PlayServ::Rooms::Browse(Filters, FPlayServBrowseCallback::CreateLambda(
    [](bool bOk, const FPlayServBrowsePage& Page, const FPlayServError& Err)
    {
        for (const FPlayServRoomListing& Room : Page.Rooms)
        {
            // Room.RoomName, Room.Players / Room.Capacity, Room.Region, Room.Attributes, Room.Connect
        }
        // Page.NextCursor goes back into Filters.Cursor for the next page.
    }));

// The player picked one. Pass your player controller and the SDK travels for you.
PlayServ::Rooms::JoinRoom(PickedRoomName, GetPlayerController(),
    FPlayServJoinCallback::CreateLambda(
        [](bool bOk, const FPlayServJoinResult& Result, const FPlayServError& Err)
        {
            if (!bOk) { /* Err.ProblemCode: room_not_found | room_full | room_closed | room_refused | room_unreachable */ }
            // bOk means you have a ticket; Result.bTravelled says whether the SDK already travelled with it.
        }));
```

> **These take the room type from `RoomDefaultSlug`.** If your project declares several `game_server` functions (one per mode, each with its own capacity, lifetime and `max_rooms`), name the one you mean: `Browse(TEXT("ranked"), Filters, OnPage)` and `JoinRoom(TEXT("ranked"), RoomName, PC, OnJoined)`. With no default configured and no slug given, both fail with an error naming the setting, never with an empty list.

> **A ticket lives about ten seconds.** Put your "join this room?" prompt before `JoinRoom`, never between the join and the travel. Pass `nullptr` for the player controller only when your game travels by itself; the ticket then comes back in `Result.Ticket`, and `PlayServ::Rooms::BuildTravelUrl(Ticket)` gives you `host:port?rsv=<token>`.

`JoinRoom` waits by itself, within limits: if the room's server has not published an address yet, it asks again once a second for up to 30 s, and if the room's server stalls (`503 room_unreachable`) it retries after the delay the platform asks for, up to three times. Every other refusal (`room_not_found`, `room_full`, `room_closed`, `room_refused`) is the room's own answer and comes straight back, with the platform's `ProblemCode` on the error.

## Rooms PlayServ hosting starts

With PlayServ hosting the platform runs your dedicated server for you, **one process per room**: it starts the process on a machine of your room type's pool the moment a player asks for a room, and stops it when the room ends. Your game makes one call on each side.

**The client asks.** A Host button is two calls, both yours:

```cpp
TMap<FString, FString> Settings;
Settings.Add(PlayServ::Rooms::Attributes::Map, TEXT("Canyon"));
Settings.Add(PlayServ::Rooms::Attributes::Name, TEXT("Sam's race"));
Settings.Add(PlayServ::Rooms::Attributes::Capacity, TEXT("6"));   // optional: this room's seats
Settings.Add(TEXT("tracks"), TEXT("canyon;harbour;summit"));       // your own keys come along

TWeakObjectPtr<APlayerController> WeakPC = PlayerController;
PlayServ::Rooms::RequestNewRoom(Settings, FPlayServRequestNewRoomCallback::CreateLambda([WeakPC](bool bOk, const FPlayServRoomListing& Room, const FPlayServError& Err)
{
    if (!bOk)
    {
        // Err.ProblemCode: room_host_unavailable | room_host_capacity_exhausted | room_unreachable | room_type_not_found;
        // refused locally with capacity_invalid or attributes_too_large before any request.
        return;
    }
    // The room is registered and empty: join it like any other.
    PlayServ::Rooms::JoinRoom(Room.RoomName, WeakPC.Get(), OnJoined);
}));
```

`RequestNewRoom` answers once the server it started has registered the room (typically a few seconds, at most 90 s), so show a "starting a server…" state. It seats nobody: the requester joins with the same `JoinRoom` as anyone, and the platform reserves their seat as part of the request, so the join gets it. `room_host_capacity_exhausted` means every machine in the pool is full; try again in a few seconds or tell the player. `RequestNewRoom` uses `RoomDefaultSlug` and returns the same `FPlayServRoomListing` that `Browse` does.

**The server registers.** A process PlayServ hosting started knows it (`GetLaunchRoomName()` is not empty), and makes one call where a server you run makes two:

```cpp
void AMyGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
    Super::InitGame(MapName, Options, ErrorMessage);
    if (!PlayServ::Rooms::GetLaunchRoomName().IsEmpty())
    {
        PlayServ::Rooms::StartRoomPlayServHosted(FPlayServSimpleCallback::CreateUObject(this, &AMyGameMode::OnRoomRegistered));
    }
}

void AMyGameMode::OnRoomRegistered(bool bOk, const FPlayServError& Err)
{
    if (!bOk)
    {
        FPlatformMisc::RequestExit(false);   // nobody can reach this process; exiting frees its slot
        return;
    }
    FPlayServRoomSnapshot Room;
    PlayServ::Rooms::GetRoom(PlayServ::Rooms::GetLaunchRoomName(), Room);
    GameSession->MaxPlayers = Room.Capacity;                   // the platform admits players up to this
    const FString Tracks = Room.Attributes.FindRef(TEXT("tracks"));   // the requester's choices
}
```

`StartRoomPlayServHosted` opens the uplink and registers the room as the launch describes it: the name the platform minted, the machine's public address with the port it allotted this process, the attributes the requester chose, and their `capacity` when they asked for one. You assemble nothing, and `PreLogin` is the same `VerifyTicket` as for any room. It is refused `not_a_platform_launch` in a process the platform did not start, and `launch_incomplete` when the launch names no port or no address.

**What your server image must do.** The platform runs your image with host networking and **no arguments**, and hands the room over in the environment: `PLAYSERV_ROOM_NAME`, `PLAYSERV_ROOM_LISTEN_PORT`, `PLAYSERV_PUBLIC_IP`, `PLAYSERV_ROOM_ATTRIBUTES` plus one `PLAYSERV_ROOM_ATTR_<KEY>` per attribute (the key upper-cased, every character but a letter or digit turned into `_`: `track-seed` arrives as `PLAYSERV_ROOM_ATTR_TRACK_SEED`), `PLAYSERV_DEPLOYMENT_TOKEN`, `PLAYSERV_API_URL` and `PLAYSERV_EXECUTOR_SLUG`. The SDK reads what it needs. Two things happen before the SDK runs, so only your entrypoint can do them:

- **Listen on `PLAYSERV_ROOM_LISTEN_PORT`**: pass it as `-port=`. The room advertises that port; a server listening on another one cannot be reached.
- **Start on the requested map**: `PLAYSERV_ROOM_ATTR_MAP` carries `Attributes::Map`. Put it on the engine command line, but look it up among the maps your image serves rather than passing it through: it is the player's input, and a value like `Map?game=…` would add URL options.

Also run as a user other than root (Unreal refuses to start as root on Linux x86-64), `exec` the server so the platform's `SIGTERM` reaches it, and log to stdout: when a process ends, the platform keeps the last 40 lines of its output, the only server log it keeps. A minimal entrypoint:

```sh
#!/bin/sh
set -eu
MAP=Canyon
case "${PLAYSERV_ROOM_ATTR_MAP:-}" in
    Canyon|Harbour|Summit) MAP="$PLAYSERV_ROOM_ATTR_MAP" ;;
esac
exec /server/MyGame/Binaries/Linux/MyGameServer MyGame "$MAP" -port="${PLAYSERV_ROOM_LISTEN_PORT:-7777}" -log -stdout -FullStdOutLogOutput -unattended
```

**Setting it up** is operator work, not code:

1. Declare the room type for one process per room: `playserv functions declare <slug> --kind game_server --hosting-mode process-per-room`.
2. Give it a room configuration (`playserv rooms config set`, or the admin console); its capacity is what a room gets when the request names none.
3. Build the Linux server (`BuildCookRun … -server -serverplatform=Linux -noclient … -archive`) and push its image: `playserv image push --slug <slug> --src <archive>/LinuxServer --dockerfile <your Dockerfile> --tag <tag>`. The CLI builds with Docker (buildx) on your machine.
4. Create the machine pool in the admin console (Rooms → Game servers → your game server → Hosting) with a machine size, the image tag and the number of rooms per machine.

A new server build reaches the pool as a new image tag (`playserv image push … --roll`, or the pool's form in the admin console); the rollout replaces the pool's machines one for one and **closes the rooms running on each**, so roll out between sessions.

## Room attributes

Attributes are your game's own: one level of string keys and values, at most 2048 bytes as JSON, and the platform reads none of them. A few keys have an agreed meaning, named in `PlayServ::Rooms::Attributes` so no call site spells them by hand:

| Constant | Key | Meaning |
|---|---|---|
| `PlayServ::Rooms::Attributes::Capacity` | `capacity` | The room's seats, `"1"`..`"1000"`. A room PlayServ hosting starts registers with it; `RequestNewRoom` refuses any other value (`capacity_invalid`) before sending. Absent, the room configuration's capacity applies. |
| `PlayServ::Rooms::Attributes::Map` | `map` | The map a hosted server starts on; your image's entrypoint puts it on the command line (above). |
| `PlayServ::Rooms::Attributes::Name` | `name` | A display name for your room browser. The room name is an id, not a title. |

Everything else (a track plan, a mode, a seed) is yours: put it on the request, read it on the server with `GetRoom`, filter by it in `Browse`.

## Map changes

Hosting belongs to the process, not to the world: a room keeps running across `ServerTravel`, with its heartbeat, roster and tickets. Two rules follow:

- **Call `StopHosting` only when the process ends.** A game mode's `EndPlay` also runs on a map change; skip `StopHosting` for `EEndPlayReason::LevelTransition`, or the room closes in the middle of the session.
- **The next map's game mode takes the room over instead of starting it again.** `StartRoom` and `StartRoomPlayServHosted` refuse a room this process already registered; `GetRoom` tells you whether it exists and returns it, and `UpdateRoom` changes it.

**Your players keep their seats**, whichever way you travel, with nothing to write:

- **Hard travel** (`bUseSeamlessTravel = false`) disconnects every client, and each one comes back with the travel URL it first used (the engine keeps `?rsv=`), so the ticket your `PreLogin` passes to `VerifyTicket` was redeemed when the player first joined. For a member of the room who is not connected, `VerifyTicket` accepts it and resumes their seat; the platform is told nothing, because as far as the room is concerned the player never left. The same ticket from a second connection while the player is still connected is refused `reservation_consumed`.
- **Seamless travel** keeps the connections, and UE 5.8 hands every one of them to a new player controller. `GetPlayerId` answers for the new controller, and a player who drops afterwards is held for the grace and reported like any other.
- **The reconnect grace does not run while a map loads**, so a slow load costs nobody their seat. `OnPlayerRemoved` fires only for a player who did not come back within the grace.

```cpp
void AMyGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Reason != EEndPlayReason::LevelTransition)
    {
        PlayServ::Rooms::StopHosting();
    }
    Super::EndPlay(Reason);
}

// The next map's game mode:
FPlayServRoomSnapshot Room;
if (PlayServ::Rooms::GetRoom(RoomName, Room))
{
    Room.State = TEXT("race 2/3");
    PlayServ::Rooms::UpdateRoom(Room);
}
```

## Hearing data changes

A hosting server can hear every change to an entity's records as it happens, whoever makes it: another server, a cloud function, an operator in the admin. The platform sends each upsert and each delete over the uplink, as it does to the C# SDK's `Platform.RuntimeData.Subscribe`:

```cpp
UPlayServRooms* Rooms = UPlayServSubsystem::Get()->GetRooms();
Rooms->OnDataUpdate.AddUObject(this, &AMyGameMode::HandleDataUpdate);   // bind first
Rooms->OnDataSubscribed.AddUObject(this, &AMyGameMode::HandleDataSubscribed);
PlayServ::Rooms::SubscribeData(TEXT("WorldCube"), TEXT("field:key"));   // the entity, and the field that keys a record

void AMyGameMode::HandleDataUpdate(const FPlayServDataUpdate& Update)
{
    // Update.Entity, Update.Id, Update.Op ("upsert" or "delete"), Update.Data (the record's fields, or null)
    if (Update.IsDelete()) { /* the record is gone */ }
}
```

The subscription goes out as soon as the uplink is ready and again on every new uplink socket, and `OnDataSubscribed(Entity)` fires each time it goes out. The platform does not send again what changed while no subscription was in place (before the first one, or while the uplink reconnected), so a server that must not miss a change reads the records again from `OnDataSubscribed`. `UnsubscribeData(Entity)` stops it. `Display` logs each subscription and the first change of each entity with the fields it carried.

## Events

`OnUplinkStateChanged`, `OnRoomConfigChanged`, `OnRoomPlacementChanged`, `OnRoomEnded(RoomName, Reason)` and `OnPlayerRemoved(RoomName, PlayerId, Reason)` are Blueprint-assignable on `UPlayServRooms`, alongside the state queries (`IsHosting`, `GetRoomNames`, `GetRoom`, `GetRoomPlayerCount`, …); `OnDataUpdate` and `OnDataSubscribed` ("Hearing data changes") are C++ delegates. Operations are C++ only. `Reason` values are the platform's vocabulary: `lifetime`, `idle`, `room_owned_by_other_instance`, `room_type_not_found`, `reconnect_grace_lapsed`, `removed_by_game`, `room_closed`, `reservation_expired`, and so on.

## Logging

Same policy as [Errors and logging](Errors.md): the credential, the session token and the reservation token are never logged by the SDK, and the hello is logged by shape only. `Display` shows the start, the applied room configuration, each room's registration and each join and drop; `Warning` shows a failed heartbeat, a refused join and an admission mode this plugin cannot serve; `Error` shows a refused hello, a missing room configuration, and a connection that reached `PostLogin` without a ticket this server verified.

> **One thing the SDK cannot hide:** the engine's own `LogNet` prints the whole login URL, ticket included, on every dedicated server. **Filter or suppress `LogNet` in your shipping server logs.** The platform takes the token only once, within seconds, but your server keeps it as the player's identity for as long as they are in the room: while no connection stands for them (during the reconnect grace, or while a map loads), whoever presents that token resumes their seat ("Map changes" above). A token read out of a log is worth a seat for the whole match, not for ten seconds.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
