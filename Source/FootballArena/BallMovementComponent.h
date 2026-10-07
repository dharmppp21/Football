#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FootballTypes.h"
#include "BallMovementComponent.generated.h"

class UBallStateComponent;
class UPrimitiveComponent;

// Moves the ball according to its state (doc §19). Ported 1:1 from the Blueprints:
// - while the ball simulates physics: Magnus and air drag (were BP_Football's Tick). The ground-hit spin pull (was
//   BP_Football's OnComponentHit) was removed in master plan 3.1 (2026-10-07): it spun the ball about its direction of
//   travel, so straight passes curved 25-60 cm sideways, by an amount that changed with the frame rate;
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

	// Off = plain engine physics, with no Magnus or air drag (used for A/B tests).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Physics")
	bool bApplyPhysicsExtras = true;

	// Magnus force = MagnusCoeff * (spin x velocity), while spinning faster than MagnusMinSpin (deg/s) and airborne
	// (master spec §92): not grounded (see AirborneHeight), or rising faster than MagnusMinRiseSpeed (cm/s) - a kicked
	// ball on its first frames, before the ground check has seen it leave; a rolling ball's contact jitter stays far
	// below that. Until 3.1 the test was any vertical speed above 15 cm/s, which that jitter passes too: Magnus then
	// pushed a rolling ball into the ground (-22% pass speed at 30 fps).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Physics")
	float MagnusCoeff = 0.0045f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Physics")
	float MagnusMinSpin = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Physics")
	float MagnusMinRiseSpeed = 100.f;

	// Extra drag force = AirDragCoeff * speed^2 against the motion, above AirDragMinSpeed (cm/s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Physics")
	bool bAirDrag = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Physics")
	float AirDragCoeff = 2e-5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Physics")
	float AirDragMinSpeed = 40.f;

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

	// The old UpdateDribble servo: velocity = lerp(ball velocity, clamp(controller velocity + (guide - ball) * Strength,
	// MaxSpeed), Blend), keeping the ball's own vertical velocity. Makes the ball Controlled by Controller.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void ApplyDribble(AActor* Controller, FVector GuidePoint, FVector ControllerVelocity, float Strength, float MaxSpeed, float Blend);

	// Controller stopped dribbling: Controlled -> Free / Airborne. Does nothing if a kick or someone else took the ball.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void EndDribble(AActor* Controller);

	// Kick: VelocityChange and AngularImpulseDegrees are added to the current motion (velocity-change impulses).
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void Launch(FVector VelocityChange, FVector AngularImpulseDegrees, EBallState LaunchState, AActor* Kicker);

	// A touch (doc §19 "calculated response + physics"): the given velocity and spin, and NewState (a skill's
	// SkillControlled, Deflected, Free...). Physics carries the ball on from there.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void ApplyTouch(FVector Velocity, FVector AngularVelocityDegrees, EBallState NewState, AActor* Toucher);

	// A calculated deflection (doc §19): ApplyTouch with the Deflected state.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void ApplyDeflection(FVector Velocity, FVector AngularVelocityDegrees, AActor* Deflector);

	// The ball's physics velocity.
	UFUNCTION(BlueprintPure, Category = "Ball")
	FVector GetBallVelocity() const;

	UFUNCTION(BlueprintPure, Category = "Ball")
	float GetBallRadius() const;

	// Height of the ground under the ball (from the last ground check that found it).
	UFUNCTION(BlueprintPure, Category = "Ball")
	float GetGroundZ() const { return float(GroundZ); }

	UFUNCTION(BlueprintPure, Category = "Ball")
	bool IsGrounded() const { return bGrounded; }

	// Test recorder: one sample per tick for Duration seconds.
	UFUNCTION(BlueprintCallable, Category = "Ball|Debug")
	void StartRecording(float Duration);

	UFUNCTION(BlueprintCallable, Category = "Ball|Debug")
	TArray<FBallRecordSample> GetRecording() const { return Recording; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
	UPrimitiveComponent* GetBody() const;
	void ApplyPhysicsExtras(UPrimitiveComponent* Body) const;
	void UpdateGrounded(UPrimitiveComponent* Body);
	void UpdateAutoState();
	void RecordSample(UPrimitiveComponent* Body);

	UPROPERTY()
	TObjectPtr<UBallStateComponent> BallState;

	double GroundZ = 0.0;
	bool bGrounded = true;
	double RecordStart = 0.0;
	double RecordUntil = 0.0;
	TArray<FBallRecordSample> Recording;
};
