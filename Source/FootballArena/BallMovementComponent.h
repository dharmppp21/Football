#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FootballTypes.h"
#include "BallMovementComponent.generated.h"

class UBallInteractionComponent;
class UBallStateComponent;
class UPrimitiveComponent;

// Moves the ball according to its state (doc §19):
// - while the ball simulates physics, once per physics step (AsyncPhysicsTickComponent, master plan U1-U3, U5): air drag
//   with the drag crisis, Magnus lift while airborne, and while it touches the ground grass rolling resistance and the
//   torsional friction that stops it spinning on the spot. (Until U2 Magnus and a constant drag came once per frame from
//   Tick, so curl changed with the frame rate. The U4 knuckle force was removed on 2026-10-08 at the user's request.)
// - Controlled: the dribble velocity servo, driven every tick by the controlling player (was UpdateDribble);
// - Launch: kicks add velocity and spin to the current motion (the impulses of the old ApplyKick).
// - SkillControlled: owned by a skill; a physics ball between the skill's touches (ApplyTouch). The ball always stays
//   simulated (master spec §6): the kinematic hold was removed in master plan Phase 1 (2026-10-06).
// It also makes the automatic state changes: Free <-> Airborne by ground contact, and Passing / Shooting
// -> Free / Airborne once the ball has left the foot.
UCLASS(ClassGroup = (Football), meta = (BlueprintSpawnableComponent))
class FOOTBALLARENA_API UBallMovementComponent : public UActorComponent {
	GENERATED_BODY()
public:
	UBallMovementComponent();

	// Off = plain engine physics: no air forces and no rolling resistance (used for A/B tests).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Physics")
	bool bApplyPhysicsExtras = true;

	// Air forces are AeroForceScale * C * speed^2 (kg cm/s^2 for cm/s): 2.28e-4 = 1/2 x air 1.2 kg/m^3 x the ball's
	// cross-section (r 11 cm). C is the drag, lift or side coefficient below (sources: memory fa-ball-dribble 2026-10-07).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Aero")
	float AeroForceScale = 2.28e-4f;

	// Drag coefficient with the drag crisis (a fit to Brazuca wind-tunnel data): Cd = DragCdHigh + DragCdCrisisStep /
	// (1 + exp((speed - DragCrisisSpeed) / DragCrisisWidth)) + DragSpinFactor * Sp, where Sp = radius x spin / speed (spin
	// across the travel). About 0.45 below ~12 m/s and 0.19 above ~15 m/s. The spin term only in the air and up to Sp 0.4
	// (the data's range): a ball rolling on the ground has Sp 1, where it would nearly triple the drag.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Aero")
	float DragCdHigh = 0.187f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Aero")
	float DragCdCrisisStep = 0.261f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Aero")
	float DragCrisisSpeed = 1291.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Aero")
	float DragCrisisWidth = 71.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Aero")
	float DragSpinFactor = 0.5f;

	// Magnus lift while airborne (master spec §92): CL = LiftCoeffMax * (1 - exp(-Sp / LiftSpinScale)), along spin x
	// velocity. It levels off near 0.3 like measured balls; the old linear Magnus (until U2) was 1.4x too strong at Sp 0.2.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Aero")
	float LiftCoeffMax = 0.32f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Aero")
	float LiftSpinScale = 0.13f;

	// Grass rolling resistance (U3): while the ball touches the ground, a force of mass x RollingResistance (cm/s^2)
	// against its motion along the ground, fading out below 5 cm/s. A rolling hollow ball turns 0.6 of it into slowing.
	// FIFA ball-roll test: a 3.4 m/s rolling start (1 m drop, 45 deg ramp) stops after 6.6 m - FIFA Quality Pro allows
	// 4-8 m (master plan U.3; 6.0 m when calibrated, before the U.4 spin-drag fix).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Ground")
	float RollingResistance = 135.f;

	// Torsional friction (master plan U5): while the ball touches the ground its spin about the vertical axis slows by
	// GroundSpinFriction (deg/s^2), the grass gripping the contact patch. Chaos' point contact has no torque about the
	// normal, so without it a curled ball kept spinning on the spot forever. 2600 stops 10 rev/s in about 1.4 s.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Ground")
	float GroundSpinFriction = 2600.f;

	// The ball touches the ground while its bottom is within this (cm) of the ground: rolling resistance and torsional
	// friction on, Magnus off.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Ground")
	float GroundContactHeight = 1.5f;

	// The ball's bottom higher than AirborneHeight (cm) above the ground = Airborne; back under GroundedHeight = grounded.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|State")
	float AirborneHeight = 6.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|State")
	float GroundedHeight = 2.f;

	// How long a launched ball stays Passing / Shooting before it becomes Free / Airborne.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|State")
	float LaunchStateDuration = 0.15f;

	// How long a deflected ball stays Deflected before it becomes Free / Airborne.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|State")
	float DeflectedStateDuration = 0.1f;

	// Spec §4: the ball is never left in SkillControlled or Receiving. After this long (s) it is let go (Free / Airborne);
	// whoever took it should have ended it long before.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|State")
	float MaxSkillControlledTime = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|State")
	float MaxReceivingTime = 3.f;

	// The old UpdateDribble servo: velocity = lerp(ball velocity, clamp(controller velocity + (guide - ball) * Strength,
	// MaxSpeed), Blend), keeping the ball's own vertical velocity. Makes the ball Controlled by Controller.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void ApplyDribble(AActor* Controller, FVector GuidePoint, FVector ControllerVelocity, float Strength, float MaxSpeed, float Blend);

	// Controller stopped dribbling: Controlled -> Free / Airborne. Does nothing if a kick or someone else took the ball.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void EndDribble(AActor* Controller);

	// Kick: VelocityChange is added to the ball's motion. Of AngularImpulseDegrees (deg/s), a topspin / backspin part (about
	// a horizontal axis) replaces the ball's - the foot grips the ball through the strike, so a shot off a rolling dribble
	// leaves with the strike's spin - and the sidespin part (about the vertical axis) adds to it. A kick with no topspin /
	// backspin (the finesse) keeps the ball's roll, as it always has (master plan U5).
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void Launch(FVector VelocityChange, FVector AngularImpulseDegrees, EBallState LaunchState, AActor* Kicker);

	// A touch (doc §19 "calculated response + physics"): the given velocity and spin, and NewState (a skill's
	// SkillControlled, Deflected, Free...). Physics carries the ball on from there.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void ApplyTouch(FVector Velocity, FVector AngularVelocityDegrees, EBallState NewState, AActor* Toucher);

	// A calculated deflection (doc §19): ApplyTouch with the Deflected state.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void ApplyDeflection(FVector Velocity, FVector AngularVelocityDegrees, AActor* Deflector);

	// The ball's velocity, including a velocity set by the response queued for this frame (it shows at once, as the old
	// immediate writes did; UBallInteractionComponent::GetPendingMotion).
	UFUNCTION(BlueprintPure, Category = "Ball")
	FVector GetBallVelocity() const;

	UFUNCTION(BlueprintPure, Category = "Ball")
	float GetBallRadius() const;

	// Height of the ground under the ball (from the last ground check that found it).
	UFUNCTION(BlueprintPure, Category = "Ball")
	float GetGroundZ() const { return float(GroundZ); }

	UFUNCTION(BlueprintPure, Category = "Ball")
	bool IsGrounded() const { return bGrounded; }

	// The air force (kg cm/s^2) at this velocity (cm/s) and spin (deg/s): drag, plus Magnus lift when airborne. The
	// per-step forces use exactly this; the tests and the kick calibration call it too.
	UFUNCTION(BlueprintPure, Category = "Ball|Aero")
	FVector ComputeAeroForce(FVector Velocity, FVector AngularVelocityDeg, bool bAirborne) const;

	// Test recorder: one sample per tick for Duration seconds.
	UFUNCTION(BlueprintCallable, Category = "Ball|Debug")
	void StartRecording(float Duration);

	UFUNCTION(BlueprintCallable, Category = "Ball|Debug")
	TArray<FBallRecordSample> GetRecording() const { return Recording; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// Once per physics step (substep), before it integrates: the air forces and rolling resistance from the ball's state at
	// that step, so they follow the ball within a frame and do not change with the frame rate.
	virtual void AsyncPhysicsTickComponent(float DeltaTime, float SimTime) override;

protected:
	virtual void BeginPlay() override;

private:
	UPrimitiveComponent* GetBody() const;
	FVector AeroForce(const FVector& Velocity, const FVector& SpinRad, double Radius, bool bAirborne) const;
	void UpdateGrounded(UPrimitiveComponent* Body);
	void UpdateAutoState();
	void RecordSample(UPrimitiveComponent* Body);

	UPROPERTY()
	TObjectPtr<UBallStateComponent> BallState;

	// Where this component's writes go as response commands (spec §146; master plan 3.5).
	UPROPERTY()
	TObjectPtr<UBallInteractionComponent> BallInteraction;

	double GroundZ = 0.0;
	double CachedRadius = 11.0;
	bool bGrounded = true;
	double RecordStart = 0.0;
	double RecordUntil = 0.0;
	TArray<FBallRecordSample> Recording;
};
