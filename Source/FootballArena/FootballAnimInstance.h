#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "FootballAnimInstance.generated.h"

class USkeletalMeshComponent;

// C++ parent of ABP_Footballer. Only animation data for the AnimBP belongs here (doc §23); gameplay stays in the skill/ball
// components. Locomotion: without the ball the pose is retargeted from MotionSourceMesh (the Game Animation Sample's
// motion matching); with it BS_Locomotion's ball row (the cleaned mocap loops) plays with X = Speed and Y = Ball; the two
// are blended by Ball.
UCLASS()
class FOOTBALLARENA_API UFootballAnimInstance : public UAnimInstance {
	GENERATED_BODY()

public:
	// Ground speed (cm/s) and how much the player is dribbling (0 = no ball .. 1 = carrying it, eased at BallBlendRate).
	UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
	float Speed = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
	float Ball = 0.f;

	// How fast Ball moves between 0 and 1 (1/s): 5 = a 0.2 s change between the dribble and the free run.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Locomotion", meta = (ClampMin = "0.1"))
	float BallBlendRate = 5.f;

	// Keep the retargeted MotionSource pose while carrying the ball too (Ball stays 0): for test AnimBPs whose source already
	// animates the dribble (ABP_Footballer_MB on the MotionBricks test pawn).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion")
	bool bRetargetPoseWithBall = false;

	// The hidden mannequin running the motion-matching AnimBP (AFootballCharacter::MotionSource), the no-ball pose's source.
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Locomotion")
	TObjectPtr<USkeletalMeshComponent> MotionSourceMesh;

	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
};
