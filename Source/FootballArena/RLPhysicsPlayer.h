#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RLPhysicsPlayer.generated.h"

class UCameraComponent;
class UPoseableMeshComponent;
class UStaticMeshComponent;
struct FRLRuntime;

// One physics footballer playing one kick live: a trained policy drives the body and its ball in MuJoCo inside the game,
// not Chaos, because that is the physics the policy learned on. Files come from RL_Train/live_cpu.py (RLLive/), which is
// also the reference this loop must match. Every kick writes its trace and timing to Saved/RLLive the first time it plays.
UCLASS()
class FOOTBALLARENA_API ARLPhysicsPlayer : public AActor
{
	GENERATED_BODY()

public:
	ARLPhysicsPlayer();
	virtual void Tick(float DeltaSeconds) override;

	// RLLive/<Kick>.bin: the reference clip at its playback rate, the ball and the target speed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RL")
	FString Kick = TEXT("kick_19_x1");

	// RLLive/<Policy>.bin: the network, which must be the one trained on this kind of kick.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RL")
	FString Policy = TEXT("policy_ground");

	// Seconds the ball keeps flying after the clip ends, before the kick starts over.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RL")
	float PauseAtEnd = 1.5f;

	// Take the player's view (off for the extra copies in a cost test).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RL")
	bool bTakeCamera = true;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void UpdatePose();
	void FrameCamera();

	UPROPERTY(VisibleAnywhere, Category = "RL")
	TObjectPtr<UPoseableMeshComponent> BodyMesh;

	UPROPERTY(VisibleAnywhere, Category = "RL")
	TObjectPtr<UStaticMeshComponent> BallMesh;

	UPROPERTY(VisibleAnywhere, Category = "RL")
	TObjectPtr<UCameraComponent> ViewCamera;

	FRLRuntime* Runtime = nullptr;
	TArray<int32> BoneIndex;
};
