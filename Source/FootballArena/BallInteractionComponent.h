#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HAL/IConsoleManager.h"
#include "FootballTypes.h"
#include "BallInteractionComponent.generated.h"

class UBallMovementComponent;
class UBallStateComponent;

// fb.Debug.Contact: draw every body contact the ball handles. fb.Debug.Regions: draw body and ball regions.
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

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
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
};
