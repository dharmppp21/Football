#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FootballTypes.h"
#include "BallAssist.generated.h"

class UCurveFloat;

// Bounded assistance (spec §14, §70, §105): bends a physical result toward an intent, strictly bounded. Stateless, so
// every action uses the same rule and Python can test it without PIE. The assist never teleports the ball, never drags
// it continuously and never replaces the physical result: callers add the result's VelocityChange as one extra impulse
// on top of the physics, at the contact.
UCLASS()
class FOOTBALLARENA_API UBallAssist : public UBlueprintFunctionLibrary {
	GENERATED_BODY()
public:
	// Above this strength only an arcade-like action may go (§14).
	static constexpr float MaxStrengthWithoutArcade = 0.4f;

	// Turns the raw direction toward the intended one by Strength x the angle between them, at most MaxAngleDegrees, and
	// moves the speed toward IntendedSpeed by Strength x the gap, at most MaxSpeedChange either way. Strength is capped at
	// 0.40 unless bArcade. Strength 0 returns the raw velocity.
	UFUNCTION(BlueprintPure, Category = "Football|Assist")
	static FBallAssistResult BoundedBlend(const FBallAssistInput& Input);

	// A player's skill (an attribute, 0-100) through SkillToScale (PlayerSkill -> assist scale, §105) scales the
	// assist's strength and bounds - never the physics, and the 0.40 cap still applies in BoundedBlend. No curve = x1.
	UFUNCTION(BlueprintPure, Category = "Football|Assist")
	static FBallAssistInput ScaleForSkill(FBallAssistInput Input, float Skill, const UCurveFloat* SkillToScale);

	// One of a player's attributes by its property name (e.g. SkillMoves, an action's AttributeProfile); -1 if there is none.
	UFUNCTION(BlueprintPure, Category = "Football|Assist")
	static float GetAttribute(const FFootballAttributes& Attributes, FName Name);
};
