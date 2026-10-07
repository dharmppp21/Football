#pragma once

#include "CoreMinimal.h"
#include "Curves/CurveFloat.h"
#include "Engine/DataAsset.h"
#include "FootballTypes.h"
#include "BallTrajectoryData.generated.h"

// A skill's ball path (DA_BallTrajectory, doc §13–§14). The curves give the ball centre relative to the ground point
// where the skill started, in skill space, at normalized skill time T (0..1 over the montage). At runtime the player's
// baked travel is swapped for where the player really is, so a blocked player keeps the ball with them:
//   LocalOffset = (ForwardCurve(T) * ForwardAmount, LateralCurve(T) * LateralAmount, VerticalCurve(T) * VerticalAmount)
// SpeedCurve, if it has keys, remaps T first (timing changes without touching the path). Samples are the baked data
// (Tools/prep_skill_anims.py); RebuildCurvesFromSamples turns them into the curves, which can then be tuned by hand.
// Allowed uses (master spec §138): editor visualization, prediction, reachability and QA. Steering the ball by it at
// runtime is legacy (UFootballSkillComponent's Legacy* functions, deleted in master plan Phase 6).
UCLASS(BlueprintType)
class FOOTBALLARENA_API UBallTrajectoryData : public UDataAsset {
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trajectory")
	FRuntimeFloatCurve ForwardCurve;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trajectory")
	FRuntimeFloatCurve LateralCurve;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trajectory")
	FRuntimeFloatCurve VerticalCurve;

	// Optional time remap: T -> SpeedCurve(T). No keys = off.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trajectory")
	FRuntimeFloatCurve SpeedCurve;

	// Tuning scales (doc §34 ForwardDisplacement / LateralAmount / VerticalAmount).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tuning")
	float ForwardAmount = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tuning")
	float LateralAmount = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tuning")
	float VerticalAmount = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Baked")
	TArray<FBallTrajectorySample> Samples;

	// Ball centre at normalized skill time T, in skill space relative to the ground point where the skill started.
	UFUNCTION(BlueprintPure, Category = "Football|Trajectory")
	FVector Evaluate(float T) const;

	// Replaces the three path curves with smooth keys (cubic, smart-auto tangents) through Samples. SpeedCurve is kept.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Football|Trajectory")
	void RebuildCurvesFromSamples();
};
