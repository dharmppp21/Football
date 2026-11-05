#include "FootballAnimInstance.h"
#include "FootballCharacter.h"
#include "GameFramework/Pawn.h"
#include "PlayerBallInteractionComponent.h"

void UFootballAnimInstance::NativeInitializeAnimation() {
	Super::NativeInitializeAnimation();
	const AFootballCharacter* Character = Cast<AFootballCharacter>(TryGetPawnOwner());
	MotionSourceMesh = Character ? Character->MotionSource : nullptr;
}

void UFootballAnimInstance::NativeUpdateAnimation(float DeltaSeconds) {
	Super::NativeUpdateAnimation(DeltaSeconds);
	const APawn* Pawn = TryGetPawnOwner();
	if (!Pawn) {
		return;
	}
	if (!MotionSourceMesh) {
		if (const AFootballCharacter* Character = Cast<AFootballCharacter>(Pawn)) {
			MotionSourceMesh = Character->MotionSource;
		}
	}
	Speed = float(Pawn->GetVelocity().Size2D());
	const UPlayerBallInteractionComponent* Dribble = Pawn->FindComponentByClass<UPlayerBallInteractionComponent>();
	const float Target = !bRetargetPoseWithBall && Dribble && Dribble->IsCarryingBall() ? 1.f : 0.f;
	Ball = FMath::FInterpConstantTo(Ball, Target, DeltaSeconds, BallBlendRate);
}
