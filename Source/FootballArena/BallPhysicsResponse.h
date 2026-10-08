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
};
