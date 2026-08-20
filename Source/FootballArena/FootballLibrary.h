#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FootballLibrary.generated.h"

UCLASS()
class FOOTBALLARENA_API UFootballLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	public:
		UFUNCTION(BlueprintPure,Category="Football")
		static float GetShotPowerForDistance(float DistanceMeters);
		UFUNCTION(BlueprintPure, Category = "Football")
		static FVector GetAimAssistedDirection(FVector AimDirection, FVector ToGoalCenter, float DistanceMeters);
};
