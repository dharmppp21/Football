#include "BallMovementComponent.h"
#include "BallInteractionComponent.h"
#include "BallStateComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"

UBallMovementComponent::UBallMovementComponent() {
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics; // the ground check and state changes are ready before this frame's physics
}

void UBallMovementComponent::BeginPlay() {
	Super::BeginPlay();
	BallState = GetOwner()->FindComponentByClass<UBallStateComponent>();
	BallInteraction = GetOwner()->FindComponentByClass<UBallInteractionComponent>();
	CachedRadius = GetBallRadius();
	// The per-step callback only runs for an active component (the placed ball's is not auto-activated).
	Activate();
	SetAsyncPhysicsTickEnabled(true);
}

UPrimitiveComponent* UBallMovementComponent::GetBody() const {
	const AActor* Owner = GetOwner(); // none on the default object, which the self-test's force checks use
	return Owner ? Cast<UPrimitiveComponent>(Owner->GetRootComponent()) : nullptr;
}

float UBallMovementComponent::GetBallRadius() const {
	const USphereComponent* Sphere = Cast<USphereComponent>(GetBody());
	return Sphere ? Sphere->GetScaledSphereRadius() : 11.f;
}

// The ball's motion as gameplay sees it: with what the response queued for this frame sets (master plan 3.5).
static void ReadMotion(const UBallInteractionComponent* Interaction, const UPrimitiveComponent* Body, FVector& Velocity, FVector& SpinDeg) {
	if (Interaction) {
		Interaction->GetPendingMotion(Velocity, SpinDeg);
		return;
	}
	Velocity = Body ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector;
	SpinDeg = Body ? Body->GetPhysicsAngularVelocityInDegrees() : FVector::ZeroVector;
}

FVector UBallMovementComponent::GetBallVelocity() const {
	FVector Velocity, Spin;
	ReadMotion(BallInteraction, GetBody(), Velocity, Spin);
	return Velocity;
}

void UBallMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UPrimitiveComponent* Body = GetBody();
	if (!Body) {
		return;
	}
	UpdateGrounded(Body);
	UpdateAutoState();
	if (GetWorld()->GetTimeSeconds() < RecordUntil) {
		RecordSample(Body);
	}
}

FVector UBallMovementComponent::AeroForce(const FVector& Velocity, const FVector& SpinRad, double Radius, bool bAirborne) const {
	const double Speed = Velocity.Size();
	if (Speed < 1.0) {
		return FVector::ZeroVector;
	}
	const FVector Dir = Velocity / Speed;
	const FVector SpinAcross = SpinRad - Dir * FVector::DotProduct(SpinRad, Dir); // spin about the travel axis lifts nothing
	const double Sp = Radius * SpinAcross.Size() / Speed;
	// Spin adds drag in the air only, and only as far as the wind-tunnel data go (Sp ~0.4): a ball rolling on the ground
	// has Sp 1, and the airborne fit there would nearly triple its drag (measured in master plan U.4: passes braked 10 m/s^2).
	const double SpinDrag = bAirborne ? DragSpinFactor * FMath::Min(Sp, 0.4) : 0.0;
	const double Cd = DragCdHigh + DragCdCrisisStep / (1.0 + FMath::Exp((Speed - DragCrisisSpeed) / FMath::Max(DragCrisisWidth, 1.f)))
		+ SpinDrag;
	const double Pressure = AeroForceScale * Speed * Speed;
	FVector Force = Dir * (-Cd * Pressure);
	if (bAirborne && Sp > 0.0) {
		const double Cl = LiftCoeffMax * (1.0 - FMath::Exp(-Sp / FMath::Max(LiftSpinScale, 0.001f)));
		Force += FVector::CrossProduct(SpinAcross, Dir).GetSafeNormal() * (Cl * Pressure);
	}
	return Force;
}

FVector UBallMovementComponent::ComputeAeroForce(FVector Velocity, FVector AngularVelocityDeg, bool bAirborne) const {
	return AeroForce(Velocity, FMath::DegreesToRadians(AngularVelocityDeg), GetBallRadius(), bAirborne);
}

void UBallMovementComponent::AsyncPhysicsTickComponent(float DeltaTime, float SimTime) {
	// No Super: it only forwards to the Blueprint event ReceiveAsyncPhysicsTick, which nothing implements and which must not
	// run off the game thread.
	const UPrimitiveComponent* Body = GetBody();
	FBodyInstance* Instance = (bApplyPhysicsExtras && Body) ? Body->GetBodyInstance() : nullptr;
	if (!Instance) {
		return;
	}
	FBodyInstanceAsyncPhysicsTickHandle Handle = Instance->GetBodyInstanceAsyncPhysicsTickHandle();
	if (!Handle) {
		return;
	}
	// This step's motion, with an impulse queued for it (a kick is added in this step's integration, after this call).
	const FVector Velocity = Handle->V() + Handle->LinearImpulseVelocity();
	const FVector Spin = Handle->W() + Handle->AngularImpulseVelocity();
	const bool bAirborne = Handle->X().Z - CachedRadius - GroundZ > GroundContactHeight;
	FVector Force = AeroForce(Velocity, Spin, CachedRadius, bAirborne);
	if (!bAirborne) {
		const FVector Flat(Velocity.X, Velocity.Y, 0.0);
		const double FlatSpeed = Flat.Size();
		if (FlatSpeed > UE_KINDA_SMALL_NUMBER) {
			Force -= Flat / FlatSpeed * (Handle->M() * RollingResistance * FMath::Min(FlatSpeed / 5.0, 1.0));
		}
		// Torsional friction: the grass slows the spin about the ground's normal, never past zero within the step.
		if (FMath::Abs(Spin.Z) > UE_KINDA_SMALL_NUMBER) {
			const double Slow = FMath::Min(double(FMath::DegreesToRadians(GroundSpinFriction)), FMath::Abs(Spin.Z) / FMath::Max(double(DeltaTime), 1e-4));
			Handle->AddTorque(FVector(0.0, 0.0, -FMath::Sign(Spin.Z) * Slow * Handle->I().Z));
		}
	}
	Handle->AddForce(Force);
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
	case EBallState::SkillControlled:
	case EBallState::Receiving:
		if (BallState->GetTimeInState() >= (BallState->GetState() == EBallState::SkillControlled ? MaxSkillControlledTime : MaxReceivingTime)) {
			UE_LOG(LogTemp, Warning, TEXT("Ball held %s for %.1f s by %s: let go"), *UEnum::GetValueAsString(BallState->GetState()),
				BallState->GetTimeInState(), *GetNameSafe(BallState->GetBallOwner()));
			BallState->SetState(bGrounded ? EBallState::Free : EBallState::Airborne, nullptr);
		}
		break;
	default:
		break; // Controlled, SkillControlled and Receiving are ended by whoever started them
	}
}

void UBallMovementComponent::ApplyDribble(AActor* Controller, FVector GuidePoint, FVector ControllerVelocity, float Strength, float MaxSpeed, float Blend) {
	UPrimitiveComponent* Body = GetBody();
	if (!Body || !BallState || !BallInteraction) {
		return;
	}
	if ((BallState->GetState() != EBallState::Controlled || BallState->GetBallOwner() != Controller)
		&& !BallState->SetState(EBallState::Controlled, Controller)) {
		return;
	}
	// Exactly the old BP_FootballPlayer.UpdateDribble; its velocity is set at the arbitration point (spec §146). The blend
	// is the bound for now: a MaxAllowedCorrection measured here is wrong when the command lands a physics step later (the
	// skill's approach, see ApplyTouch). The enforced bound comes with the frame-rate independent dribble (master plan 7.3).
	const FVector BallVelocity = GetBallVelocity();
	const FVector ToGuide = GuidePoint - GetOwner()->GetActorLocation();
	const FVector Spring = ToGuide.GetSafeNormal(0.0001f) * (ToGuide.Size() * Strength);
	const FVector Target = (Spring + ControllerVelocity).GetClampedToSize(0.0, MaxSpeed);
	const FVector Blended = BallVelocity + Blend * (Target - BallVelocity);
	FBallResponseCommand C;
	C.Priority = BallInteraction->GetHeldPriority(); // what the caller claimed the ball at: Dribble, or Skill for a skill's approach
	C.Source = C.Priority == EContactPriority::Skill ? TEXT("SkillApproach") : TEXT("Dribble");
	C.Instigator = Controller;
	C.ResponseType = EBallResponseMode::ControlledContact;
	C.bUseDesiredVelocity = true;
	C.DesiredVelocity = FVector(Blended.X, Blended.Y, BallVelocity.Z);
	BallInteraction->SubmitResponse(C);
}

void UBallMovementComponent::EndDribble(AActor* Controller) {
	if (BallState && BallState->GetState() == EBallState::Controlled && BallState->GetBallOwner() == Controller) {
		BallState->SetState(bGrounded ? EBallState::Free : EBallState::Airborne, nullptr);
	}
}

void UBallMovementComponent::Launch(FVector VelocityChange, FVector AngularImpulseDegrees, EBallState LaunchState, AActor* Kicker) {
	UPrimitiveComponent* Body = GetBody();
	if (!Body || !BallInteraction) {
		return;
	}
	// The old ApplyKick's two velocity-change impulses, as a strike command (spec §145: only a capture outranks it). The
	// state changes now; the impulses land at the arbitration point, before this frame's physics step.
	Body->SetSimulatePhysics(true);
	FBallResponseCommand C;
	C.Source = TEXT("Kick");
	C.Instigator = Kicker;
	C.ResponseType = EBallResponseMode::PhysicalStrike;
	C.Priority = EContactPriority::Skill;
	C.LinearImpulse = VelocityChange;
	C.bVelocityChange = true;
	if (!FVector(AngularImpulseDegrees.X, AngularImpulseDegrees.Y, 0.0).IsNearlyZero()) {
		// The strike's topspin / backspin replaces the ball's (a dribbled ball rolls with up to ~9 rev/s of topspin, which a
		// shot used to keep and dip with); its sidespin adds to the ball's.
		FVector VelocityNow, SpinNow;
		BallInteraction->GetPendingMotion(VelocityNow, SpinNow);
		C.bSetAngularVelocity = true;
		C.AngularVelocityDeg = FVector(AngularImpulseDegrees.X, AngularImpulseDegrees.Y, SpinNow.Z + AngularImpulseDegrees.Z);
	} else {
		C.AngularImpulse = FMath::DegreesToRadians(AngularImpulseDegrees); // sidespin only (the finesse): the ball keeps its roll
	}
	BallInteraction->SubmitResponse(C);
	if (BallState) {
		BallState->SetState(LaunchState, Kicker);
	}
}

void UBallMovementComponent::ApplyTouch(FVector Velocity, FVector AngularVelocityDegrees, EBallState NewState, AActor* Toucher) {
	UPrimitiveComponent* Body = GetBody();
	if (!Body || !BallInteraction) {
		return;
	}
	// The velocity and spin are set at the arbitration point, as the old touch set them. Not an impulse: a skill command
	// can land a physics step late (in master plan 3.5 the commands of two consecutive skill ticks sometimes reached the same
	// arbitration point), and an impulse taken from the velocity seen here would then overshoot. A set velocity also shows
	// at once to the skill's next read (GetBallVelocity). The touch becomes an impulse with the skill's rework (Phase 6).
	// The state changes now.
	const bool bDeflection = NewState == EBallState::Deflected;
	FBallResponseCommand C;
	C.Source = bDeflection ? TEXT("Deflection") : TEXT("Touch");
	C.Instigator = Toucher;
	C.ResponseType = bDeflection ? EBallResponseMode::Deflection : EBallResponseMode::PhysicalTouch;
	C.Priority = BallInteraction->GetHeldPriority(); // what the caller claimed the ball at: Skill, Foot or Body
	C.bUseDesiredVelocity = true;
	C.DesiredVelocity = Velocity;
	C.bSetAngularVelocity = true;
	C.AngularVelocityDeg = AngularVelocityDegrees;
	BallInteraction->SubmitResponse(C);
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
	ReadMotion(BallInteraction, Body, Sample.Velocity, Sample.AngularVelocityDeg);
	Sample.State = BallState ? BallState->GetState() : EBallState::Free;
	Sample.bGrounded = bGrounded;
}
