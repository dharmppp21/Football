#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "FootballCharacter.generated.h"

class USkeletalMeshComponent;

// BP_FootballPlayer's parent: a Character whose movement component is UFootballCharacterMovement.
UCLASS()
class FOOTBALLARENA_API AFootballCharacter : public ACharacter {
	GENERATED_BODY()

public:
	AFootballCharacter(const FObjectInitializer& ObjectInitializer);

	// The Game Animation Sample's mannequin, never drawn: its mesh and motion-matching AnimBP (SandboxCharacter_CMC_ABP) are
	// set in the Blueprint, and the visible footballer mesh copies its pose (ABP_Footballer: Retarget Pose From Mesh with
	// RTG_UEFN_To_Footballer) whenever he has no ball. Sits where the visible mesh sits and poses before it every frame.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<USkeletalMeshComponent> MotionSource;

protected:
	virtual void BeginPlay() override;
};
