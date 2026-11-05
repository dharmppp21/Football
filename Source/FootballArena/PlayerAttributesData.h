#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "FootballTypes.h"
#include "PlayerAttributesData.generated.h"

class UCurveFloat;

// One player's attributes (spec §62), strong foot, and the curve that turns their skill into assist scale (§105). The
// default player is /Game/Football/Data/DA_PlayerAttributes_Default; UFootballActionComponent copies it into every
// request's context (§118).
UCLASS(BlueprintType)
class FOOTBALLARENA_API UPlayerAttributesData : public UPrimaryDataAsset {
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes")
	FFootballAttributes Attributes;

	// The strong foot (spec §61 weak foot); Both = two-footed.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes")
	EFootSelection PreferredFoot = EFootSelection::Right;

	// PlayerSkill (0-100) -> assist scale, for UBallAssist::ScaleForSkill. Kept here until the other tuning
	// curves of §105 arrive (Phases 8-10); then it moves to one shared tuning asset.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Assist")
	TObjectPtr<UCurveFloat> SkillToAssistScale;
};
