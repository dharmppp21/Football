#include "AnimNotify_DribbleFoot.h"
#include "Components/SkeletalMeshComponent.h"
#include "PlayerBallInteractionComponent.h"

void UAnimNotify_DribbleFoot::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) {
	Super::Notify(MeshComp, Animation, EventReference);
	if (AActor* Owner = MeshComp ? MeshComp->GetOwner() : nullptr) {
		if (UPlayerBallInteractionComponent* Dribble = Owner->FindComponentByClass<UPlayerBallInteractionComponent>()) {
			Dribble->OnDribbleFootWindow(Foot);
		}
	}
}

FString UAnimNotify_DribbleFoot::GetNotifyName_Implementation() const {
	return Foot == EFootSelection::Left ? TEXT("Dribble L") : TEXT("Dribble R");
}
