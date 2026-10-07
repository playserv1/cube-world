#include "GasLab/GasLabTags.h"

namespace GasLabTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Throw, "Ability.Throw", "Throw a creeper");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Throw, "Cooldown.Throw", "The throw is cooling down");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Burning, "State.Burning", "On fire after a creeper's blast");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Blast, "Effect.Blast", "A creeper's blast");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Burning, "Effect.Burning", "The burning a blast leaves");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Damage, "Data.Damage", "SetByCaller: base damage of a blast");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Radius, "Data.Radius", "SetByCaller: radius of a blast, cm");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_BurnDamage, "Data.BurnDamage", "SetByCaller: damage per burning tick");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Creeper_Blast, "GameplayCue.Creeper.Blast", "A creeper's blast hit this actor");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Burning, "GameplayCue.Burning", "Flames while burning");
}
