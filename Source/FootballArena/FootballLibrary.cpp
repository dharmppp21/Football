#include "FootballLibrary.h"
float UFootballLibrary::GetShotPowerForDistance(float DistanceMeters) {
	const float BasePower = 1400.0f;
	const float PerMetre = 80.0f;
	const float MaxPower = 3600.0f;
	return FMath::Min(BasePower + DistanceMeters * PerMetre, MaxPower);
}

FVector UFootballLibrary::GetAimAssistedDirection(FVector AimDirection, FVector ToGoalCenter, float DistanceMeters) {
	// Horizontal only. ApplyKick does normalize(normalize(KickDirection) + Lift*(0,0,1)),
	// so any Z here lands straight on top of the tuned Lift value and skews the trajectory.
	AimDirection.Z = 0.0f;
	ToGoalCenter.Z = 0.0f;

	AimDirection = AimDirection.GetSafeNormal();
	ToGoalCenter = ToGoalCenter.GetSafeNormal();
	if (AimDirection.IsNearlyZero() || ToGoalCenter.IsNearlyZero()) {
		return AimDirection;
	}

	const float Dot = FVector::DotProduct(AimDirection, ToGoalCenter);

	// Fade the assist in across the edge of the cone instead of switching it on at a
	// hard threshold, so a small aim wobble near the boundary can't jump the shot.
	const float ConeEdge = 0.5f;   // ~60 deg off target: no help at all
	const float ConeFull = 0.85f;  // ~32 deg off target: full help
	const float ConeFalloff = FMath::SmoothStep(ConeEdge, ConeFull, Dot);
	if (ConeFalloff <= 0.0f) {
		return AimDirection;
	}

	const float AssistStrength = FMath::Clamp(DistanceMeters / 40.0f, 0.0f, 0.6f) * ConeFalloff;

	FRotator AimRot = AimDirection.Rotation();
	FRotator GoalRot = ToGoalCenter.Rotation();
	FRotator BlendedRot = FMath::Lerp(AimRot, GoalRot, AssistStrength);

	return BlendedRot.Vector();
}
