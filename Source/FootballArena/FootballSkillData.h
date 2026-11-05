#pragma once

#include "CoreMinimal.h"
#include "FootballActionData.h"
#include "FootballTypes.h"
#include "FootballSkillData.generated.h"

class UBallTrajectoryData;

// One skill (DA_Skill_*, doc §10, §34): an action played as a montage with physical ball touches, by
// UFootballSkillComponent. The montage's Ball Contact Window notifies time the contacts; each one points at an entry of
// ContactDescriptors. A new skill = a montage + this asset + its descriptors (doc §40), no code.
UCLASS(BlueprintType)
class FOOTBALLARENA_API UFootballSkillData : public UFootballActionData {
	GENERATED_BODY()
public:
	UFootballSkillData() { Priority = 40; } // spec §64: a skill outranks a receive and a dribble touch, never a shot

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill", meta = (ClampMin = "0.1"))
	float PlayRate = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	ESkillRootMotion RootMotionMode = ESkillRootMotion::BakedTrack;

	// BakedTrack: the clip's curves holding the capsule travel (cm forward / right of the start), written by Tools/prep_skill_anims.py.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	FName RootForwardCurve = TEXT("RootFwd");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	FName RootRightCurve = TEXT("RootRight");

	// The ball path the bake predicts for the skill's reference run (doc §13–§14): debug preview and QA only (spec §138).
	// Nothing at runtime follows it - the ball is physical.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts")
	TObjectPtr<UBallTrajectoryData> TrajectoryProfile;

	// Tuning on top of the descriptors (doc §34). ExitSpeed > 0 replaces the Release contact's PreferredSpeed;
	// ExitAngle (degrees, + = right) turns its PreferredDirection; ContactTolerance > 0 replaces every contact's.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tuning")
	float ExitSpeed = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tuning")
	float ExitAngle = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tuning")
	float ContactTolerance = 0.f;

	// Seconds the skill takes at PlayRate (doc §10 Duration).
	UFUNCTION(BlueprintPure, Category = "Football|Skill")
	float GetDuration() const;

	// Descriptor Index with the tuning above applied, and its body region mirrored for the left-foot version.
	UFUNCTION(BlueprintPure, Category = "Football|Skill")
	FBallContactDescriptor GetTunedDescriptor(int32 Index, bool bMirrored) const;
};
