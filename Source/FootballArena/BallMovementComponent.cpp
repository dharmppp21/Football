#include "BallMovementComponent.h"
#include "BallStateComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"

UBallMovementComponent::UBallMovementComponent() {
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics; // forces and velocities set here go into this frame's physics step
}

void UBallMovementComponent::BeginPlay() {
	Super::BeginPlay();
	BallState = GetOwner()->FindComponentByClass<UBallStateComponent>();
}

UPrimitiveComponent* UBallMovementComponent::GetBody() const {
	return Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
}

float UBallMovementComponent::GetBallRadius() const {
	const USphereComponent* Sphere = Cast<USphereComponent>(GetBody());
	return Sphere ? Sphere->GetScaledSphereRadius() : 11.f;
}

FVector UBallMovementComponent::GetBallVelocity() const {
	const UPrimitiveComponent* Body = GetBody();
	return Body ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector;
}

void UBallMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UPrimitiveComponent* Body = GetBody();
	if (!Body) {
		return;
	}
	if (bApplyPhysicsExtras && Body->IsSimulatingPhysics()) {
		ApplyPhysicsExtras(Body);
	}
	UpdateGrounded(Body);
	UpdateAutoState();
	if (GetWorld()->GetTimeSeconds() < RecordUntil) {
		RecordSample(Body);
	}
}

void UBallMovementComponent::ApplyPhysicsExtras(UPrimitiveComponent* Body) const {
	// Magnus (BP_Football Tick, first branch), only in the air (spec §92 early-out). bGrounded is last tick's ground check,
	// so a just-kicked ball counts by its rise speed until that check sees it leave.
	const FVector Spin = Body->GetPhysicsAngularVelocityInDegrees();
	const FVector Velocity = Body->GetPhysicsLinearVelocity();
	if ((!bGrounded || Velocity.Z > MagnusMinRiseSpeed) && Spin.Size() >= MagnusMinSpin) {
		// 0.01745 is the Blueprint's own degrees-to-radians factor.
		Body->AddForce(FVector::CrossProduct(Spin * 0.01745, Velocity) * MagnusCoeff);
	}
	// Air drag (second branch).
	if (bAirDrag) {
		const double Speed = Velocity.Size();
		if (Speed >= AirDragMinSpeed) {
			Body->AddForce(Velocity.GetSafeNormal(0.0001f) * -1.0 * (Speed * Speed * AirDragCoeff));
		}
	}
}

void UBallMovementComponent::UpdateGrounded(UPrimitiveComponent* Body) {
	const USphereComponent* Sphere = Cast<USphereComponent>(Body);
	const double Radius = Sphere ? Sphere->GetScaledSphereRadius() : Body->Bounds.SphereRadius;
	const FVector Center = Body->GetComponentLocation();
	// Only what the ball itself collides with counts as ground: trace on the ball's own channel with its own
	// responses, so overlap-only shapes (the player's BallControlZone, goal / out-of-bounds triggers) are ignored.
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(BallGroundCheck), false, GetOwner());
	const FCollisionResponseParams Responses(Body->GetCollisionResponseToChannels());
	FHitResult Hit;
	const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Center, Center - FVector(0.0, 0.0, Radius + AirborneHeight + 1.0),
		Body->GetCollisionObjectType(), Params, Responses);
	const double Height = bHit ? Hit.Distance - Radius : TNumericLimits<double>::Max();
	if (bHit) {
		GroundZ = Hit.ImpactPoint.Z;
	}
	if (Height <= GroundedHeight) {
		bGrounded = true;
	} else if (Height > AirborneHeight) {
		bGrounded = false;
	}
}

void UBallMovementComponent::UpdateAutoState() {
	if (!BallState) {
		return;
	}
	switch (BallState->GetState()) {
	case EBallState::Free:
		if (!bGrounded) {
			BallState->SetState(EBallState::Airborne, nullptr);
		}
		break;
	case EBallState::Airborne:
		if (bGrounded) {
			BallState->SetState(EBallState::Free, nullptr);
		}
		break;
	case EBallState::Passing:
	case EBallState::Shooting:
		if (BallState->GetTimeInState() >= LaunchStateDuration) {
			BallState->SetState(bGrounded ? EBallState::Free : EBallState::Airborne, nullptr);
		}
		break;
	case EBallState::Deflected:
		if (BallState->GetTimeInState() >= DeflectedStateDuration) {
			BallState->SetState(bGrounded ? EBallState::Free : EBallState::Airborne, nullptr);
		}
		break;
	default:
		break; // Controlled, SkillControlled and Receiving are ended by whoever started them
	}
}

void UBallMovementComponent::ApplyDribble(AActor* Controller, FVector GuidePoint, FVector ControllerVelocity, float Strength, float MaxSpeed, float Blend) {
	UPrimitiveComponent* Body = GetBody();
	if (!Body || !BallState) {
		return;
	}
	if ((BallState->GetState() != EBallState::Controlled || BallState->GetBallOwner() != Controller)
		&& !BallState->SetState(EBallState::Controlled, Controller)) {
		return;
	}
	// Exactly the old BP_FootballPlayer.UpdateDribble.
	const FVector BallVelocity = Body->GetPhysicsLinearVelocity();
	const FVector ToGuide = GuidePoint - GetOwner()->GetActorLocation();
	const FVector Spring = ToGuide.GetSafeNormal(0.0001f) * (ToGuide.Size() * Strength);
	const FVector Target = (Spring + ControllerVelocity).GetClampedToSize(0.0, MaxSpeed);
	const FVector Blended = BallVelocity + Blend * (Target - BallVelocity);
	Body->SetPhysicsLinearVelocity(FVector(Blended.X, Blended.Y, BallVelocity.Z));
}

void UBallMovementComponent::EndDribble(AActor* Controller) {
	if (BallState && BallState->GetState() == EBallState::Controlled && BallState->GetBallOwner() == Controller) {
		BallState->SetState(bGrounded ? EBallState::Free : EBallState::Airborne, nullptr);
	}
}

void UBallMovementComponent::Launch(FVector VelocityChange, FVector AngularImpulseDegrees, EBallState LaunchState, AActor* Kicker) {
	UPrimitiveComponent* Body = GetBody();
	if (!Body) {
		return;
	}
	// Same three calls, in the same order, as the old ApplyKick.
	Body->SetSimulatePhysics(true);
	Body->AddImpulse(VelocityChange, NAME_None, true);
	Body->AddAngularImpulseInDegrees(AngularImpulseDegrees, NAME_None, true);
	if (BallState) {
		BallState->SetState(LaunchState, Kicker);
	}
}

void UBallMovementComponent::ApplyTouch(FVector Velocity, FVector AngularVelocityDegrees, EBallState NewState, AActor* Toucher) {
	UPrimitiveComponent* Body = GetBody();
	if (!Body) {
		return;
	}
	Body->SetPhysicsLinearVelocity(Velocity);
	Body->SetPhysicsAngularVelocityInDegrees(AngularVelocityDegrees);
	if (BallState) {
		BallState->SetState(NewState, Toucher);
	}
}

void UBallMovementComponent::ApplyDeflection(FVector Velocity, FVector AngularVelocityDegrees, AActor* Deflector) {
	ApplyTouch(Velocity, AngularVelocityDegrees, EBallState::Deflected, Deflector);
}

void UBallMovementComponent::StartRecording(float Duration) {
	Recording.Reset();
	RecordStart = GetWorld()->GetTimeSeconds();
	RecordUntil = RecordStart + Duration;
	if (UPrimitiveComponent* Body = GetBody()) {
		RecordSample(Body);
	}
}

void UBallMovementComponent::RecordSample(UPrimitiveComponent* Body) {
	FBallRecordSample& Sample = Recording.AddDefaulted_GetRef();
	Sample.Time = float(GetWorld()->GetTimeSeconds() - RecordStart);
	Sample.Position = Body->GetComponentLocation();
	Sample.Velocity = GetBallVelocity();
	Sample.AngularVelocityDeg = Body->GetPhysicsAngularVelocityInDegrees();
	Sample.State = BallState ? BallState->GetState() : EBallState::Free;
	Sample.bGrounded = bGrounded;
}
