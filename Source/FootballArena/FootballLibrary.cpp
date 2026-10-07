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
	const FVector ToGoalCenterRaw = ToGoalCenter; // keep the un-normalized vector to recover PlayerLocation below

	const FVector NormAim = AimDirection.GetSafeNormal();
	const FVector NormGoal = ToGoalCenter.GetSafeNormal();
	if (NormAim.IsNearlyZero() || NormGoal.IsNearlyZero()) {
		return AimDirection;
	}

	const float Dot = FVector::DotProduct(NormAim, NormGoal);

	// Fade the assist in across the edge of the cone instead of switching it on at a
	// hard threshold, so a small aim wobble near the boundary can't jump the shot.
	const float ConeEdge = 0.5f;   // ~60 deg off target: no help at all
	const float ConeFull = 0.85f;  // ~32 deg off target: full help
	const float ConeFalloff = FMath::SmoothStep(ConeEdge, ConeFull, Dot);
	if (ConeFalloff <= 0.0f) {
		return AimDirection;
	}

	// Real FIFA's assisted aim is not "always pull toward the exact centre of the goal" -
	// aiming at the corner flag scores in the corner. It maps your raw direction onto the
	// goal frame and only corrects the part that would otherwise miss. So: find where the
	// player's own aim ray crosses the goal line, clamp that point to stay inside the posts,
	// and bend toward THAT - not the centre. A shot already on target barely moves at all.
	//
	// GoalCentre must match the (5270,0,150) literal wired into BP_FootballPlayer's Tick
	// (feeds the ToGoalCenter pin here) - if that spawn point ever moves, update both.
	const FVector GoalCentre(5270.0f, 0.0f, 150.0f);
	const float GoalHalfWidth = 420.0f; // posts are at +-458.7; a small inset so the target isn't ON the post
	const FVector PlayerLocation = GoalCentre - ToGoalCenterRaw;

	float TargetY = GoalCentre.Y;
	if (FMath::Abs(NormAim.X) > KINDA_SMALL_NUMBER) {
		const float T = (GoalCentre.X - PlayerLocation.X) / NormAim.X;
		if (T > 0.0f) { // only project forward along the aim, never behind the player
			const float RawIntersectY = PlayerLocation.Y + NormAim.Y * T;
			TargetY = FMath::Clamp(RawIntersectY, GoalCentre.Y - GoalHalfWidth, GoalCentre.Y + GoalHalfWidth);
		}
	}

	const FVector TargetPoint(GoalCentre.X, TargetY, GoalCentre.Z);
	FVector ToTarget = (TargetPoint - PlayerLocation);
	ToTarget.Z = 0.0f;
	ToTarget = ToTarget.GetSafeNormal();
	if (ToTarget.IsNearlyZero()) {
		ToTarget = NormGoal; // degenerate: player standing on the target line
	}

	// Strength ramps 0.55 -> 1.0 with range: the old 0-0.6 cap under-corrected even at full
	// cone confidence (a 14deg miss at 20m only closed half the gap and still missed the post).
	// That cap existed to protect corner placement, which the geometric targeting above already
	// guarantees for free (target == raw aim when already on target) - so there is no longer a
	// cost to pulling harder, and a near-miss inside the cone should actually land on target.
	const float DistanceFactor = FMath::Lerp(0.55f, 1.0f, FMath::Clamp(DistanceMeters / 35.0f, 0.0f, 1.0f));
	const float AssistStrength = DistanceFactor * ConeFalloff;

	FRotator AimRot = NormAim.Rotation();
	FRotator TargetRot = ToTarget.Rotation();
	FRotator BlendedRot = FMath::Lerp(AimRot, TargetRot, AssistStrength);

	return BlendedRot.Vector();
}
