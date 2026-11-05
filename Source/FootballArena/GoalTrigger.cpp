#include "GoalTrigger.h"
#include "Components/BoxComponent.h"

AGoalTrigger::AGoalTrigger() {
	TriggerBox = CreateDefaultSubobject<UBoxComponent>(TEXT("TriggerBox"));
	RootComponent = TriggerBox;
	TriggerBox->SetBoxExtent(FVector(15.0f, 353.0f, 120.5f)); // inside a real 7.32 x 2.44 m goal
	TriggerBox->SetCollisionProfileName(TEXT("Trigger"));
	TriggerBox->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);

	TriggerBox->OnComponentBeginOverlap.AddDynamic(this, &AGoalTrigger::OnOverlapBegin);
}

void AGoalTrigger::OnOverlapBegin(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult){
	if (OtherActor && OtherActor->ActorHasTag(TEXT("Ball"))){
		OnGoalScored.Broadcast(OtherActor);
	}
}