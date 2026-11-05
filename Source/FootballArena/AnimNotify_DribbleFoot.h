#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "FootballTypes.h"
#include "AnimNotify_DribbleFoot.generated.h"

// On the run loops, once per stride for each foot: late in the swing, the toe at ball height and still driving forward (its
// forward speed down to half its peak) - where a dribbling foot meets the ball. The touch dribble makes its planned touches at
// these (UPlayerBallInteractionComponent::bFootSyncDribble).
UCLASS(meta = (DisplayName = "Dribble Foot Window"))
class FOOTBALLARENA_API UAnimNotify_DribbleFoot : public UAnimNotify {
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dribble")
	EFootSelection Foot = EFootSelection::Right;

	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;
};
