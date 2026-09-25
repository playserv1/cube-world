#include "Rooms/PlayServRosterHash.h"

namespace
{
	constexpr uint32 Sha256K[64] =
	{
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
	};

	uint32 RotateRight(uint32 Value, uint32 Bits)
	{
		return (Value >> Bits) | (Value << (32u - Bits));
	}

	void CompressBlock(uint32 State[8], const uint8 Block[64])
	{
		uint32 W[64];
		for (int32 Index = 0; Index < 16; ++Index)
		{
			W[Index] = (static_cast<uint32>(Block[Index * 4]) << 24)
				| (static_cast<uint32>(Block[Index * 4 + 1]) << 16)
				| (static_cast<uint32>(Block[Index * 4 + 2]) << 8)
				| static_cast<uint32>(Block[Index * 4 + 3]);
		}
		for (int32 Index = 16; Index < 64; ++Index)
		{
			const uint32 S0 = RotateRight(W[Index - 15], 7) ^ RotateRight(W[Index - 15], 18) ^ (W[Index - 15] >> 3);
			const uint32 S1 = RotateRight(W[Index - 2], 17) ^ RotateRight(W[Index - 2], 19) ^ (W[Index - 2] >> 10);
			W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
		}

		uint32 A = State[0];
		uint32 B = State[1];
		uint32 C = State[2];
		uint32 D = State[3];
		uint32 E = State[4];
		uint32 F = State[5];
		uint32 G = State[6];
		uint32 H = State[7];

		for (int32 Index = 0; Index < 64; ++Index)
		{
			const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
			const uint32 Choice = (E & F) ^ (~E & G);
			const uint32 Temp1 = H + S1 + Choice + Sha256K[Index] + W[Index];
			const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
			const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
			const uint32 Temp2 = S0 + Majority;

			H = G;
			G = F;
			F = E;
			E = D + Temp1;
			D = C;
			C = B;
			B = A;
			A = Temp1 + Temp2;
		}

		State[0] += A;
		State[1] += B;
		State[2] += C;
		State[3] += D;
		State[4] += E;
		State[5] += F;
		State[6] += G;
		State[7] += H;
	}
}

FString PlayServRosterHash::Sha256Hex(const uint8* Data, int32 Length)
{
	uint32 State[8] =
	{
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
	};

	const uint64 BitLength = static_cast<uint64>(Length) * 8u;
	int32 Offset = 0;
	while (Length - Offset >= 64)
	{
		CompressBlock(State, Data + Offset);
		Offset += 64;
	}

	uint8 Tail[128];
	FMemory::Memzero(Tail, sizeof(Tail));
	const int32 Remaining = Length - Offset;
	if (Remaining > 0)
	{
		FMemory::Memcpy(Tail, Data + Offset, Remaining);
	}
	Tail[Remaining] = 0x80;
	const int32 TailBlocks = Remaining + 1 + 8 > 64 ? 2 : 1;
	const int32 TailLength = TailBlocks * 64;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Tail[TailLength - 1 - Index] = static_cast<uint8>(BitLength >> (8 * Index));
	}
	CompressBlock(State, Tail);
	if (TailBlocks == 2)
	{
		CompressBlock(State, Tail + 64);
	}

	FString Hex;
	Hex.Reserve(64);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Hex += FString::Printf(TEXT("%08x"), State[Index]);
	}
	return Hex;
}

FString PlayServRosterHash::Sha256Hex(const FString& Utf8Source)
{
	const FTCHARToUTF8 Converter(*Utf8Source);
	return Sha256Hex(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
}

FString PlayServRosterHash::CanonicalRoster(const TArray<FString>& PlayerIds)
{
	TArray<FString> Distinct;
	for (const FString& PlayerId : PlayerIds)
	{
		if (!PlayerId.IsEmpty())
		{
			Distinct.AddUnique(PlayerId);
		}
	}
	Distinct.Sort([](const FString& Left, const FString& Right)
	{
		return FCString::Strcmp(*Left, *Right) < 0;
	});
	return FString::Join(Distinct, TEXT("\n"));
}

FString PlayServRosterHash::Compute(const TArray<FString>& PlayerIds)
{
	return Sha256Hex(CanonicalRoster(PlayerIds));
}

int32 PlayServRosterHash::Count(const TArray<FString>& PlayerIds)
{
	TSet<FString> Distinct;
	for (const FString& PlayerId : PlayerIds)
	{
		if (!PlayerId.IsEmpty())
		{
			Distinct.Add(PlayerId);
		}
	}
	return Distinct.Num();
}
