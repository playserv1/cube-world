// The gameplay tags of the GAS lab: the creeper's blast, the burning it leaves, the throw and its cooldown.
// Native tags, so no config or asset has to carry them and every server and client knows the same set.
#pragma once

#include "NativeGameplayTags.h"

namespace GasLabTags
{
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Throw);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Throw);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Burning);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Blast);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Burning);
	/** SetByCaller magnitudes: the blast's base damage and radius, the burn's damage per tick. */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Radius);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_BurnDamage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Creeper_Blast);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Burning);
}
