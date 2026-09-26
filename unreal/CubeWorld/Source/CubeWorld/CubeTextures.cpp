#include "CubeTextures.h"
#include "CubeSpec.h"
#include "Engine/Texture2D.h"

namespace
{
	constexpr int32 T = CubeSpec::TextureSize;

	struct FRng
	{
		uint32 A;
		explicit FRng(uint32 Seed) : A(Seed) {}
		double Next()
		{
			A += 0x6d2b79f5u;
			uint32 X = A;
			X = (X ^ (X >> 15)) * (X | 1u);
			X ^= X + ((X ^ (X >> 7)) * (X | 61u));
			return (double)(X ^ (X >> 14)) / 4294967296.0;
		}
		int32 Int(int32 N) { return FMath::Min(N - 1, (int32)(Next() * N)); }
	};

	FColor Shade(const int32 Base[3], double K)
	{
		return FColor((uint8)FMath::Clamp(Base[0] * K, 0.0, 255.0), (uint8)FMath::Clamp(Base[1] * K, 0.0, 255.0), (uint8)FMath::Clamp(Base[2] * K, 0.0, 255.0), 255);
	}

	FColor Shade(std::initializer_list<int32> Base, double K) { int32 B[3]; int32 I = 0; for (int32 V : Base) B[I++] = V; return Shade(B, K); }

	// Fills a tile with a base colour jittered per pixel: the grainy look every block has.
	void Noise(FCubePixels& P, int32 X0, int32 Y0, const int32 Base[3], double Spread, FRng& R)
	{
		for (int32 Y = 0; Y < T; Y++) for (int32 X = 0; X < T; X++) P.Set(X0 + X, Y0 + Y, Shade(Base, 1 - Spread / 2 + R.Next() * Spread));
	}

	const TMap<FName, TArray<int32>>& GrassTints()
	{
		static const TMap<FName, TArray<int32>> Tints = { { TEXT("green"), { 93, 160, 60 } }, { TEXT("red"), { 190, 42, 38 } }, { TEXT("blue"), { 58, 150, 150 } }, { TEXT("yellow"), { 196, 186, 64 } }, { TEXT("purple"), { 140, 95, 170 } }, { TEXT("pink"), { 210, 120, 160 } } };
		return Tints;
	}

	void PaintDirt(FCubePixels& P, int32 X, int32 Y, FRng& R)
	{
		const int32 Base[3] = { 121, 85, 58 };
		Noise(P, X, Y, Base, 0.3, R);
		for (int32 I = 0; I < 14; I++) P.Set(X + R.Int(T), Y + R.Int(T), Shade({ 90, 62, 40 }, 1));
	}

	void PaintTile(FCubePixels& P, const FString& Name, int32 X, int32 Y, FRng& R)
	{
		if (Name.StartsWith(TEXT("grass_")))
		{
			TArray<FString> Parts; Name.ParseIntoArray(Parts, TEXT("_"));
			const TArray<int32>& Tint = GrassTints()[FName(*Parts[2])];
			const int32 Base[3] = { Tint[0], Tint[1], Tint[2] };
			if (Parts[1] == TEXT("top")) { Noise(P, X, Y, Base, 0.28, R); return; }
			PaintDirt(P, X, Y, R);
			for (int32 PX = 0; PX < T; PX++)
			{
				const int32 Depth = 2 + R.Int(3);
				for (int32 PY = 0; PY < Depth; PY++) P.Set(X + PX, Y + PY, Shade(Base, 0.85 + R.Next() * 0.3));
			}
			return;
		}
		if (Name == TEXT("leaves"))
		{
			const int32 Base[3] = { 58, 122, 40 };
			Noise(P, X, Y, Base, 0.4, R);
			for (int32 I = 0; I < 70; I++) P.Set(X + R.Int(T), Y + R.Int(T), FColor(0, 0, 0, 0));
			for (int32 I = 0; I < 20; I++) P.Set(X + R.Int(T), Y + R.Int(T), Shade({ 96, 168, 58 }, 1));
			return;
		}
		if (Name == TEXT("dirt")) { PaintDirt(P, X, Y, R); return; }
		if (Name == TEXT("sand")) { const int32 B[3] = { 219, 205, 160 }; Noise(P, X, Y, B, 0.14, R); return; }
		if (Name == TEXT("stone"))
		{
			const int32 B[3] = { 126, 126, 126 };
			Noise(P, X, Y, B, 0.22, R);
			for (int32 I = 0; I < 6; I++) P.Fill(X + R.Int(14), Y + R.Int(14), 2, 1, Shade({ 100, 100, 100 }, 1));
			return;
		}
		if (Name == TEXT("bedrock")) { const int32 B[3] = { 80, 80, 80 }; Noise(P, X, Y, B, 0.9, R); return; }
		if (Name == TEXT("log_side"))
		{
			for (int32 PX = 0; PX < T; PX++)
			{
				const double Tone = PX % 4 == 0 ? 0.75 : PX % 4 == 2 ? 1.05 : 0.92;
				for (int32 PY = 0; PY < T; PY++) P.Set(X + PX, Y + PY, Shade({ 104, 78, 46 }, Tone * (0.92 + R.Next() * 0.16)));
			}
			return;
		}
		if (Name == TEXT("log_top"))
		{
			const int32 B[3] = { 104, 78, 46 };
			Noise(P, X, Y, B, 0.2, R);
			P.Fill(X + 2, Y + 2, 12, 12, Shade({ 176, 138, 85 }, 1));
			P.Fill(X + 4, Y + 4, 8, 8, Shade({ 150, 114, 66 }, 1));
			P.Fill(X + 6, Y + 6, 4, 4, Shade({ 176, 138, 85 }, 1));
			P.Fill(X + 7, Y + 7, 2, 2, Shade({ 150, 114, 66 }, 1));
			return;
		}
		if (Name == TEXT("brick"))
		{
			const int32 B[3] = { 188, 176, 166 };
			Noise(P, X, Y, B, 0.1, R);
			for (int32 Row = 0; Row < 4; Row++)
			{
				const int32 Offset = Row % 2 ? 4 : 0;
				for (int32 Col = -1; Col < 3; Col++)
				{
					const int32 BX = Col * 8 + Offset;
					for (int32 PY = 0; PY < 3; PY++) for (int32 PX = 0; PX < 7; PX++)
					{
						const int32 XX = BX + PX; if (XX < 0 || XX >= T) continue;
						P.Set(X + XX, Y + Row * 4 + PY, Shade({ 150, 72, 58 }, 0.85 + R.Next() * 0.3));
					}
				}
			}
			return;
		}
		if (Name == TEXT("glass"))
		{
			P.Fill(X, Y, T, T, FColor(0, 0, 0, 0));
			const FColor Edge(255, 255, 255, 230), Streak(225, 240, 255, 140);
			P.Fill(X, Y, T, 1, Edge); P.Fill(X, Y + T - 1, T, 1, Edge); P.Fill(X, Y, 1, T, Edge); P.Fill(X + T - 1, Y, 1, T, Edge);
			for (int32 I = 0; I < 6; I++) P.Set(X + 11 - I, Y + 2 + I, Streak);
			for (int32 I = 0; I < 3; I++) P.Set(X + 6 - I, Y + 9 + I, Streak);
			return;
		}
		if (Name == TEXT("gold"))
		{
			const int32 B[3] = { 232, 190, 50 };
			Noise(P, X, Y, B, 0.1, R);
			P.Fill(X + 1, Y + 1, 14, 1, Shade({ 255, 236, 140 }, 1)); P.Fill(X + 1, Y + 1, 1, 14, Shade({ 255, 236, 140 }, 1));
			P.Fill(X + 1, Y + 14, 14, 1, Shade({ 170, 128, 20 }, 1)); P.Fill(X + 14, Y + 1, 1, 14, Shade({ 170, 128, 20 }, 1));
			P.Fill(X + 4, Y + 4, 8, 8, Shade({ 255, 225, 96 }, 1));
			P.Fill(X + 6, Y + 6, 4, 4, Shade({ 232, 190, 50 }, 1));
			return;
		}
		if (Name.StartsWith(TEXT("crack")))
		{
			const int32 Stage = FCString::Atoi(*Name.Mid(5));
			FRng CR(77);
			P.Fill(X, Y, T, T, FColor(0, 0, 0, 0));
			const int32 Lines = 2 + Stage * 2;
			for (int32 I = 0; I < Lines; I++)
			{
				int32 PX = 3 + CR.Int(10), PY = 3 + CR.Int(10);
				const int32 Length = 3 + CR.Int(4 + Stage);
				for (int32 S = 0; S < Length; S++)
				{
					P.Set(X + PX, Y + PY, FColor(0, 0, 0, 140));
					PX += (int32)FMath::Sign(CR.Next() - 0.5) * (CR.Next() < 0.6 ? 1 : 0);
					PY += (int32)FMath::Sign(CR.Next() - 0.5) * (CR.Next() < 0.6 ? 1 : 0);
					PX = FMath::Clamp(PX, 0, T - 1); PY = FMath::Clamp(PY, 0, T - 1);
				}
			}
			return;
		}
		if (Name == TEXT("white")) { P.Fill(X, Y, T, T, FColor::White); return; }
		const int32 Magenta[3] = { 255, 0, 255 };
		Noise(P, X, Y, Magenta, 0, R);
	}
}

UTexture2D* FCubePixels::ToTexture(bool bSRGB) const
{
	UTexture2D* Tex = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	Tex->Filter = TF_Nearest;
	Tex->SRGB = bSRGB;
	Tex->NeverStream = true;
	Tex->AddToRoot();
	void* Mip = Tex->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Mip, Data.GetData(), Data.Num() * sizeof(FColor));
	Tex->GetPlatformData()->Mips[0].BulkData.Unlock();
	Tex->UpdateResource();
	return Tex;
}

const TArray<FName>& FCubeTextures::TileNames()
{
	static TArray<FName> Names;
	if (Names.Num() == 0)
	{
		for (const FString Tint : { TEXT("green"), TEXT("red"), TEXT("blue"), TEXT("yellow"), TEXT("purple"), TEXT("pink") })
		{
			Names.Add(FName(*FString::Printf(TEXT("grass_top_%s"), *Tint)));
			Names.Add(FName(*FString::Printf(TEXT("grass_side_%s"), *Tint)));
		}
		for (const TCHAR* N : { TEXT("leaves"), TEXT("dirt"), TEXT("sand"), TEXT("stone"), TEXT("log_side"), TEXT("log_top"), TEXT("brick"), TEXT("glass"), TEXT("gold"), TEXT("bedrock"), TEXT("white") }) Names.Add(N);
		for (int32 I = 0; I < 10; I++) Names.Add(FName(*FString::Printf(TEXT("crack%d"), I)));
	}
	return Names;
}

void FCubeTextures::Build()
{
	const TArray<FName>& Names = TileNames();
	Rows = FMath::DivideAndRoundUp(Names.Num(), Columns);
	FCubePixels Pixels(Columns * T, Rows * T);
	FRng R(20260925);
	for (int32 I = 0; I < Names.Num(); I++)
	{
		TileIndex.Add(Names[I], I);
		PaintTile(Pixels, Names[I].ToString(), (I % Columns) * T, (I / Columns) * T, R);
	}
	Atlas = Pixels.ToTexture(true);
}

FCubeTileUV FCubeTextures::UV(FName Tile) const
{
	const int32* I = TileIndex.Find(Tile);
	const int32 Index = I ? *I : 0;
	const int32 Col = Index % Columns, Row = Index / Columns;
	// Unreal's V runs down the image: the tile's top row is V0.
	return { (float)Col / Columns, (float)Row / Rows, (float)(Col + 1) / Columns, (float)(Row + 1) / Rows };
}

FCubeFaces FCubeTextures::Faces(FName Kind, FName RegionColor) const
{
	const FString K = Kind.ToString();
	if (K == TEXT("grass"))
	{
		const FString Tint = GrassTints().Contains(RegionColor) ? RegionColor.ToString() : TEXT("green");
		return { FName(*FString::Printf(TEXT("grass_top_%s"), *Tint)), FName(*FString::Printf(TEXT("grass_side_%s"), *Tint)), TEXT("dirt") };
	}
	if (K == TEXT("wood")) return { TEXT("log_top"), TEXT("log_side"), TEXT("log_top") };
	if (TileIndex.Contains(Kind)) return { Kind, Kind, Kind };
	return { TEXT("stone"), TEXT("stone"), TEXT("stone") };
}

FName FCubeTextures::CrackTile(int32 Stage) const
{
	return FName(*FString::Printf(TEXT("crack%d"), FMath::Clamp(Stage, 0, 9)));
}

// ---- skin -----------------------------------------------------------------------------------------

namespace
{
	struct FSkinBox { int32 U, V, W, H, D; };
	struct FSkinRects { FIntRect Top, Bottom, Right, Front, Left, Back; };

	FSkinRects RectsOf(const FSkinBox& B)
	{
		auto R = [](int32 X, int32 Y, int32 W, int32 H) { return FIntRect(X, Y, X + W, Y + H); };
		return { R(B.U + B.D, B.V, B.W, B.D), R(B.U + B.D + B.W, B.V, B.W, B.D), R(B.U, B.V + B.D, B.D, B.H), R(B.U + B.D, B.V + B.D, B.W, B.H), R(B.U + B.D + B.W, B.V + B.D, B.D, B.H), R(B.U + B.D + B.W + B.D, B.V + B.D, B.W, B.H) };
	}

	uint32 HashText(const FString& Text)
	{
		uint32 H = 2166136261u;
		for (TCHAR C : Text) { H ^= (uint32)C; H *= 16777619u; }
		return H;
	}

	// HSL as CSS defines it (hue in degrees, saturation and lightness in percent).
	FColor Hsl(double H, double S, double L)
	{
		H = FMath::Fmod(FMath::Fmod(H, 360.0) + 360.0, 360.0) / 360.0; S = FMath::Clamp(S / 100.0, 0.0, 1.0); L = FMath::Clamp(L / 100.0, 0.0, 1.0);
		auto Channel = [&](double Hue)
		{
			const double Q = L < 0.5 ? L * (1 + S) : L + S - L * S, P = 2 * L - Q;
			if (Hue < 0) Hue += 1; if (Hue > 1) Hue -= 1;
			if (Hue < 1.0 / 6) return P + (Q - P) * 6 * Hue;
			if (Hue < 0.5) return Q;
			if (Hue < 2.0 / 3) return P + (Q - P) * (2.0 / 3 - Hue) * 6;
			return P;
		};
		return FColor((uint8)(Channel(H + 1.0 / 3) * 255), (uint8)(Channel(H) * 255), (uint8)(Channel(H - 1.0 / 3) * 255), 255);
	}

	void FillRect(FCubePixels& P, const FIntRect& R, FColor C) { P.Fill(R.Min.X, R.Min.Y, R.Width(), R.Height(), C); }
}

UTexture2D* FCubeTextures::PaintSkin(const FString& PlayerId)
{
	const uint32 Seed = HashText(PlayerId);
	auto Pick = [Seed](int32 N, int32 K) { return (int32)((Seed >> (K * 5)) % (uint32)N); };
	const double SkinTones[5][3] = { { 28, 45, 72 }, { 28, 40, 62 }, { 26, 45, 48 }, { 24, 40, 36 }, { 22, 38, 26 } };
	const double* Tone = SkinTones[Pick(5, 0)];
	const double ShirtHue = Pick(360, 1), PantsHue = Pick(360, 2), HairL = 12 + Pick(30, 3);
	const FColor Skin = Hsl(Tone[0], Tone[1], Tone[2]), SkinDark = Hsl(Tone[0], Tone[1], Tone[2] - 10), Hair = Hsl(25 + Pick(20, 4), 45, HairL);
	const FColor Shirt = Hsl(ShirtHue, 55, 45), ShirtDark = Hsl(ShirtHue, 55, 36), Pants = Hsl(PantsHue, 45, 38), Shoes(58, 58, 58, 255);
	const FColor Eyes[3] = { FColor(59, 107, 214, 255), FColor(76, 154, 76, 255), FColor(107, 74, 43, 255) };
	const FColor Eye = Eyes[Pick(3, 5)];

	FCubePixels P(64, 64);
	const FSkinBox HeadBox = { 0, 0, 8, 8, 8 }, BodyBox = { 16, 16, 8, 12, 4 }, RightArmBox = { 40, 16, 4, 12, 4 }, LeftArmBox = { 32, 48, 4, 12, 4 }, RightLegBox = { 0, 16, 4, 12, 4 }, LeftLegBox = { 16, 48, 4, 12, 4 };
	FRng R(Seed);
	auto Speckle = [&](const FIntRect& Rect, FColor C, int32 N) { for (int32 I = 0; I < N; I++) P.Set(Rect.Min.X + R.Int(Rect.Width()), Rect.Min.Y + R.Int(Rect.Height()), C); };

	const FSkinRects Head = RectsOf(HeadBox);
	for (const FIntRect* F : { &Head.Top, &Head.Bottom, &Head.Right, &Head.Front, &Head.Left, &Head.Back }) FillRect(P, *F, Skin);
	FillRect(P, Head.Top, Hair);
	for (const FIntRect* S : { &Head.Right, &Head.Left, &Head.Back }) P.Fill(S->Min.X, S->Min.Y, S->Width(), 4, Hair);
	P.Fill(Head.Front.Min.X, Head.Front.Min.Y, 8, 2, Hair);
	P.Set(Head.Front.Min.X + 2, Head.Front.Min.Y + 4, FColor::White); P.Set(Head.Front.Min.X + 3, Head.Front.Min.Y + 4, Eye);
	P.Set(Head.Front.Min.X + 4, Head.Front.Min.Y + 4, Eye); P.Set(Head.Front.Min.X + 5, Head.Front.Min.Y + 4, FColor::White);
	P.Fill(Head.Front.Min.X + 3, Head.Front.Min.Y + 5, 2, 1, SkinDark);
	P.Fill(Head.Front.Min.X + 3, Head.Front.Min.Y + 6, 2, 1, Hsl(Tone[0], Tone[1], Tone[2] - 22));

	const FSkinRects Body = RectsOf(BodyBox);
	for (const FIntRect* F : { &Body.Top, &Body.Bottom, &Body.Right, &Body.Front, &Body.Left, &Body.Back }) { FillRect(P, *F, Shirt); Speckle(*F, ShirtDark, 6); }
	P.Fill(Body.Front.Min.X, Body.Front.Min.Y + 11, 8, 1, ShirtDark);
	P.Fill(Body.Back.Min.X, Body.Back.Min.Y + 11, 8, 1, ShirtDark);

	for (const FSkinBox* Arm : { &RightArmBox, &LeftArmBox })
	{
		const FSkinRects A = RectsOf(*Arm);
		for (const FIntRect* F : { &A.Top, &A.Bottom, &A.Right, &A.Front, &A.Left, &A.Back }) FillRect(P, *F, Skin);
		for (const FIntRect* F : { &A.Right, &A.Front, &A.Left, &A.Back }) P.Fill(F->Min.X, F->Min.Y, F->Width(), 3, Shirt);
		FillRect(P, A.Top, Shirt);
	}
	for (const FSkinBox* Leg : { &RightLegBox, &LeftLegBox })
	{
		const FSkinRects L = RectsOf(*Leg);
		for (const FIntRect* F : { &L.Top, &L.Bottom, &L.Right, &L.Front, &L.Left, &L.Back }) { FillRect(P, *F, Pants); Speckle(*F, Hsl(PantsHue, 45, 30), 4); }
		for (const FIntRect* F : { &L.Right, &L.Front, &L.Left, &L.Back }) P.Fill(F->Min.X, F->Min.Y + 10, F->Width(), 2, Shoes);
		FillRect(P, L.Bottom, Shoes);
	}
	return P.ToTexture(true);
}
