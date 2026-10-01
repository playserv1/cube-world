#include "CubePhysics.h"

namespace
{
	constexpr double Eps = 1e-7;

	struct FBox3 { double Min[3]; double Max[3]; };

	FBox3 BoxOf(const FCubeBody& B)
	{
		const double Half = CubeSpec::Width / 2;
		return { { B.X - Half, B.Y - Half, B.Z }, { B.X + Half, B.Y + Half, B.Z + B.Height() } };
	}

	bool OverlapsBox(const FBox3& B, const FCubeSolidQuery& IsSolid)
	{
		for (int32 X = FMath::FloorToInt32(B.Min[0] + Eps); X <= FMath::FloorToInt32(B.Max[0] - Eps); X++)
			for (int32 Y = FMath::FloorToInt32(B.Min[1] + Eps); Y <= FMath::FloorToInt32(B.Max[1] - Eps); Y++)
				for (int32 Z = FMath::FloorToInt32(B.Min[2] + Eps); Z <= FMath::FloorToInt32(B.Max[2] - Eps); Z++)
					if (IsSolid(X, Y, Z)) return true;
		return false;
	}

	// How far the box can move along an axis before a solid block stops it.
	double Sweep(const FBox3& B, int32 Axis, double D, const FCubeSolidQuery& IsSolid)
	{
		if (D == 0) return 0;
		int32 Lo[3], Hi[3];
		for (int32 A = 0; A < 3; A++) { Lo[A] = FMath::FloorToInt32(B.Min[A] + Eps); Hi[A] = FMath::FloorToInt32(B.Max[A] - Eps); }
		const double Edge = D > 0 ? B.Max[Axis] : B.Min[Axis];
		const int32 From = FMath::FloorToInt32(FMath::Min(Edge, Edge + D) + Eps), To = FMath::FloorToInt32(FMath::Max(Edge, Edge + D) - Eps);
		const int32 O0 = Axis == 0 ? 1 : 0, O1 = Axis == 2 ? 1 : 2;
		const bool bPositive = D > 0;
		for (int32 I = From; I <= To; I++)
			for (int32 J = Lo[O0]; J <= Hi[O0]; J++)
				for (int32 K = Lo[O1]; K <= Hi[O1]; K++)
				{
					int32 P[3]; P[Axis] = I; P[O0] = J; P[O1] = K;
					if (!IsSolid(P[0], P[1], P[2])) continue;
					D = bPositive ? FMath::Min(D, I - B.Max[Axis]) : FMath::Max(D, I + 1 - B.Min[Axis]);
				}
		return FMath::Abs(D) < Eps ? 0 : D;
	}

	void Shift(FBox3& B, int32 Axis, double D) { B.Min[Axis] += D; B.Max[Axis] += D; }

	// Move: z first, then the larger horizontal axis.
	void Collide(const FCubeBody& Body, double DX, double DY, double DZ, const FCubeSolidQuery& IsSolid, double& OutX, double& OutY, double& OutZ)
	{
		FBox3 B = BoxOf(Body);
		OutZ = Sweep(B, 2, DZ, IsSolid); Shift(B, 2, OutZ);
		const bool bYFirst = FMath::Abs(DY) > FMath::Abs(DX);
		if (bYFirst) { OutY = Sweep(B, 1, DY, IsSolid); Shift(B, 1, OutY); }
		OutX = Sweep(B, 0, DX, IsSolid); Shift(B, 0, OutX);
		if (!bYFirst) { OutY = Sweep(B, 1, DY, IsSolid); Shift(B, 1, OutY); }
	}

	// Sneaking on the ground keeps the player from walking off an edge: "free" means the box, moved and
	// lowered by the step height, touches nothing, so there is no ground there.
	void BackOffFromEdge(const FCubeBody& Body, double& DX, double& DY, const FCubeSolidQuery& IsSolid)
	{
		const FBox3 B = BoxOf(Body);
		auto Free = [&](double OX, double OY)
		{
			const FBox3 T = { { B.Min[0] + OX, B.Min[1] + OY, B.Min[2] - CubeSpec::StepHeight }, { B.Max[0] + OX, B.Max[1] + OY, B.Max[2] - CubeSpec::StepHeight } };
			return !OverlapsBox(T, IsSolid);
		};
		auto Shrink = [](double V) { return FMath::Abs(V) < 0.05 ? 0.0 : V - FMath::Sign(V) * 0.05; };
		while (DX != 0 && Free(DX, 0)) DX = Shrink(DX);
		while (DY != 0 && Free(0, DY)) DY = Shrink(DY);
		while (DX != 0 && DY != 0 && Free(DX, DY)) { DX = Shrink(DX); DY = Shrink(DY); }
	}
}

void CubePhysics::Tick(FCubeBody& Body, const FCubeInput& Input, const FCubeSolidQuery& IsSolid)
{
	using namespace CubeSpec;
	Body.PX = Body.X; Body.PY = Body.Y; Body.PZ = Body.Z;
	Body.bSneaking = Input.bSneak;
	Body.bSprinting = Input.bSprint && Input.Forward > 0 && !Body.bSneaking;

	if (Input.bJump)
	{
		if (Body.bOnGround && Body.JumpDelay == 0)
		{
			Body.VZ = JumpVelocity;
			if (Body.bSprinting)
			{
				Body.VX += -FMath::Sin(Input.Yaw) * SprintJumpBoost;
				Body.VY += FMath::Cos(Input.Yaw) * SprintJumpBoost;
			}
			Body.JumpDelay = JumpDelayTicks;
		}
	}
	else Body.JumpDelay = 0;
	if (Body.JumpDelay > 0) Body.JumpDelay--;

	double Strafe = Input.Strafe * InputScale, Forward = Input.Forward * InputScale;
	if (Body.bSneaking) { Strafe *= SneakMultiplier; Forward *= SneakMultiplier; }
	const double LengthSq = Strafe * Strafe + Forward * Forward;
	if (LengthSq >= 1e-7)
	{
		double Speed = Body.bOnGround ? WalkAcceleration : AirAcceleration;
		if (Body.bSprinting) Speed *= SprintMultiplier;
		const double Scale = (LengthSq > 1 ? 1 / FMath::Sqrt(LengthSq) : 1) * Speed;
		const double SX = Strafe * Scale, SF = Forward * Scale;
		const double S = FMath::Sin(Input.Yaw), C = FMath::Cos(Input.Yaw);
		Body.VX += SX * C - SF * S;
		Body.VY += SF * C + SX * S;
	}

	double DX = Body.VX, DY = Body.VY, DZ = Body.VZ;
	if (Body.bSneaking && Body.bOnGround) BackOffFromEdge(Body, DX, DY, IsSolid);

	const bool bWasGoingDown = DZ < 0;
	double MX, MY, MZ;
	Collide(Body, DX, DY, DZ, IsSolid, MX, MY, MZ);
	Body.X += MX; Body.Y += MY; Body.Z += MZ;
	const bool bCollidedZ = FMath::Abs(MZ - DZ) > Eps;
	Body.bOnGround = bCollidedZ && bWasGoingDown;
	Body.Peak = Body.bOnGround ? Body.Z : FMath::Max(Body.Peak, Body.Z);
	Body.bHorizontalCollision = FMath::Abs(MX - DX) > Eps || FMath::Abs(MY - DY) > Eps;
	if (FMath::Abs(MX - DX) > Eps) Body.VX = 0;
	if (FMath::Abs(MY - DY) > Eps) Body.VY = 0;
	if (bCollidedZ) Body.VZ = 0;

	const double Friction = Body.bOnGround ? GroundFriction : AirFriction;
	Body.VZ = (Body.VZ - Gravity) * VerticalDrag;
	Body.VX *= Friction;
	Body.VY *= Friction;
	if (FMath::Abs(Body.VX) < MinVelocity) Body.VX = 0;
	if (FMath::Abs(Body.VY) < MinVelocity) Body.VY = 0;
	if (FMath::Abs(Body.VZ) < MinVelocity) Body.VZ = 0;
}

void CubePhysics::Knockback(FCubeBody& Body, double KX, double KY, double Strength)
{
	Body.VX = Body.VX / 2 + KX;
	Body.VY = Body.VY / 2 + KY;
	if (Body.bOnGround) Body.VZ = FMath::Min(CubeSpec::KnockbackLift, Body.VZ / 2 + Strength);
}

void CubePhysics::PushAway(FCubeBody& Body, const TArray<FCubeOtherBody>& Others)
{
	for (const FCubeOtherBody& O : Others)
	{
		if (O.Z >= Body.Z + Body.Height() || O.Z + O.Height <= Body.Z) continue;
		double DX = Body.X - O.X, DY = Body.Y - O.Y;
		const double D = FMath::Max(FMath::Abs(DX), FMath::Abs(DY));
		if (D >= CubeSpec::Width || D < 0.01) continue;
		DX /= D; DY /= D;
		const double F = FMath::Min(1.0, 1 / D);
		Body.VX += DX * F * CubeSpec::Push;
		Body.VY += DY * F * CubeSpec::Push;
	}
}

bool CubePhysics::Overlaps(const FCubeBody& Body, const FCubeSolidQuery& IsSolid)
{
	return OverlapsBox(BoxOf(Body), IsSolid);
}
