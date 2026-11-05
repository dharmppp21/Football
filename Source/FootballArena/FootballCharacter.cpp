#include "FootballCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "FootballCharacterMovement.h"

AFootballCharacter::AFootballCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UFootballCharacterMovement>(ACharacter::CharacterMovementComponentName)) {
	MotionSource = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("MotionSource"));
	MotionSource->SetupAttachment(GetCapsuleComponent());
	MotionSource->SetHiddenInGame(true);
	MotionSource->SetCastShadow(false);
	MotionSource->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MotionSource->SetGenerateOverlapEvents(false);
	// Hidden, it must still pose: the visible mesh retargets from it.
	MotionSource->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
}

void AFootballCharacter::BeginPlay() {
	Super::BeginPlay();
	if (MotionSource && GetMesh()) {
		MotionSource->SetRelativeTransform(GetMesh()->GetRelativeTransform());
		GetMesh()->AddTickPrerequisiteComponent(MotionSource);
	}
}
