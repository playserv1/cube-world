// Block textures (16 × 16 tiles in one atlas), the player skin (64 × 64) and the crack stages, all painted
// here in the Minecraft style but our own pixels. A port of web/textures.js and web/skin.js.
#pragma once

#include "CoreMinimal.h"

class UTexture2D;

struct FCubeTileUV
{
	float U0, V0, U1, V1;
};

struct FCubeFaces
{
	FName Top, Side, Bottom;
};

class FCubeTextures
{
public:
	/** Builds the atlas texture and the tile table. Call once, on the game thread. */
	void Build();

	UTexture2D* Atlas = nullptr;
	FCubeTileUV UV(FName Tile) const;
	/** The three tiles of a block kind; grass is tinted by the region colour it stands in. */
	FCubeFaces Faces(FName Kind, FName RegionColor) const;
	FName CrackTile(int32 Stage) const;

	/** A 64 × 64 skin in the standard layout, painted from the player's id. */
	static UTexture2D* PaintSkin(const FString& PlayerId);

	static const TArray<FName>& TileNames();

private:
	TMap<FName, int32> TileIndex;
	int32 Columns = 8, Rows = 1;
};

/** A CPU pixel buffer turned into a nearest-filtered texture. */
struct FCubePixels
{
	int32 Width, Height;
	TArray<FColor> Data;

	FCubePixels(int32 W, int32 H) : Width(W), Height(H) { Data.Init(FColor(0, 0, 0, 0), W * H); }
	void Set(int32 X, int32 Y, FColor C) { if (X >= 0 && Y >= 0 && X < Width && Y < Height) Data[Y * Width + X] = C; }
	FColor Get(int32 X, int32 Y) const { return Data[Y * Width + X]; }
	void Fill(int32 X, int32 Y, int32 W, int32 H, FColor C) { for (int32 J = 0; J < H; J++) for (int32 I = 0; I < W; I++) Set(X + I, Y + J, C); }
	UTexture2D* ToTexture(bool bSRGB = true) const;
};
