#include "MotionBricksAnimInstance.h"
#include "MotionBricksComponent.h"
#include "Animation/AnimNodeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

namespace {
// MotionBricks (X left, Y up, Z forward) -> proxy mesh space (X left, Y forward, Z up): a reflection swapping Y and Z, so a
// rotation about axis a by angle t becomes one about swap(a) by -t.
FQuat ToMesh(const FQuat& Q) { return FQuat(-Q.X, -Q.Z, -Q.Y, Q.W); }
}  // namespace

bool FMotionBricksAnimInstanceProxy::BuildMapping(const UMotionBricksComponent* Driver) {
	const USkeletalMeshComponent* MeshComp = GetSkelMeshComponent();
	const USkeletalMesh* Mesh = MeshComp ? MeshComp->GetSkeletalMeshAsset() : nullptr;
	if (!Mesh || Driver->GetJointNames().IsEmpty()) {
		return false;
	}
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	const TArray<FTransform>& RefLocal = Ref.GetRefBonePose();
	TArray<FQuat> Component;
	TArray<FVector> ComponentPos;
	Component.SetNum(Ref.GetNum());
	ComponentPos.SetNum(Ref.GetNum());
	for (int32 B = 0; B < Ref.GetNum(); ++B) {
		const int32 P = Ref.GetParentIndex(B);
		Component[B] = P == INDEX_NONE ? RefLocal[B].GetRotation() : Component[P] * RefLocal[B].GetRotation();
		ComponentPos[B] = P == INDEX_NONE ? RefLocal[B].GetTranslation() : ComponentPos[P] + Component[P].RotateVector(RefLocal[B].GetTranslation());
	}
	const TArray<FName>& Names = Driver->GetJointNames();
	Parents = Driver->GetJointParents();
	MeshBone.SetNum(Names.Num());
	RestComponent.SetNum(Names.Num());
	for (int32 J = 0; J < Names.Num(); ++J) {
		MeshBone[J] = Ref.FindBoneIndex(Names[J]);
		RestComponent[J] = MeshBone[J] == INDEX_NONE ? FQuat::Identity : Component[MeshBone[J]];
	}
	// Hinge stacks share a name without the axis ("left_hip_pitch_skel" -> "left_hip_skel"); joints are listed parents first,
	// so the stack's first member has the lowest index and its last the highest.
	auto StackKey = [](FString Name) {
		for (const TCHAR* Axis : {TEXT("_pitch"), TEXT("_roll"), TEXT("_yaw")}) {
			Name.ReplaceInline(Axis, TEXT(""));
		}
		return Name;
	};
	RotationFrom.SetNum(Names.Num());
	for (int32 J = 0; J < Names.Num(); ++J) {
		const FString Key = StackKey(Names[J].ToString());
		int32 First = J, Last = J;
		for (int32 K = 0; K < Names.Num(); ++K) {
			if (StackKey(Names[K].ToString()) == Key) {
				First = FMath::Min(First, K);
				Last = FMath::Max(Last, K);
			}
		}
		RotationFrom[J] = First == Last ? J : J != First ? Last : Parents[J] < 0 ? J : RotationFrom[Parents[J]];
	}
	// arm limits: the torso joint, each shoulder pivot and the joints it carries
	Torso = Names.IndexOfByKey(FName(TEXT("waist_pitch_skel")));
	Arm.Init(INDEX_NONE, Names.Num());
	for (int32 S = 0; S < 2; ++S) {
		const FString Side = S == 0 ? TEXT("left") : TEXT("right");
		ArmPivot[S] = Names.IndexOfByKey(FName(*(Side + TEXT("_shoulder_roll_skel"))));
		const int32 Elbow = Names.IndexOfByKey(FName(*(Side + TEXT("_elbow_skel"))));
		if (ArmPivot[S] == INDEX_NONE || Elbow == INDEX_NONE || MeshBone[ArmPivot[S]] == INDEX_NONE || MeshBone[Elbow] == INDEX_NONE) {
			ArmPivot[S] = INDEX_NONE;
			continue;
		}
		RestUpperArm[S] = ComponentPos[MeshBone[Elbow]] - ComponentPos[MeshBone[ArmPivot[S]]];
		for (int32 J = 0; J < Names.Num(); ++J) {
			for (int32 K = J; K >= 0; K = Parents[K]) {
				if (K == ArmPivot[S]) {
					Arm[J] = S;
					break;
				}
			}
		}
	}
	return true;
}

void FMotionBricksAnimInstanceProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) {
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
	const AActor* Owner = InAnimInstance ? InAnimInstance->GetOwningActor() : nullptr;
	const UMotionBricksComponent* Driver = Owner ? Owner->FindComponentByClass<UMotionBricksComponent>() : nullptr;
	if (!Driver) {
		bHasPose = false;
		return;
	}
	if (!bMapped) {
		bMapped = BuildMapping(Driver);
	}
	MaxAbduction = Driver->ArmMaxAbduction;
	MaxBackSwing = Driver->ArmMaxBackSwing;
	bHasPose = bMapped && Driver->GetPose(Local, RootHeight, Heading);
}

bool FMotionBricksAnimInstanceProxy::Evaluate(FPoseContext& Output) {
	Output.ResetToRefPose();
	if (!bHasPose || Local.Num() != MeshBone.Num()) {
		return true;
	}
	const int32 Count = Local.Num();
	TArray<FQuat, TInlineAllocator<64>> Global, Mesh;
	Global.SetNum(Count);
	Mesh.SetNum(Count);
	const FQuat Unheading(FVector(0, 1, 0), -Heading);
	for (int32 J = 0; J < Count; ++J) {  // MotionBricks lists parents before children
		Global[J] = Parents[J] < 0 ? Unheading * Local[J] : Global[Parents[J]] * Local[J];
	}
	for (int32 J = 0; J < Count; ++J) {
		Mesh[J] = ToMesh(Global[RotationFrom[J]]) * RestComponent[J];
	}
	// soft arm limits in the torso frame (mesh axes: X left, Y forward, Z up): each upper arm's angle out to the side and
	// back from hanging is eased past half its limit, by turning the whole arm about its shoulder pivot
	if (Torso != INDEX_NONE) {
		auto Ease = [](float Deg, float Max) { return Max <= 0.f || Deg <= 0.5f * Max ? Deg : Max - 0.25f * Max * Max / Deg; };
		const FQuat T = ToMesh(Global[Torso]);
		for (int32 S = 0; S < 2; ++S) {
			if (ArmPivot[S] == INDEX_NONE) {
				continue;
			}
			const float Out = S == 0 ? 1.f : -1.f;  // the left arm goes out along +X
			FVector D = T.UnrotateVector(ToMesh(Global[RotationFrom[ArmPivot[S]]]).RotateVector(RestUpperArm[S]));
			const float Abduction = FMath::RadiansToDegrees(FMath::Atan2(Out * D.X, -D.Z));
			const FQuat In(FVector(0, 1, 0), Out * FMath::DegreesToRadians(Abduction - Ease(Abduction, MaxAbduction)));
			D = In.RotateVector(D);
			const float Back = FMath::RadiansToDegrees(FMath::Atan2(-D.Y, -D.Z));
			const FQuat Forward(FVector(1, 0, 0), FMath::DegreesToRadians(Back - Ease(Back, MaxBackSwing)));
			const FQuat Correction = T * Forward * In * T.Inverse();
			for (int32 J = 0; J < Count; ++J) {
				if (Arm[J] == S) {
					Mesh[J] = Correction * Mesh[J];
				}
			}
		}
	}
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	for (int32 J = 0; J < Count; ++J) {
		if (MeshBone[J] == INDEX_NONE) {
			continue;
		}
		const FCompactPoseBoneIndex Index = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBone[J]));
		if (!Index.IsValid()) {
			continue;
		}
		FTransform& Bone = Output.Pose[Index];
		Bone.SetRotation((Parents[J] < 0 ? Mesh[J] : Mesh[Parents[J]].Inverse() * Mesh[J]).GetNormalized());
		if (Parents[J] < 0) {
			Bone.SetTranslation(FVector(0.0, 0.0, RootHeight * 100.0));
		}
	}
	return true;
}
