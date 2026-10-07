#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "FootballTypes.h"
#include "AnimNotifyState_BallContactWindow.generated.h"

// A contact window on a skill montage (ANS_BallContactWindow, doc §8). While it is open the skill component checks
// contact ContactIndex of the skill's ContactDescriptors (body region, ball region, type, tolerance...) every tick.
// It only carries data: the component reads the windows from the montage when the skill starts and follows the
// montage position, so a window can't be missed in a long frame or lost when it ends on the last frame.
// Spec §12: only the index and the optional mode / label live here; the physics data stays in the action's descriptor.
UCLASS(meta = (DisplayName = "Ball Contact Window"))
class FOOTBALLARENA_API UAnimNotifyState_BallContactWindow : public UAnimNotifyState {
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (ClampMin = "0"))
	int32 ContactIndex = 0;

	// Optional: a response mode for this window in place of the descriptor's (read by the Phase 3 response).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (InlineEditConditionToggle))
	bool bOverrideContactMode = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (EditCondition = "bOverrideContactMode"))
	EBallResponseMode ContactMode = EBallResponseMode::PhysicalTouch;

	// Optional name shown on the montage and in fb.Debug.Contact.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact")
	FName DebugLabel;

	virtual FString GetNotifyName_Implementation() const override;
};
