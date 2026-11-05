#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "MotionBricksAnimInstance.generated.h"

class UMotionBricksComponent;

// Writes UMotionBricksComponent's pose straight into the G1 proxy mesh (no anim graph). MotionBricks' joint rotations are
// relative to its rest pose (identity), so each bone's component-space rest rotation is multiplied by the MotionBricks
// global rotation, mapped into mesh space (Y and Z swapped, the same map as the G1 proxy's FBX) with the smoothed pelvis
// heading removed: the capsule supplies the facing. The pelvis keeps only its height (the capsule supplies the travel).
// The G1 splits each ball joint into a stack of hinges (hip / shoulder / waist pitch-roll-yaw, wrist roll-pitch-yaw, ankle
// pitch-roll). The retargeter's FK chains only pass on rotation relative to a chain's parent bone and interpolate between
// chain bones by length, so a stack is shown as one rigid joint pivoting at its second hinge: the first hinge keeps its rest
// pose and the others get the stack's combined rotation (its last hinge's). A chain starting at the second hinge (hip_roll
// sits over the knee, shoulder_roll over the elbow) then carries the whole joint, and its knee / elbow fall at the same
// fraction of the chain as the Footballer's, so thigh, shin and foot (upper arm, forearm, hand) map one to one.
USTRUCT()
struct FOOTBALLARENA_API FMotionBricksAnimInstanceProxy : public FAnimInstanceProxy {
	GENERATED_BODY()

	FMotionBricksAnimInstanceProxy() = default;
	explicit FMotionBricksAnimInstanceProxy(UAnimInstance* InAnimInstance) : FAnimInstanceProxy(InAnimInstance) {}

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	bool BuildMapping(const UMotionBricksComponent* Driver);

	TArray<int32> MeshBone;        // per MotionBricks joint: the proxy's bone index (INDEX_NONE = absent)
	TArray<int32> Parents;         // per MotionBricks joint
	TArray<int32> RotationFrom;    // per MotionBricks joint: the joint whose MotionBricks global rotation it shows
	TArray<int32> Arm;             // per MotionBricks joint: 0 left / 1 right for the shoulder pivot and below, else -1
	TArray<FQuat> RestComponent;   // per MotionBricks joint: the proxy's component-space rest rotation
	TArray<FQuat> Local;           // this frame's MotionBricks local rotations
	int32 Torso = INDEX_NONE;      // waist_pitch: the torso frame of the arm limits
	int32 ArmPivot[2] = {INDEX_NONE, INDEX_NONE};  // left / right shoulder_roll
	FVector RestUpperArm[2];       // left / right shoulder_roll -> elbow in the proxy's rest component space
	float MaxAbduction = 0.f;      // copied from UMotionBricksComponent::ArmMaxAbduction / ArmMaxBackSwing
	float MaxBackSwing = 0.f;
	float RootHeight = 0.f;
	float Heading = 0.f;
	bool bMapped = false;
	bool bHasPose = false;
};

UCLASS(Transient, Blueprintable)
class FOOTBALLARENA_API UMotionBricksAnimInstance : public UAnimInstance {
	GENERATED_BODY()

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override { return new FMotionBricksAnimInstanceProxy(this); }
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override { delete static_cast<FMotionBricksAnimInstanceProxy*>(InProxy); }
};
