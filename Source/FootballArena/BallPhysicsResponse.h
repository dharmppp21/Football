#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FootballTypes.h"
#include "BallPhysicsResponse.generated.h"

// The raw physical response of the ball to a body contact (spec §7, §91, §122-§129). Stateless, so every action uses the
// same physics and Python can test it without PIE. One impulse model for every impulse mode; the modes only differ in
// the parameters their profiles feed in (a strike: high contact strength; an absorbing trap: low restitution).
UCLASS()
class FOOTBALLARENA_API UBallPhysicsResponse : public UBlueprintFunctionLibrary {
	GENERATED_BODY()
public:
	// A rigid contact between the ball and a moving body surface:
	// - normal impulse: (1 + Restitution) x the approach speed at the contact point, scaled by the effective mass of the
	//   ball at that point and by ContactStrength;
	// - tangential impulse: whatever stops the surfaces sliding (rolling grip), capped by Coulomb friction
	//   (Friction x the normal impulse) - beyond the cap the ball slides;
	// - spin from where it is hit: angular impulse = (contact point - centre) x impulse (spec §91).
	// No impulse when the ball is not closing on the surface, and none for ControlledContact / Capture (not impulses).
	UFUNCTION(BlueprintPure, Category = "Football|Response")
	static FBallResponse ComputeResponse(const FBallResponseInput& Input);

	// What ground friction leaves of a grounded ball's motion once its skid is gone: it rolls, its angular momentum about
	// the ground contact kept - (v - k r (up x w)) / (1 + k) on the ground plane, k = InertiaFactor, w in rad/s. A rolling
	// ball keeps its velocity; one sliding without spin keeps 1 / (1 + k) of it (0.6 for the real ball).
	UFUNCTION(BlueprintPure, Category = "Football|Response")
	static FVector RollingVelocity(FVector Velocity, FVector AngularVelocity, float Radius, float InertiaFactor);
};
