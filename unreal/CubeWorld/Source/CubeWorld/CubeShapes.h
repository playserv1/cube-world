// Small coloured meshes built in code for the bomb, its parachute, the explosion and the tombstone: spheres,
// cylinders, boxes and rods, flat shaded with the light baked into the vertex colours (the materials are
// unlit, as the blocks are). Units are centimetres, z up.
#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;
class UMaterialInterface;

struct FCubeShape
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UV;
	TArray<FColor> Colors;
	/** Faces lit from above as the browser's hemisphere light does; false for things that glow (the spark, the fire). */
	bool bShaded = true;

	/** A colour given as the browser gives it, "#rrggbb". */
	static FLinearColor Hex(const TCHAR* Hex);

	/** One triangle facing Out; with bTwoSided also its back. */
	void Tri(const FVector& A, const FVector& B, const FVector& C, const FVector& Out, const FLinearColor& Color, bool bTwoSided = false);
	void Quad(const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& Out, const FLinearColor& Color, bool bTwoSided = false);

	/** A sphere, or with MaxTheta < PI the cap around its top; Stripe picks the colour of each of Segments slices. */
	void Sphere(const FVector& Center, float Radius, int32 Segments, int32 Rings, TFunctionRef<FLinearColor(int32)> Stripe,
		float MaxTheta = PI, bool bTwoSided = false);
	void Sphere(const FVector& Center, float Radius, int32 Segments, int32 Rings, const FLinearColor& Color);
	/** A cylinder along Axis; with Arc < 2π only that part of it, from the angle Start. */
	void Cylinder(const FVector& Center, const FVector& Axis, float Radius, float Length, int32 Segments, const FLinearColor& Color,
		float Start = 0, float Arc = 2 * PI);
	void Box(const FVector& Center, const FVector& Half, const FLinearColor& Color);
	/** A thin square rod from A to B. */
	void Rod(const FVector& A, const FVector& B, float Thickness, const FLinearColor& Color);
	void Octahedron(const FVector& Center, float Radius, const FLinearColor& Color);

	void Commit(UProceduralMeshComponent* Mesh, int32 Section = 0) const;

	/** The material these meshes are drawn with: the vertex colour, unlit. */
	static UMaterialInterface* Material();
};
