#include "PlayerBallInteractionComponent.h"
#include "BallInteractionComponent.h"
#include "BallMovementComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "FootballSkillComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogFootballPlayerBall, Log, All);

UPlayerBallInteractionComponent::UPlayerBallInteractionComponent() {
	PrimaryComponentTick.bCanEverTick = true; // only draws debug; does nothing when fb.Debug.Regions is off
}

USkeletalMeshComponent* UPlayerBallInteractionComponent::GetMesh() const {
	return GetOwner()->FindComponentByClass<USkeletalMeshComponent>();
}

void UPlayerBallInteractionComponent::BeginPlay() {
	Super::BeginPlay();
	TArray<USceneComponent*> SceneComponents;
	GetOwner()->GetComponents(SceneComponents);
	for (USceneComponent* Component : SceneComponents) {
		if (Component && Component->GetFName() == GuidePointName) {
			GuidePoint = Component;
			break;
		}
	}
	if (!GuidePoint) {
		UE_LOG(LogFootballPlayerBall, Warning, TEXT("%s has no scene component named '%s': dribbling is disabled"), *GetNameSafe(GetOwner()), *GuidePointName.ToString());
	}

	// Body regions, indexed by EBodyRegion.
	const UEnum* Enum = StaticEnum<EBodyRegion>();
	RegionRows.Init(nullptr, Enum->NumEnums() - 1);
	LoadedRegionTable = BodyRegionTable.LoadSynchronous();
	if (LoadedRegionTable) {
		for (int32 i = 0; i < RegionRows.Num(); ++i) {
			RegionRows[i] = LoadedRegionTable->FindRow<FBodyRegionRow>(FName(*Enum->GetNameStringByIndex(i)), TEXT("BodyRegions"), false);
		}
	} else {
		UE_LOG(LogFootballPlayerBall, Warning, TEXT("%s: body region table %s not found: no body contacts"), *GetNameSafe(GetOwner()), *BodyRegionTable.ToString());
	}

	// Report per-body overlaps with the ball (set here, at runtime, so the asset and placed instances stay untouched).
	if (USkeletalMeshComponent* Mesh = GetMesh()) {
		Mesh->bMultiBodyOverlap = true;
		Mesh->SetCollisionResponseToChannel(BallChannel, ECR_Overlap);
		Mesh->SetGenerateOverlapEvents(true);
	}
	// The ball passes through the capsule (it overlaps the Ball channel; contacts go through the body regions), so the
	// movement component must not repel it: UCharacterMovementComponent::ApplyRepulsionForce pushes overlapping physics
	// bodies out of the capsule and stops them dead at its surface.
	if (UCharacterMovementComponent* Movement = GetOwner()->FindComponentByClass<UCharacterMovementComponent>()) {
		Movement->RepulsionForce = 0.f;
	}
}

const FBodyRegionRow* UPlayerBallInteractionComponent::GetRegionRow(EBodyRegion Region) const {
	return RegionRows.IsValidIndex(int32(Region)) ? RegionRows[int32(Region)] : nullptr;
}

int32 UPlayerBallInteractionComponent::GetLoadedRegionCount() const {
	int32 Count = 0;
	for (const FBodyRegionRow* Row : RegionRows) {
		Count += Row ? 1 : 0;
	}
	return Count;
}

FName UPlayerBallInteractionComponent::GetRegionBone(EBodyRegion Region) const {
	const FBodyRegionRow* Row = GetRegionRow(Region);
	const USkeletalMeshComponent* Mesh = GetMesh();
	if (!Row || !Mesh) {
		return NAME_None;
	}
	return Row->Socket.IsNone() ? Row->Bone : Mesh->GetSocketBoneName(Row->Socket);
}

bool UPlayerBallInteractionComponent::GetRegionPoint(EBodyRegion Region, FVector& OutPoint, FVector& OutNormal) const {
	const FBodyRegionRow* Row = GetRegionRow(Region);
	const USkeletalMeshComponent* Mesh = GetMesh();
	if (!Row || !Mesh) {
		return false;
	}
	const FName Space = Row->Socket.IsNone() ? Row->Bone : Row->Socket;
	if (Space.IsNone() || !Mesh->DoesSocketExist(Space)) {
		return false;
	}
	const FTransform Frame = Mesh->GetSocketTransform(Space); // sockets and bones both work here
	OutPoint = Frame.TransformPosition(Row->LocalOffset);
	OutNormal = Frame.TransformVectorNoScale(Row->LocalNormal).GetSafeNormal();
	return true;
}

bool UPlayerBallInteractionComponent::FindContactRegion(FName Bone, const FVector& WorldPoint, EBodyRegion& OutRegion, FVector& OutPoint,
	FVector& OutNormal, const FBodyRegionRow*& OutRow) const {
	const USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh) {
		return false;
	}
	double Best = TNumericLimits<double>::Max();
	for (int32 i = 0; i < RegionRows.Num(); ++i) {
		if (!RegionRows[i]) {
			continue;
		}
		const EBodyRegion Region = EBodyRegion(i);
		const FName RegionBone = GetRegionBone(Region);
		if (!Bone.IsNone() && RegionBone != Bone && !Mesh->BoneIsChildOf(RegionBone, Bone)) {
			continue;
		}
		FVector Point, Normal;
		if (!GetRegionPoint(Region, Point, Normal)) {
			continue;
		}
		const double Distance = FVector::DistSquared(Point, WorldPoint);
		if (Distance < Best) {
			Best = Distance;
			OutRegion = Region;
			OutPoint = Point;
			OutNormal = Normal;
			OutRow = RegionRows[i];
		}
	}
	return Best < TNumericLimits<double>::Max();
}

void UPlayerBallInteractionComponent::UpdateDribble(bool bHasBall, AActor* Ball) {
	// A skill working with the ball has it until the skill ends (doc §31: skill > dribble).
	if (const UFootballSkillComponent* Skill = GetOwner()->FindComponentByClass<UFootballSkillComponent>()) {
		if (Skill->IsSkillUsingBall()) {
			return;
		}
	}
	UBallMovementComponent* Movement = (bHasBall && Ball) ? Ball->FindComponentByClass<UBallMovementComponent>() : nullptr;
	if (DribbledBall.IsValid() && DribbledBall.Get() != Movement) {
		DribbledBall->EndDribble(GetOwner());
		DribbledBall = nullptr;
	}
	if (!Movement || !GuidePoint) {
		return;
	}
	// Doc §31: a controlled dribble touch outranks body and environment contacts, never a skill.
	if (UBallInteractionComponent* Contacts = Ball->FindComponentByClass<UBallInteractionComponent>()) {
		if (!Contacts->TryClaim(EContactPriority::Dribble, GetOwner())) {
			return;
		}
	}
	FVector Guide = GuidePoint->GetComponentLocation();
	if (bAgileDribble) {
		// Pull the guide in along the facing, never push it out.
		const FVector Forward = GetOwner()->GetActorForwardVector().GetSafeNormal2D();
		const double Ahead = FVector::DotProduct(Guide - GetOwner()->GetActorLocation(), Forward);
		Guide -= Forward * FMath::Max(Ahead - AgileGuideDistance, 0.0);
	}
	Movement->ApplyDribble(GetOwner(), Guide, GetOwner()->GetVelocity(), DribbleStrength, MaxDribbleVelocity, DribbleBlend);
	DribbledBall = Movement;
}

void UPlayerBallInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (CVarFootballDebugRegions.GetValueOnGameThread() == 0) {
		return;
	}
	const UEnum* Enum = StaticEnum<EBodyRegion>();
	for (int32 i = 0; i < RegionRows.Num(); ++i) {
		FVector Point, Normal;
		if (!RegionRows[i] || !GetRegionPoint(EBodyRegion(i), Point, Normal)) {
			continue;
		}
		const FColor Color = Enum->GetNameStringByIndex(i).StartsWith(TEXT("Left")) ? FColor::Green
			: Enum->GetNameStringByIndex(i).StartsWith(TEXT("Right")) ? FColor::Red : FColor::Yellow;
		DrawDebugPoint(GetWorld(), Point, 5.f, Color);
		DrawDebugDirectionalArrow(GetWorld(), Point, Point + Normal * 8.f, 3.f, Color);
		if (CVarFootballDebugRegions.GetValueOnGameThread() > 1) {
			DrawDebugString(GetWorld(), Point, Enum->GetNameStringByIndex(i), nullptr, Color, 0.f, false, 0.7f);
		}
	}
}
