#include "CubeShapes.h"
#include "ProceduralMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

FLinearColor FCubeShape::Hex(const TCHAR* Hex)
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
}

void FCubeShape::Tri(const FVector& A, const FVector& B, const FVector& C, const FVector& Out, const FLinearColor& Color, bool bTwoSided)
{
	FVector Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
	if (FVector::DotProduct(Normal, Out) < 0) Normal = -Normal;
	// Light from above, as in the world: tops 1, sides about 0.7, bottoms 0.4.
	const float Light = bShaded ? 0.7f + 0.3f * (float)Normal.Z : 1.f;
	const FColor Lit = FLinearColor(Color.R * Light, Color.G * Light, Color.B * Light, 1).ToFColor(false);
	const int32 Base = Vertices.Num();
	for (const FVector& P : { A, B, C }) { Vertices.Add(P); Normals.Add(Normal); UV.Add(FVector2D::ZeroVector); Colors.Add(Lit); }
	// Unreal is left-handed: a front face has (B - A) x (C - A) pointing against the normal.
	const bool bFlip = FVector::DotProduct(FVector::CrossProduct(B - A, C - A), Normal) > 0;
	Triangles.Append(bFlip ? TArray<int32>{ Base, Base + 2, Base + 1 } : TArray<int32>{ Base, Base + 1, Base + 2 });
	if (bTwoSided) Tri(A, B, C, -Normal, Color, false);
}

void FCubeShape::Quad(const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& Out, const FLinearColor& Color, bool bTwoSided)
{
	Tri(A, B, C, Out, Color, bTwoSided);
	Tri(A, C, D, Out, Color, bTwoSided);
}

void FCubeShape::Sphere(const FVector& Center, float Radius, int32 Segments, int32 Rings, TFunctionRef<FLinearColor(int32)> Stripe, float MaxTheta, bool bTwoSided)
{
	auto At = [&](int32 S, int32 R)
	{
		const float Phi = 2 * PI * S / Segments, Theta = MaxTheta * R / Rings;
		return Center + Radius * FVector(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
	};
	for (int32 S = 0; S < Segments; S++)
		for (int32 R = 0; R < Rings; R++)
		{
			const FVector A = At(S, R), B = At(S + 1, R), C = At(S + 1, R + 1), D = At(S, R + 1);
			const FVector Out = (A + B + C + D) / 4 - Center;
			if (R == 0) Tri(A, C, D, Out, Stripe(S), bTwoSided);
			else if (R == Rings - 1 && MaxTheta >= PI) Tri(A, B, D, Out, Stripe(S), bTwoSided);
			else Quad(A, B, C, D, Out, Stripe(S), bTwoSided);
		}
}

void FCubeShape::Sphere(const FVector& Center, float Radius, int32 Segments, int32 Rings, const FLinearColor& Color)
{
	Sphere(Center, Radius, Segments, Rings, [&Color](int32) { return Color; });
}

void FCubeShape::Cylinder(const FVector& Center, const FVector& InAxis, float Radius, float Length, int32 Segments, const FLinearColor& Color, float Start, float Arc)
{
	const FVector Axis = InAxis.GetSafeNormal();
	// U and V span the cross-section; for an axis along x, angles 0 to π cover the half above it.
	const FVector U = FMath::Abs(Axis.Z) > 0.99 ? FVector::XAxisVector : FVector::CrossProduct(FVector::ZAxisVector, Axis).GetSafeNormal();
	const FVector V = FVector::CrossProduct(Axis, U);
	const FVector Top = Center + Axis * Length / 2, Bottom = Center - Axis * Length / 2;
	auto Rim = [&](int32 I) { const float A = Start + Arc * I / Segments; return (FMath::Cos(A) * U + FMath::Sin(A) * V) * Radius; };
	for (int32 I = 0; I < Segments; I++)
	{
		const FVector R0 = Rim(I), R1 = Rim(I + 1);
		Quad(Bottom + R0, Bottom + R1, Top + R1, Top + R0, (R0 + R1) / 2, Color);
		Tri(Top, Top + R0, Top + R1, Axis, Color);
		Tri(Bottom, Bottom + R1, Bottom + R0, -Axis, Color);
	}
}

void FCubeShape::Box(const FVector& Center, const FVector& Half, const FLinearColor& Color)
{
	auto P = [&](float X, float Y, float Z) { return Center + FVector(X * Half.X, Y * Half.Y, Z * Half.Z); };
	Quad(P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), P(1, -1, 1), FVector::XAxisVector, Color);
	Quad(P(-1, 1, -1), P(-1, -1, -1), P(-1, -1, 1), P(-1, 1, 1), -FVector::XAxisVector, Color);
	Quad(P(1, 1, -1), P(-1, 1, -1), P(-1, 1, 1), P(1, 1, 1), FVector::YAxisVector, Color);
	Quad(P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1), P(-1, -1, 1), -FVector::YAxisVector, Color);
	Quad(P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1), FVector::ZAxisVector, Color);
	Quad(P(-1, 1, -1), P(1, 1, -1), P(1, -1, -1), P(-1, -1, -1), -FVector::ZAxisVector, Color);
}

void FCubeShape::Rod(const FVector& A, const FVector& B, float Thickness, const FLinearColor& Color)
{
	Cylinder((A + B) / 2, B - A, Thickness / 2, FVector::Dist(A, B), 4, Color);
}

void FCubeShape::Octahedron(const FVector& Center, float Radius, const FLinearColor& Color)
{
	const FVector Axes[6] = { { 1, 0, 0 }, { 0, 1, 0 }, { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	for (int32 I = 0; I < 4; I++)
		for (int32 Pole = 4; Pole < 6; Pole++)
		{
			const FVector A = Center + Axes[I] * Radius, B = Center + Axes[(I + 1) % 4] * Radius, C = Center + Axes[Pole] * Radius;
			Tri(A, B, C, (A + B + C) / 3 - Center, Color);
		}
}

void FCubeShape::Commit(UProceduralMeshComponent* Mesh, int32 Section) const
{
	Mesh->CreateMeshSection(Section, Vertices, Triangles, Normals, UV, Colors, TArray<FProcMeshTangent>(), false);
}

UMaterialInterface* FCubeShape::Material()
{
	UMaterialInterface* Unlit = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Unlit.M_Unlit"));
	return Unlit ? Unlit : UMaterial::GetDefaultMaterial(MD_Surface);
}
