#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HAL/IConsoleManager.h"
#include "FootballTypes.h"
#include "BallInteractionComponent.generated.h"

class FPhysScene_Chaos;
class UBallMovementComponent;
class UBallStateComponent;

// fb.Debug.Contact: draw every body contact the ball handles. fb.Debug.Regions: draw body and ball regions.
// fb.Debug.Physics: draw the arbitrated response commands. fb.Debug.Ball: draw the ball's velocity, spin and state.
extern TAutoConsoleVariable<int32> CVarFootballDebugContacts;
extern TAutoConsoleVariable<int32> CVarFootballDebugRegions;

// Ball side of ball interaction (doc §31, §35):
// - Contact priority: every system that moves the ball claims it first with TryClaim. In a frame, a higher
//   priority beats a lower one, and the current controller keeps its priority across frames (a dribbled ball
//   can't be knocked away by a lower-priority bump).
// - Body collisions: the ball overlapping a character's skeletal mesh is turned into a calculated deflection
//   (doc §19) from the touched body region. Event-driven: nothing is checked per frame. The owner's own body
//   is ignored while it holds the ball and for OwnerGraceTime after letting go.
UCLASS(ClassGroup = (Football), meta = (BlueprintSpawnableComponent))
class FOOTBALLARENA_API UBallInteractionComponent : public UActorComponent {
	GENERATED_BODY()
public:
	UBallInteractionComponent();

	// Body touches closing slower than this (cm/s, relative to the body) are resting contacts and are ignored.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Contact")
	float MinDeflectSpeed = 50.f;

	// How long after its owner let go of the ball (or kicked it) the owner's own body can't deflect it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Contact")
	float OwnerGraceTime = 0.5f;

	// Claims the ball for this frame (doc §31). True = go ahead and move it. False = a higher-priority contact,
	// or the ball's current controller at the same priority, already has it.
	UFUNCTION(BlueprintCallable, Category = "Ball|Contact")
	bool TryClaim(EContactPriority Priority, AActor* Instigator);

	// The priority the ball is held at right now (this frame's claim or the controlling state).
	UFUNCTION(BlueprintPure, Category = "Ball|Contact")
	EContactPriority GetHeldPriority() const;

	UFUNCTION(BlueprintPure, Category = "Ball|Contact")
	FBallContactInfo GetLastContact() const { return LastContact; }

	// Every body contact of the last moments, oldest first (LastContact keeps only the last one of a frame).
	UFUNCTION(BlueprintPure, Category = "Ball|Contact")
	TArray<FBallContactInfo> GetContactHistory() const { return ContactHistory; }

	// Spec §146 / §147: queues a response for this frame. Once per frame, just before physics runs, the highest-priority
	// command is applied and the others are dropped and logged, so two systems never write the ball in the same step.
	// Commands submitted after physics (overlap / hit events) wait for the next frame. False = a higher-priority command
	// is already queued (this one will be dropped).
	UFUNCTION(BlueprintCallable, Category = "Ball|Response")
	bool SubmitResponse(const FBallResponseCommand& Command);

	// The arbitration rule, testable without PIE: the index of the highest Priority, -1 if empty. On a tie the later one: it
	// was worked out from the earlier one's result (see GetPendingMotion).
	UFUNCTION(BlueprintPure, Category = "Ball|Response")
	static int32 PickWinningCommand(const TArray<FBallResponseCommand>& Commands);

	// The last arbitrated commands, oldest first.
	UFUNCTION(BlueprintPure, Category = "Ball|Response")
	TArray<FBallCommandRecord> GetCommandLog() const { return CommandLog; }

	// The ball's velocity and spin (deg/s) as gameplay sees them: a velocity or spin set by the queued winning command shows
	// at once, as the old immediate writes did (an impulse only after the physics step, as before). Whatever reads the ball's
	// motion goes through this (UBallMovementComponent::GetBallVelocity), so a writer whose command lands after its next
	// tick, or a second contact in the same frame, sees the first write.
	void GetPendingMotion(FVector& Velocity, FVector& AngularVelocityDeg) const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void OnPhysScenePreTick(FPhysScene_Chaos* Scene, float DeltaTime);
	void ApplyCommand(const FBallResponseCommand& Command);
	void LogCommand(const FBallResponseCommand& Command, bool bApplied, const FString& Note, const FVector& VelocityBefore);
	EContactPriority ControllingStatePriority() const;
	void RecordContact(AActor* Instigator, EBodyRegion Region, EContactPriority Priority, bool bApplied, const TCHAR* Note,
		const FVector& BodyPoint, const FVector& Normal, float ContactRadius, const FVector& VelocityIn, const FVector& VelocityOut);

	UFUNCTION()
	void OnBallBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void OnBallStateChanged(EBallState OldState, EBallState NewState, AActor* Instigator);

	UPROPERTY()
	TObjectPtr<UBallStateComponent> BallState;

	UPROPERTY()
	TObjectPtr<UBallMovementComponent> BallMovement;

	uint64 ClaimFrame = 0;
	EContactPriority ClaimPriority = EContactPriority::None;
	TWeakObjectPtr<AActor> ClaimInstigator;
	double OwnerReleaseTime = -1.0e9;
	FBallContactInfo LastContact;
	TArray<FBallContactInfo> ContactHistory;
	UPROPERTY(Transient)
	TArray<FBallResponseCommand> PendingCommands;
	TArray<FBallCommandRecord> CommandLog;
	FDelegateHandle PreTickHandle;
};
