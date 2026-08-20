#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GoalTrigger.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGoalScored, AActor*, ScoringBall);

UCLASS()
class FOOTBALLARENA_API AGoalTrigger : public AActor {
	GENERATED_BODY()
public:
	AGoalTrigger();

	UPROPERTY(BlueprintAssignable, Category = "Football")
	FOnGoalScored OnGoalScored;
protected:
	UPROPERTY(VisibleAnywhere, Category = "Football")
	class UBoxComponent* TriggerBox;

	UFUNCTION()
	void OnOverlapBegin(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
};