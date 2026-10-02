// The movement keys and the mouse read straight from the system. Around a border crossing the player controller
// changes (the engine's placeholder, then the next server's), and a new controller does not see a key that was already
// held; the world does not even tick until the next server says play has begun. So the game reads the keys and the
// mouse itself, the same way on every frame, whichever controller there is.
#pragma once

#include "CoreMinimal.h"

struct FCubeKeys
{
	/** The MoveForward and MoveRight axes as the input settings map them (W = +1, S = -1; D = +1, A = -1). */
	float Forward = 0, Strafe = 0;
	bool bJump = false, bSprint = false, bSneak = false;
};

namespace CubeKeys
{
	/** False when the keyboard is not the game's to read: not Windows, or another window is in front. */
	bool Read(FCubeKeys& Out);

	/** Starts adding up the mouse's movement (Slate's raw deltas, before any controller sees them), if it was not already:
	 *  from then on it runs for the whole game, and whoever turns the view by it takes it (the pawn on every frame, a
	 *  crossing's gap while there is no pawn). A gap that took over from a pawn goes on from the very movement that pawn
	 *  never saw: the frame's movement its controller had and lost with it when the old server's actors went. */
	void StartMouse();
	/** The movement since the last call, as the engine's MouseX and MouseY axes carry it (MouseY is up). */
	FVector2D TakeMouse();
	void StopMouse();
	/** -fakemouse: a movement nobody's hand made, added where a real one is seen while the mouse is being added up. */
	void AddMouse(const FVector2D& Delta);
}
