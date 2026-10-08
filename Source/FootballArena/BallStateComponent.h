#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FootballTypes.h"
#include "BallStateComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnBallStateChanged, EBallState, OldState, EBallState, NewState, AActor*, Instigator);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnBallOwnerChanged, AActor*, NewOwner, EBallOwnerType, OwnerType);

// The ball's state machine (doc §12) and its owner (doc §30). They are kept apart on purpose: a player
// still owns a ball that is Free (knocked ahead) or Shooting. The owner only changes when another character
// takes the ball (Controlled, SkillControlled, Receiving, Passing, Shooting, GoalkeeperControlled) or on ResetState.
// Spec §4 entry / update / exit: entry = SetState (OnBallStateChanged), update = UBallMovementComponent::UpdateAutoState
// (ground contact, launch / deflection timers, the SkillControlled / Receiving time limits), exit = the OldState passed
// to OnBallStateChanged.
UCLASS(ClassGroup = (Football), meta = (BlueprintSpawnableComponent))
class FOOTBALLARENA_API UBallStateComponent : public UActorComponent {
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable, Category = "Ball")
	FOnBallStateChanged OnBallStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Ball")
	FOnBallOwnerChanged OnBallOwnerChanged;

	// Changes the state if IsTransitionAllowed says so. Returns false (and logs) if refused.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	bool SetState(EBallState NewState, AActor* Instigator);

	// Free with no owner. Call when the ball is respawned.
	UFUNCTION(BlueprintCallable, Category = "Ball")
	void ResetState();

	UFUNCTION(BlueprintPure, Category = "Ball")
	EBallState GetState() const { return State; }

	UFUNCTION(BlueprintPure, Category = "Ball")
	AActor* GetBallOwner() const { return BallOwner.Get(); }

	UFUNCTION(BlueprintPure, Category = "Ball")
	EBallOwnerType GetOwnerType() const { return OwnerType; }

	UFUNCTION(BlueprintPure, Category = "Ball")
	float GetTimeInState() const;

	UFUNCTION(BlueprintPure, Category = "Ball")
	static bool IsTransitionAllowed(EBallState From, EBallState To);

private:
	void SetBallOwner(AActor* NewOwner);

	EBallState State = EBallState::Free;
	TWeakObjectPtr<AActor> BallOwner;
	EBallOwnerType OwnerType = EBallOwnerType::None;
	double StateStartTime = 0.0;
};
