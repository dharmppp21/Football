#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Async/Future.h"
#include "MotionBricksComponent.generated.h"

// Test driver for motion-bricks.cpp (localai-org: NVIDIA MotionBricks on GGML; Unitree G1 skeleton, 34 joints, 30 fps, Y-up
// metres): loads its DLL, plans ReplanFrames-frame chunks on a worker thread from the owner's ground velocity and facing, and
// plays them back. UMotionBricksAnimInstance poses the hidden G1 proxy (AFootballCharacter::MotionSource) from GetPose(); the
// Footballer retargets from it.
UCLASS(ClassGroup = (Football), meta = (BlueprintSpawnableComponent))
class FOOTBALLARENA_API UMotionBricksComponent : public UActorComponent {
	GENERATED_BODY()

public:
	UMotionBricksComponent();

	UPROPERTY(EditAnywhere, Category = "MotionBricks")
	FString LibraryDirectory = TEXT("D:/Football Game/ext/motion-bricks.cpp/build/release/bin");

	UPROPERTY(EditAnywhere, Category = "MotionBricks")
	FString ModelDirectory = TEXT("D:/Football Game/ext/motion-bricks.cpp/generated/g1-f32");

	UPROPERTY(EditAnywhere, Category = "MotionBricks")
	FString StylePath = TEXT("D:/Football Game/ext/motion-bricks.cpp/generated/styles/walk.mbstyle");

	// Frames played from each plan before the next one starts from there (the demo's 16 = 0.53 s at 30 fps).
	UPROPERTY(EditAnywhere, Category = "MotionBricks", meta = (ClampMin = "4", ClampMax = "64"))
	int32 ReplanFrames = 16;

	// Commanded speed (m/s) = the owner's ground speed x this.
	UPROPERTY(EditAnywhere, Category = "MotionBricks", meta = (ClampMin = "0"))
	float SpeedScale = 1.f;

	UPROPERTY(EditAnywhere, Category = "MotionBricks", meta = (ClampMin = "1"))
	int32 Threads = 4;

	// Time constant (s) of the G1 pelvis heading that is taken out of the pose; the capsule supplies the facing.
	UPROPERTY(EditAnywhere, Category = "MotionBricks", meta = (ClampMin = "0.01"))
	float HeadingSmoothing = 0.25f;

	// Soft limits (degrees, relative to the torso) on how far the upper arms go out to the side and back: after ~1 s of
	// sprinting the walk style flings them to shoulder height and ~110 deg back. Swing up to half the limit is untouched;
	// beyond it eases towards the limit. 0 = off.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MotionBricks", meta = (ClampMin = "0", ClampMax = "180"))
	float ArmMaxAbduction = 35.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MotionBricks", meta = (ClampMin = "0", ClampMax = "180"))
	float ArmMaxBackSwing = 75.f;

	// Wall time of the last plan (ms) and how many plans have arrived, for the test probes.
	UPROPERTY(Transient, BlueprintReadOnly, Category = "MotionBricks")
	float LastPlanMs = 0.f;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "MotionBricks")
	int32 PlanCount = 0;

	// The pose being shown: local XYZW rotations in MotionBricks joint order, the pelvis height (m) and the smoothed pelvis
	// heading (radians about MotionBricks' up axis). False until the first plan has arrived.
	bool GetPose(TArray<FQuat>& OutLocal, float& OutRootHeight, float& OutHeading) const;
	const TArray<FName>& GetJointNames() const { return JointNames; }
	const TArray<int32>& GetJointParents() const { return JointParents; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	struct FChunk {
		TArray<float> Root;  // [frames, 3]
		TArray<float> Rot;   // [frames, joints, 4] XYZW
		int32 Frames = 0;
		float Ms = 0.f;
	};

	bool Load();
	void Unload();
	void StartPlan();

	void* Dll = nullptr;
	void* Options = nullptr;
	void* Model = nullptr;
	void* Style = nullptr;
	void* Agent = nullptr;
	void* Command = nullptr;
	int32 JointCount = 0;
	TArray<FName> JointNames;
	TArray<int32> JointParents;

	TFuture<FChunk> Pending;
	TArray<float> BufRoot;
	TArray<float> BufRot;
	int32 BufFrames = 0;
	double Playhead = 0.0;  // frames into the buffer

	TArray<FQuat> CurLocal;
	float CurRootHeight = 0.f;
	float CurHeading = 0.f;
	bool bHasPose = false;
	bool bHeadingInit = false;
};
