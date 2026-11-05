#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "FootballCharacterMovement.generated.h"

// The player's movement: CharacterMovement, except that while he dribbles the stick goes through the dribble first
// (UPlayerBallInteractionComponent::SteerDribbleInput) - on a sharp turn he goes to the ball before he turns.
UCLASS()
class FOOTBALLARENA_API UFootballCharacterMovement : public UCharacterMovementComponent {
	GENERATED_BODY()

public:
	// Carrying the ball the body turns where the stick asks, even while the steering still runs to the ball.
	virtual FRotator ComputeOrientToMovementRotation(const FRotator& CurrentRotation, float DeltaTime, FRotator& DeltaRotation) const override;

	virtual float GetMaxSpeed() const override;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;

	// While the stick is held, a lower speed limit (sprint released, stick eased off, agile dribbling) is reached at this
	// rate (cm/s per s) instead of the engine's one- or two-frame clamp, which popped the locomotion blend. 0 = engine behaviour.
	// Letting go of the stick still brakes as before.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking", meta = (ClampMin = "0"))
	float SpeedDropRate = 1000.f;

	// Without the ball (and bSampleMovementWithoutBall off) the facing follows the stick through a critically damped spring of
	// this stiffness (1/s): turns ease in and out and the stick's small wobbles are filtered, instead of the fixed-rate turn
	// that started and stopped in one frame and copied every wobble. RotationRate still caps the turning speed. 0 = engine behaviour.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (Rotation Settings)", meta = (ClampMin = "0"))
	float TurnStiffness = 22.f;

	// Without the ball the capsule moves like the Game Animation Sample's character (SandboxCharacter_CMC: UpdateMovement_PreCMC
	// / UpdateRotation_PreCMC), which its motion-matching clips were captured and tuned for: acceleration 800 cm/s2 (sprinting,
	// 800 -> 300 between 300 and 700 cm/s), braking 500 with the stick held / 2000 let go, ground friction 5 (sprinting, 5 -> 3
	// by 500 cm/s), and an instant turn - the motion-matching AnimBP's offset root bone and turn clips make the visible turn.
	// With the ball (or off): this component's own settings.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	bool bSampleMovementWithoutBall = true;

	// With the sample's movement, the capsule's turn: 0 = instant, as in the sample; > 0 = through the critically damped spring
	// (TurnStiffness's spring at this stiffness), which keeps a jittery stick out of the motion-matching trajectory the
	// visible facing steers toward. 22: the visible facing's jerk drops ~5x against instant (turn_free_probe), a 90-degree
	// flick still settles in ~0.6 s; 35 brings the flick's jerk back.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (Rotation Settings)", meta = (ClampMin = "0"))
	float SampleTurnStiffness = 22.f;

	virtual float GetMaxAcceleration() const override;
	virtual float GetMaxBrakingDeceleration() const override;
	virtual FRotator GetDeltaRotation(float DeltaTime) const override;

protected:
	virtual FVector ConstrainInputAcceleration(const FVector& InputAcceleration) const override;

private:
	// bSampleMovementWithoutBall and no ball held (also not by a skill).
	bool UseSampleMovement() const;
	bool IsSprintGait() const;
	// The eased limit for the CalcVelocity call in progress (0 = none).
	float HeldSpeed = 0.f;
	// The facing spring's turning speed (deg/s).
	mutable float TurnYawRate = 0.f;
};
