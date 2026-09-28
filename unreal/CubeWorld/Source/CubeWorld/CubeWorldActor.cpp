#include "CubeWorldActor.h"
#include "CubeWorld.h"
#include "CubeWorldGameInstance.h"
#include "CubeSpec.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

namespace
{
	// Each face: its normal, the four corners as bottom-left, bottom-right, top-right, top-left seen from outside,
	// which tile it takes and its light. Coordinates are the server's: x, y on the ground, z up.
	struct FSide { int32 N[3]; int32 C[4][3]; int32 Tile; float Light; };
	const FSide Sides[6] = {
		{ { 1, 0, 0 }, { { 1, 1, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 1, 1, 1 } }, 1, 0.6f },
		{ { -1, 0, 0 }, { { 0, 0, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 0, 0, 1 } }, 1, 0.6f },
		{ { 0, 1, 0 }, { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 } }, 1, 0.8f },
		{ { 0, -1, 0 }, { { 1, 0, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, { 1, 0, 1 } }, 1, 0.8f },
		{ { 0, 0, 1 }, { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } }, 0, 1.0f },
		{ { 0, 0, -1 }, { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 } }, 2, 0.5f },
	};

	struct FMeshData
	{
		TArray<FVector> Vertices; TArray<int32> Triangles; TArray<FVector> Normals; TArray<FVector2D> UV; TArray<FColor> Colors;

		void AddQuad(const FSide& S, const FVector& Origin, float Size, const FCubeTileUV& T, float Light, bool bInvert = false)
		{
			const int32 Base = Vertices.Num();
			const FVector Corners[4] = {
				Origin + FVector(S.C[0][0], S.C[0][1], S.C[0][2]) * Size, Origin + FVector(S.C[1][0], S.C[1][1], S.C[1][2]) * Size,
				Origin + FVector(S.C[2][0], S.C[2][1], S.C[2][2]) * Size, Origin + FVector(S.C[3][0], S.C[3][1], S.C[3][2]) * Size };
			const FVector2D UVs[4] = { { T.U0, T.V1 }, { T.U1, T.V1 }, { T.U1, T.V0 }, { T.U0, T.V0 } };
			FVector Normal(S.N[0], S.N[1], S.N[2]);
			if (bInvert) Normal = -Normal;
			const uint8 L = (uint8)(255 * Light);
			for (int32 I = 0; I < 4; I++) { Vertices.Add(Corners[I]); Normals.Add(Normal); UV.Add(UVs[I]); Colors.Add(FColor(L, L, L, 255)); }
			// Unreal is left-handed: a front face has (B - A) x (C - A) pointing against the normal.
			const bool bFlip = FVector::DotProduct(FVector::CrossProduct(Corners[1] - Corners[0], Corners[2] - Corners[0]), Normal) > 0;
			const int32 Order[6] = { 0, 1, 2, 0, 2, 3 }, Flipped[6] = { 0, 2, 1, 0, 3, 2 };
			for (int32 I : (bFlip ? Flipped : Order)) Triangles.Add(Base + I);
		}

		void Commit(UProceduralMeshComponent* Mesh, int32 Section) const
		{
			Mesh->CreateMeshSection(Section, Vertices, Triangles, Normals, UV, Colors, TArray<FProcMeshTangent>(), false);
		}
	};

	UMaterialInstanceDynamic* LoadMaterial(UObject* Outer, const TCHAR* Path)
	{
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, Path);
		if (!Base)
		{
			UE_LOG(LogCubeWorld, Warning, TEXT("material %s is missing: run Scripts/MakeAssets.py"), Path);
			Base = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		return UMaterialInstanceDynamic::Create(Base, Outer);
	}
}

ACubeWorldActor::ACubeWorldActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

UProceduralMeshComponent* ACubeWorldActor::NewMesh(const FString& Name)
{
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(this, *FString::Printf(TEXT("%s_%d"), *Name, MeshCounter++));
	Mesh->SetupAttachment(RootComponent);
	Mesh->RegisterComponent();
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->bUseAsyncCooking = false;
	Mesh->SetCastShadow(false);
	return Mesh;
}

void ACubeWorldActor::BeginPlay()
{
	Super::BeginPlay();
	Game = Cast<UCubeWorldGameInstance>(GetGameInstance());
	if (!Game) return;

	Opaque = LoadMaterial(this, TEXT("/Game/Materials/M_Blocks.M_Blocks"));
	Cutout = LoadMaterial(this, TEXT("/Game/Materials/M_BlocksCutout.M_BlocksCutout"));
	Overlay = LoadMaterial(this, TEXT("/Game/Materials/M_Overlay.M_Overlay"));
	Unlit = LoadMaterial(this, TEXT("/Game/Materials/M_Unlit.M_Unlit"));
	for (UMaterialInstanceDynamic* M : { Opaque, Cutout, Overlay }) M->SetTextureParameterValue(TEXT("Atlas"), Game->Textures.Atlas);

	BuildSky();
	Game->OnWelcome.AddUObject(this, &ACubeWorldActor::RebuildAllFromWelcome);
	Game->OnCube.AddUObject(this, &ACubeWorldActor::HandleCube);
	Game->OnFall.AddUObject(this, &ACubeWorldActor::HandleFall);
	Game->OnDig.AddUObject(this, &ACubeWorldActor::HandleDig);
	RebuildAll();
}

void ACubeWorldActor::RebuildAllFromWelcome(const FCubePose&, bool)
{
	for (auto& Pair : Cracks) if (Pair.Value) Pair.Value->DestroyComponent();
	Cracks.Empty();
	for (FCubeFalling& F : Falling) if (F.Mesh) F.Mesh->DestroyComponent();
	Falling.Empty();
	RebuildAll();
}

void ACubeWorldActor::RebuildAll()
{
	TSet<FIntPoint> All;
	for (const FIntPoint& Id : Game->World.AllChunks()) All.Add(Id);
	Rebuild(All);
}

void ACubeWorldActor::Rebuild(const TSet<FIntPoint>& Ids)
{
	for (const FIntPoint& Id : Ids) RebuildChunk(Id);
}

void ACubeWorldActor::RebuildChunk(const FIntPoint& Id)
{
	const FCubeVoxelWorld& W = Game->World;
	const FCubeTextures& Tex = Game->Textures;
	UProceduralMeshComponent** Found = Chunks.Find(Id);
	UProceduralMeshComponent* Mesh = Found ? *Found : nullptr;
	if (!Mesh)
	{
		Mesh = NewMesh(TEXT("Chunk"));
		Mesh->SetMaterial(0, Opaque);
		Mesh->SetMaterial(1, Cutout);
		Chunks.Add(Id, Mesh);
	}

	FMeshData OpaqueData, CutoutData;
	const int32 C = FCubeVoxelWorld::Chunk;
	for (int32 X = Id.X * C; X < FMath::Min(W.Width, (Id.X + 1) * C); X++)
		for (int32 Y = Id.Y * C; Y < FMath::Min(W.Depth, (Id.Y + 1) * C); Y++)
			for (int32 Z = W.MinZ; Z < W.MaxZ; Z++)
			{
				const FName Kind = W.KindAt(X, Y, Z);
				if (Kind == TEXT("air")) continue;
				const FBlockDef& Block = W.Block(Kind);
				const FCubeFaces Faces = Tex.Faces(Kind, W.RegionColor(X));
				FMeshData& Data = Block.bTransparent ? CutoutData : OpaqueData;
				for (const FSide& S : Sides)
				{
					const int32 NX = X + S.N[0], NY = Y + S.N[1], NZ = Z + S.N[2];
					if (NZ < W.MinZ) continue;
					const FName Neighbour = W.Inside(NX, NY, NZ) ? W.KindAt(NX, NY, NZ) : FName(TEXT("air"));
					if (Neighbour != TEXT("air"))
					{
						const FBlockDef& Other = W.Block(Neighbour);
						if (!Other.bTransparent) continue;
						if (Block.bTransparent && Neighbour == Kind) continue;
					}
					const FName Tile = S.Tile == 0 ? Faces.Top : S.Tile == 1 ? Faces.Side : Faces.Bottom;
					Data.AddQuad(S, FVector(X, Y, Z) * CubeSpec::BlockCm, CubeSpec::BlockCm, Tex.UV(Tile), S.Light);
				}
			}
	OpaqueData.Commit(Mesh, 0);
	CutoutData.Commit(Mesh, 1);
}

void ACubeWorldActor::BuildBlockMesh(UProceduralMeshComponent* Mesh, FName Kind, FName Tile, bool bCrack)
{
	const FCubeTextures& Tex = Game->Textures;
	const FCubeFaces Faces = Tex.Faces(Kind, TEXT("green"));
	FMeshData Data;
	const float Size = bCrack ? CubeSpec::BlockCm * 1.004f : CubeSpec::BlockCm;
	const FVector Origin = bCrack ? FVector(-CubeSpec::BlockCm * 0.002f) : FVector::ZeroVector;
	for (const FSide& S : Sides)
	{
		const FName T = bCrack ? Tile : S.Tile == 0 ? Faces.Top : S.Tile == 1 ? Faces.Side : Faces.Bottom;
		Data.AddQuad(S, Origin, Size, Tex.UV(T), bCrack ? 1.f : S.Light);
	}
	Data.Commit(Mesh, 0);
}

void ACubeWorldActor::HandleCube(int32 X, int32 Y, int32 Z, FName)
{
	Rebuild(Game->World.ChunksAround(X, Y));
}

// Falling sand: the block is hidden where it will land and a loose block drops there at 0.04 a tick.
void ACubeWorldActor::HandleFall(FName Kind, int32 X, int32 Y, int32 FromZ, int32 ToZ)
{
	UProceduralMeshComponent* Mesh = NewMesh(TEXT("Falling"));
	Mesh->SetMaterial(0, Kind == TEXT("glass") || Kind == TEXT("leaves") ? Cutout : Opaque);
	BuildBlockMesh(Mesh, Kind, NAME_None, false);
	Mesh->SetRelativeLocation(FVector(X, Y, FromZ) * CubeSpec::BlockCm);
	Rebuild(Game->World.Hide(X, Y, ToZ, true));
	Falling.Add({ Mesh, X, Y, ToZ, (double)FromZ, 0.0 });
}

void ACubeWorldActor::TickFalling()
{
	for (int32 I = Falling.Num() - 1; I >= 0; I--)
	{
		FCubeFalling& F = Falling[I];
		F.VZ = (F.VZ - 0.04) * 0.98;
		F.Z += F.VZ;
		if (F.Z > F.ToZ) { F.Mesh->SetRelativeLocation(FVector(F.X, F.Y, F.Z) * CubeSpec::BlockCm); continue; }
		F.Mesh->DestroyComponent();
		Rebuild(Game->World.Hide(F.X, F.Y, F.ToZ, false));
		Falling.RemoveAt(I);
	}
}

void ACubeWorldActor::HandleDig(const FString& PlayerId, int32 X, int32 Y, int32 Z, int32 Stage)
{
	UProceduralMeshComponent** Found = Cracks.Find(PlayerId);
	if (Stage < 0)
	{
		if (Found && *Found) (*Found)->DestroyComponent();
		Cracks.Remove(PlayerId);
		return;
	}
	UProceduralMeshComponent* Mesh = Found ? *Found : nullptr;
	if (!Mesh)
	{
		Mesh = NewMesh(TEXT("Crack"));
		Mesh->SetMaterial(0, Overlay);
		Mesh->SetTranslucentSortPriority(1);
		Cracks.Add(PlayerId, Mesh);
	}
	BuildBlockMesh(Mesh, NAME_None, Game->Textures.CrackTile(Stage), true);
	Mesh->SetRelativeLocation(FVector(X, Y, Z) * CubeSpec::BlockCm);
}

// The sky: a huge box around the world, faces turned inward, one flat blue.
void ACubeWorldActor::BuildSky()
{
	UProceduralMeshComponent* Sky = NewMesh(TEXT("Sky"));
	Sky->SetMaterial(0, Opaque);
	FMeshData Data;
	const float Size = 20000.f;
	const FVector Origin = FVector(CubeSpec::Width_ * CubeSpec::BlockCm / 2, CubeSpec::Depth * CubeSpec::BlockCm / 2, 0) - FVector(Size / 2);
	const FCubeTileUV White = Game->Textures.UV(TEXT("white"));
	for (const FSide& S : Sides) Data.AddQuad(S, Origin, Size, White, 1.f, true);
	for (FColor& C : Data.Colors) C = FColor(70, 118, 220, 255);
	Data.Commit(Sky, 0);
}

void ACubeWorldActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Accumulator += FMath::Min(DeltaSeconds, 0.25f);
	while (Accumulator >= CubeSpec::TickSeconds) { TickFalling(); Accumulator -= CubeSpec::TickSeconds; }
}
