// Player movement, one 50 ms tick at a time, as Minecraft's LivingEntity does it: accelerate from the
// input, move against the blocks, then apply friction and gravity. A port of web/physics.js into the
// server's coordinates (x, y on the ground, z up); the body position is the feet.
#pragma once

#include "CoreMinimal.h"
#include "CubeSpec.h"

struct FCubeBody
{
	double X = 0, Y = 0, Z = 0;
	double PX = 0, PY = 0, PZ = 0;
	double VX = 0, VY = 0, VZ = 0;
	bool bOnGround = false, bSneaking = false, bSprinting = false, bHorizontalCollision = false;
	int32 JumpDelay = 0;

	double Height() const { return bSneaking ? CubeSpec::SneakHeight : CubeSpec::Height; }
	double EyeHeight() const { return bSneaking ? CubeSpec::SneakEyeHeight : CubeSpec::EyeHeight; }
	void Teleport(double InX, double InY, double InZ) { X = PX = InX; Y = PY = InY; Z = PZ = InZ; VX = VY = VZ = 0; bOnGround = false; }
};

/** Yaw is Minecraft's, in radians: 0 faces +y, forward is (-sin yaw, cos yaw). */
struct FCubeInput
{
	double Forward = 0, Strafe = 0, Yaw = 0;
	bool bJump = false, bSneak = false, bSprint = false;
};

struct FCubeOtherBody
{
	double X, Y, Z, Height;
};

typedef TFunction<bool(int32, int32, int32)> FCubeSolidQuery;

namespace CubePhysics
{
	void Tick(FCubeBody& Body, const FCubeInput& Input, const FCubeSolidQuery& IsSolid);
	void Knockback(FCubeBody& Body, double KX, double KY, double Strength);
	void PushAway(FCubeBody& Body, const TArray<FCubeOtherBody>& Others);
	bool Overlaps(const FCubeBody& Body, const FCubeSolidQuery& IsSolid);
}
