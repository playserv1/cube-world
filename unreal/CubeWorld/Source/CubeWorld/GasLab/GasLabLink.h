// The wire between the lab's servers, in two kinds the research compares:
//   - Direct: every server listens on its game port + 100 and keeps a TCP connection to each neighbour (a DS-DS mesh;
//     the lower region dials, the higher one accepts). Frames are length-prefixed bytes.
//   - Relay: every server keeps one WebSocket to a relay that stands in for a PlayServ platform feature that does not
//     exist yet (a server-to-server channel on the game server uplink): JSON frames {"type":"s2s_send","to":4,
//     "data":"<base64>"} in, {"type":"s2s_message","from":3,"data":"..."} out, as the uplink's frames look today.
// -gaslinkdelay=<ms> holds every outgoing frame that long, to stand in for a network this machine does not have.
#pragma once

#include "CoreMinimal.h"

struct FGasLabPeer
{
	int32 Region = -1;
	FString Host;
	int32 Port = 0;
};

class IGasLabLink
{
public:
	virtual ~IGasLabLink() = default;
	virtual void Start(int32 InRegion, const TArray<FGasLabPeer>& InPeers) = 0;
	virtual void Send(int32 ToRegion, const TArray<uint8>& Bytes) = 0;
	virtual void Tick() = 0;
	virtual bool IsConnected(int32 ToRegion) const = 0;
	virtual FString Name() const = 0;

	TFunction<void(int32 /*FromRegion*/, const TArray<uint8>& /*Bytes*/)> OnReceive;
	uint64 BytesOut = 0, BytesIn = 0, FramesOut = 0, FramesIn = 0;

	static TUniquePtr<IGasLabLink> Make();
	/** The delay -gaslinkdelay asks for, seconds. */
	static double Delay();
};
