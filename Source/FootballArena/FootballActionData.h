#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "FootballTypes.h"
#include "FootballActionData.generated.h"

class UAnimMontage;

// One action (spec §3 FFootballActionDescriptor, §75 DA_*): what UFootballActionComponent::RequestAction validates, aligns
// and starts. The gameplay tag is the identity (Action.Football.<Category>.<Name>); its category is the parent tag.
// Skills derive from it (UFootballSkillData). Fields that came from the skill asset keep their saved values through the
// CoreRedirects in DefaultEngine.ini (SkillTag, MirroredMontage, AllowedFeet, MaxStartDistance).
UCLASS(BlueprintType)
class FOOTBALLARENA_API UFootballActionData : public UPrimaryDataAsset {
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Action")
	FGameplayTag ActionTag;

	// The category tag (e.g. Action.Football.Skill): the parent of ActionTag.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Action")
	FGameplayTag ActionCategory;

	// The right-foot version, and the left-foot one (a montage of the baked mirrored clip, with the same contact windows).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Action")
	TObjectPtr<UAnimMontage> Montage;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Action")
	TObjectPtr<UAnimMontage> AlternateMontage;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Action")
	EFootSelection AllowedFoot = EFootSelection::Both;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Direction")
	EActionDirectionMode DirectionMode = EActionDirectionMode::FacingSnapped;

	// FacingSnapped: directions the action can go, relative to the facing (doc §28). The input snaps to the nearest one.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Direction", meta = (Bitmask, BitmaskEnum = "/Script/FootballArena.ESkillDirection"))
	int32 AllowedDirections = 0xFF;

	// Refused (fallback) if the ball is farther than this from the player at the request (cm, ground plane).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Start", meta = (ClampMin = "0"))
	float StartDistance = 150.f;

	// The player's speed range the action may start in (cm/s); MaxSpeed 0 = no upper limit.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Start", meta = (ClampMin = "0"))
	float MinSpeed = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Start", meta = (ClampMin = "0"))
	float MaxSpeed = 0.f;

	// Contacts (spec §13), for the right-foot version; the left-foot version mirrors their body regions. The montage's
	// Ball Contact Window notifies time them.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts")
	TArray<FBallContactDescriptor> ContactDescriptors;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profiles")
	FActionAlignmentProfile AlignmentProfile;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profiles")
	FActionBallResponseProfile BallResponseProfile;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profiles")
	FActionAssistProfile AssistProfile;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profiles")
	FActionAttributeProfile AttributeProfile;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profiles")
	FActionInterruptionProfile InterruptionProfile;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profiles")
	FActionRecoveryProfile RecoveryProfile;

	// Spec §64, data-driven: a higher priority may take over a running action. Suggested order: goalkeeper catch 60 >
	// shot 50 > skill 40 > receive 30 > dribble touch 20 > incidental deflection 10.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Flow")
	int32 Priority = 0;

	// Seconds after the action ends before it may start again.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Flow", meta = (ClampMin = "0"))
	float Cooldown = 0.f;

	// Spec §63: a request that can't start yet is retried for this long (80-150 ms) before it is dropped or falls back.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Flow", meta = (ClampMin = "0"))
	float InputBufferTime = 0.12f;

	// Spec §121: what runs instead when the ball can't be reached (too far, out of the alignment limits).
	// Action.Football.Control.Dribble = keep the normal dribble going (it needs no asset).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Flow")
	FGameplayTag FallbackActionTag;

	UFUNCTION(BlueprintPure, Category = "Football|Action")
	UAnimMontage* GetMontage(bool bMirrored) const { return bMirrored ? AlternateMontage : Montage; }
};
