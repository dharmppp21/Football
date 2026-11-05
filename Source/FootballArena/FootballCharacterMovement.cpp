#include "FootballCharacterMovement.h"
#include "PlayerBallInteractionComponent.h"
#include "GameFramework/Character.h"

// The sample's sprint gait starts at this top speed (cm/s; the sprint button raises MaxWalkSpeed to 660).
static constexpr float SampleSprintMaxSpeed = 550.f;

bool UFootballCharacterMovement::UseSampleMovement() const {
	if (!bSampleMovementWithoutBall) {
		return false;
	}
	const UPlayerBallInteractionComponent* Dribble = GetOwner() ? GetOwner()->FindComponentByClass<UPlayerBallInteractionComponent>() : nullptr;
	return !(Dribble && Dribble->HasDribbledBall());
}

bool UFootballCharacterMovement::IsSprintGait() const {
	return MaxWalkSpeed >= SampleSprintMaxSpeed;
}

float UFootballCharacterMovement::GetMaxAcceleration() const {
	if (!UseSampleMovement()) {
		return Super::GetMaxAcceleration();
	}
	// SandboxCharacter_CMC::CalculateMaxAcceleration
	return IsSprintGait() ? FMath::GetMappedRangeValueClamped(FVector2D(300.0, 700.0), FVector2D(800.0, 300.0), Velocity.Size2D()) : 800.f;
}

float UFootballCharacterMovement::GetMaxBrakingDeceleration() const {
	if (!UseSampleMovement() || MovementMode != MOVE_Walking) {
		return Super::GetMaxBrakingDeceleration();
	}
	// SandboxCharacter_CMC::CalculateBrakingDeceleration
	return Acceleration.IsNearlyZero() ? 2000.f : 500.f;
}

FRotator UFootballCharacterMovement::GetDeltaRotation(float DeltaTime) const {
	FRotator Delta = Super::GetDeltaRotation(DeltaTime);
	if (UseSampleMovement() && IsMovingOnGround()) {
		Delta.Yaw = 360.f; // SandboxCharacter_CMC::UpdateRotation_PreCMC: RotationRate -1 on the ground = an instant turn
	}
	return Delta;
}

FVector UFootballCharacterMovement::ConstrainInputAcceleration(const FVector& InputAcceleration) const {
	const FVector Input = Super::ConstrainInputAcceleration(InputAcceleration);
	const UPlayerBallInteractionComponent* Dribble = GetOwner() ? GetOwner()->FindComponentByClass<UPlayerBallInteractionComponent>() : nullptr;
	return Dribble ? Dribble->SteerDribbleInput(Input, GetMaxSpeed()) : Input;
}

float UFootballCharacterMovement::GetMaxSpeed() const {
	return HeldSpeed > 0.f ? HeldSpeed : Super::GetMaxSpeed();
}

void UFootballCharacterMovement::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) {
	HeldSpeed = 0.f;
	if (SpeedDropRate > 0.f && IsMovingOnGround() && !Acceleration.IsZero()) {
		const float Limit = Super::GetMaxSpeed() * AnalogInputModifier;
		const float Speed = Velocity.Size2D();
		if (Speed > Limit + UE_KINDA_SMALL_NUMBER) {
			HeldSpeed = FMath::Max(Limit, Speed - SpeedDropRate * DeltaTime);
			AnalogInputModifier = 1.f;  // HeldSpeed already includes the stick
		}
	}
	if (UseSampleMovement() && IsMovingOnGround()) {
		// SandboxCharacter_CMC::CalculateGroundFriction
		Friction = IsSprintGait() ? FMath::GetMappedRangeValueClamped(FVector2D(0.0, 500.0), FVector2D(5.0, 3.0), Velocity.Size2D()) : 5.f;
	}
	Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
	HeldSpeed = 0.f;
}

FRotator UFootballCharacterMovement::ComputeOrientToMovementRotation(const FRotator& CurrentRotation, float DeltaTime, FRotator& DeltaRotation) const {
	// The path follows the ball, the facing the player's intent: no holding the old facing and no wobble while the turn touches
	// bring the ball round. Turning speed is still RotationRate.
	const UPlayerBallInteractionComponent* Dribble = GetOwner() ? GetOwner()->FindComponentByClass<UPlayerBallInteractionComponent>() : nullptr;
	const FVector Ask = CharacterOwner ? CharacterOwner->GetLastMovementInputVector() : FVector::ZeroVector;
	if (Dribble && Dribble->IsCarryingBall() && Ask.SizeSquared2D() > UE_KINDA_SMALL_NUMBER) {
		TurnYawRate = 0.f;
		return FVector(Ask.X, Ask.Y, 0.0).Rotation();
	}
	FRotator Target = Super::ComputeOrientToMovementRotation(CurrentRotation, DeltaTime, DeltaRotation);
	const bool bSample = UseSampleMovement();
	const float K = bSample ? SampleTurnStiffness : TurnStiffness;
	if (K <= 0.f || DeltaTime <= 0.f) {
		TurnYawRate = 0.f; // the sample's instant turn (GetDeltaRotation), or the engine's
		return Target;
	}
	// Critically damped spring on the yaw offset E = facing - target, solved exactly for this step (frame-rate independent):
	// E' = (E + (V + K E) dt) e^(-K dt), V' = (V - K (V + K E) dt) e^(-K dt). Capped by RotationRate (the sample's: 1080 deg/s).
	const float MaxRate = bSample ? 1080.f : RotationRate.Yaw;
	const float E = FMath::FindDeltaAngleDegrees(Target.Yaw, CurrentRotation.Yaw);
	const float Decay = FMath::Exp(-K * DeltaTime);
	const float Tmp = (TurnYawRate + K * E) * DeltaTime;
	const float MaxStep = MaxRate * DeltaTime;
	const float Step = FMath::Clamp((E + Tmp) * Decay - E, -MaxStep, MaxStep);
	TurnYawRate = FMath::Clamp((TurnYawRate - K * Tmp) * Decay, -MaxRate, MaxRate);
	Target.Yaw = CurrentRotation.Yaw + Step;
	return Target;
}
