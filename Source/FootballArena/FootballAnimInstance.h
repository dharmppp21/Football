#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "FootballAnimInstance.generated.h"

// C++ parent of ABP_Footballer. Only animation data for the AnimBP belongs here (doc §23); gameplay
// stays in the skill/ball components. Empty until the first animation variable is needed
// (locomotion); it exists now so ABP_Footballer starts on the right parent instead of being reparented later.
UCLASS()
class FOOTBALLARENA_API UFootballAnimInstance : public UAnimInstance {
	GENERATED_BODY()
};
