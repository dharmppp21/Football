#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "FootballTypes.h"
#include "FootballSkillData.generated.h"

class UAnimMontage;
class UBallTrajectoryData;

// One skill (DA_Skill_*, doc §10, §34): the montage it plays, its contacts, its ball path and its tuning.
// The montage's Ball Contact Window notifies time the contacts; each one points at an entry of ContactDescriptors.
// A new skill = a montage + this asset + its descriptors + a trajectory (doc §40), no code.
UCLASS(BlueprintType)
class FOOTBALLARENA_API UFootballSkillData : public UPrimaryDataAsset {
	GENERATED_BODY()
public:
	// Identity (doc §27). UFootballSkillComponent::RequestSkillByTag finds the skill by it.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	FGameplayTag SkillTag;

	// The right-foot version.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	TObjectPtr<UAnimMontage> Montage;

	// The left-foot version: a montage of the baked mirrored clip (UFootballEditorLibrary::BakeMirroredSequence),
	// with the same contact windows.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	TObjectPtr<UAnimMontage> MirroredMontage;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	EFootSelection AllowedFeet = EFootSelection::Both;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill", meta = (ClampMin = "0.1"))
	float PlayRate = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	ESkillRootMotion RootMotionMode = ESkillRootMotion::BakedTrack;

	// BakedTrack: the clip's curves holding the capsule travel (cm forward / right of the start), written by Tools/prep_skill_anims.py.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	FName RootForwardCurve = TEXT("RootFwd");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	FName RootRightCurve = TEXT("RootRight");

	// Directions the skill can go, relative to the facing (doc §28). The input direction snaps to the nearest one.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill", meta = (Bitmask, BitmaskEnum = "/Script/FootballArena.ESkillDirection"))
	int32 AllowedDirections = 0xFF;

	// Contacts (doc §5), for the right-foot version; the left-foot version mirrors their body regions.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts")
	TArray<FBallContactDescriptor> ContactDescriptors;

	// The ball path around and between the contacts (doc §13–§14).
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

	// Refused if the ball is farther than this from the player (cm).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Start")
	float MaxStartDistance = 150.f;

	// Refused if the ball would need to move faster than this to reach the first contact (cm/s).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Start")
	float MaxApproachSpeed = 600.f;

	// Seconds the skill takes at PlayRate (doc §10 Duration).
	UFUNCTION(BlueprintPure, Category = "Football|Skill")
	float GetDuration() const;

	UFUNCTION(BlueprintPure, Category = "Football|Skill")
	UAnimMontage* GetMontage(bool bMirrored) const { return bMirrored ? MirroredMontage : Montage; }

	// Descriptor Index with the tuning above applied, and its body region mirrored for the left-foot version.
	UFUNCTION(BlueprintPure, Category = "Football|Skill")
	FBallContactDescriptor GetTunedDescriptor(int32 Index, bool bMirrored) const;
};
