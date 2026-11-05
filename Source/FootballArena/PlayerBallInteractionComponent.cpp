#include "PlayerBallInteractionComponent.h"
#include "BallInteractionComponent.h"
#include "BallMovementComponent.h"
#include "BallStateComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "FootballActionComponent.h"
#include "PlayerAttributesData.h"
#include "FootballSkillComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogFootballPlayerBall, Log, All);

static TAutoConsoleVariable<int32> CVarFootballDebugDribble(TEXT("fb.Debug.Dribble"), 0,
	TEXT("1 = draw the touch dribble: where the foot meets the ball and its reach (green = the ball can be touched now), and every touch for 1 s (yellow = the ball's new velocity, cyan = where it is sent, with the reason)."));

// Below this asked speed (cm/s) the stick counts as let go; below this ball speed the ball counts as at rest; a stop touch
// plans this far ahead (s) - long enough for the ball to come to rest where it is sent.
static constexpr double DribbleStopInputSpeed = 10.0;
static constexpr double DribbleRestBallSpeed = 30.0;
static constexpr double DribbleStopPlanTime = 5.0;
// A held ball more than this (cm) behind the player's centre is out of reach: he has to turn round to it. 30 = at the heels
// (a 180 degree turn leaves the ball there for a moment; at 15 the turn's last touch missed it, measured 2026-10-10).
static constexpr double DribbleBehindReach = 30.0;
// Faster than this (cm/s), the player's motion, not his facing, decides what is behind him.
static constexpr double DribbleMovingSpeed = 50.0;
// A ball within this (cm) of the player's centre line is "in the middle": his preferred foot takes it.
static constexpr double DribbleFootSideMargin = 5.0;
// A nearly still player (slower than DribbleMovingSpeed) reaches a held ball this close (cm) whichever side of him it is: a
// drag-back or a heel. A moving one only reaches it in front or at his heels (DribbleBehindReach).
static constexpr double DribbleCloseReach = 45.0;
// Foot windows (the run loops' Dribble Foot Window notifies): a window-to-window time between these (s) is a stride (a walk's
// is ~1 s, a sprint's 0.5 s); with either foot's last window older than the timeout the run's rhythm is gone (standing, a skill).
static constexpr double FootStrideMin = 0.2;
static constexpr double FootWindowTimeout = 1.5;
// Below this speed (cm/s) the player is standing or creeping: no stride to keep step with (old windows may still be fresh).
static constexpr double FootSyncMinSpeed = 60.0;
// ChooseKickFoot: one unit of cost per this far (cm) the ball sits across the body from the foot, and per this far (deg) a
// kick goes out past the foot's own side beyond KickOpenAngle; the weak foot's most speed loss (share) and aim error (deg).
static constexpr double KickBallAcrossCost = 20.0;
static constexpr double KickOpenAngle = 30.0;
static constexpr double KickOutsideAngleCost = 45.0;
static constexpr double KickWeakSpeedLoss = 0.15;
static constexpr double KickWeakMaxError = 8.0;
static FBallControlProfile MakeControlProfile(float TouchDistance, float TouchReach, float ControlDistance, float TouchInterval,
	float MaxCorrectionSpeed, float MaxBallSpeed) {
	FBallControlProfile P;
	P.TouchDistance = TouchDistance;
	P.TouchReach = TouchReach;
	P.ControlDistance = ControlDistance;
	P.TouchInterval = TouchInterval;
	P.MaxCorrectionSpeed = MaxCorrectionSpeed;
	P.MaxBallSpeed = MaxBallSpeed;
	return P;
}

// possession still lives in BP_FootballPlayer's variables (bHasBall, ControlledBall, bCanCollect), which its kicks
// and TestForcePossession set too; they are read and written here by name until master plan 7.2 moves them into this component.
static bool GetOwnerFlag(const AActor* Owner, const TCHAR* Name) {
	const FBoolProperty* Flag = FindFProperty<FBoolProperty>(Owner->GetClass(), Name);
	return Flag && Flag->GetPropertyValue_InContainer(Owner);
}

static void SetOwnerFlag(AActor* Owner, const TCHAR* Name, bool bValue) {
	if (const FBoolProperty* Flag = FindFProperty<FBoolProperty>(Owner->GetClass(), Name)) {
		Flag->SetPropertyValue_InContainer(Owner, bValue);
	}
}

static void SetOwnerBall(AActor* Owner, AActor* Ball) {
	SetOwnerFlag(Owner, TEXT("bHasBall"), Ball != nullptr);
	if (const FObjectPropertyBase* Held = FindFProperty<FObjectPropertyBase>(Owner->GetClass(), TEXT("ControlledBall"))) {
		Held->SetObjectPropertyValue_InContainer(Owner, Ball);
	}
}

UPlayerBallInteractionComponent::UPlayerBallInteractionComponent() {
	PrimaryComponentTick.bCanEverTick = true; // only draws debug; does nothing when fb.Debug.Regions is off
	// Touch profiles (cm, s, cm/s): foot point, reach, where a touch sends the ball, interval, correction cap, top speed.
	WalkControl = MakeControlProfile(40.f, 30.f, 55.f, 0.40f, 450.f, 900.f);
	SprintControl = MakeControlProfile(50.f, 32.f, 70.f, 0.60f, 380.f, 1100.f);
	AgileControl = MakeControlProfile(32.f, 25.f, 42.f, 0.28f, 550.f, 600.f);
}

void UPlayerBallInteractionComponent::SetAgileDribble(bool bAgile) {
	bAgileDribble = bAgile;
	UCharacterMovementComponent* Movement = GetOwner()->FindComponentByClass<UCharacterMovementComponent>();
	if (!Movement || AgileMaxSpeed <= 0.f) {
		return;
	}
	if (bAgile && !bAgileSpeedApplied) {
		SpeedBeforeAgile = Movement->MaxWalkSpeed;
		Movement->MaxWalkSpeed = FMath::Min(Movement->MaxWalkSpeed, AgileMaxSpeed);
		bAgileSpeedApplied = true;
	} else if (!bAgile && bAgileSpeedApplied) {
		// "unchanged since" stands in for knowing the sprint state; a sprint pressed meanwhile keeps its speed.
		if (FMath::IsNearlyEqual(Movement->MaxWalkSpeed, FMath::Min(SpeedBeforeAgile, AgileMaxSpeed))) {
			Movement->MaxWalkSpeed = SpeedBeforeAgile;
		}
		bAgileSpeedApplied = false;
	}
}

USkeletalMeshComponent* UPlayerBallInteractionComponent::GetMesh() const {
	// The character's own (visible) mesh, never the hidden motion-matching mannequin (AFootballCharacter::MotionSource).
	if (const ACharacter* Character = Cast<ACharacter>(GetOwner())) {
		return Character->GetMesh();
	}
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
	// The capsule ignores the ball (it used to overlap it): measured 2026-10-10, a touch given while the ball sat inside the
	// moving capsule came out with the player's velocity added (450 cm/s set -> 808 measured, 747 -> 1283 sprinting), a hidden
	// push the old servo overwrote every tick. Nothing reads capsule-ball overlaps; the ball's contacts are the mesh's regions.
	if (UCapsuleComponent* Capsule = GetOwner()->FindComponentByClass<UCapsuleComponent>()) {
		Capsule->SetCollisionResponseToChannel(BallChannel, ECR_Ignore);
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
	if (bTouchDribble) {
		UpdateTouchDribble(bHasBall, Ball);
	} else {
		UpdateServoDribble(bHasBall, Ball);
	}
	// A foot window counts in the frame it came only (whatever the dribble did with it).
	bFootWindowPending[0] = bFootWindowPending[1] = false;
}

void UPlayerBallInteractionComponent::UpdateServoDribble(bool bHasBall, AActor* Ball) {
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
	if (!GuidePoint) {
		return;
	}
	if (!bHasBall) {
		TryCapture();
		return;
	}
	if (!Movement) {
		return;
	}
	const FVector Guide = GuideWithin(bAgileDribble ? AgileGuideDistance : UE_BIG_NUMBER);
	if (FVector::Dist(Ball->GetActorLocation(), Guide) > SeparationLimit) {
		// Spec §78: the ball got away (a skill's exit, a deflection, a miss) - let it go instead of pulling it back.
		SetOwnerBall(GetOwner(), nullptr);
		SetOwnerFlag(GetOwner(), TEXT("bCanCollect"), true);
		Movement->EndDribble(GetOwner());
		return;
	}
	// Doc §31: a controlled dribble touch outranks body and environment contacts, never a skill.
	if (UBallInteractionComponent* Contacts = Ball->FindComponentByClass<UBallInteractionComponent>()) {
		if (!Contacts->TryClaim(EContactPriority::Dribble, GetOwner())) {
			return;
		}
	}
	Movement->ApplyDribble(GetOwner(), Guide, GetOwner()->GetVelocity(), DribbleStrength, MaxDribbleVelocity, DribbleBlend);
	DribbledBall = Movement;
}

FVector UPlayerBallInteractionComponent::GuideWithin(double MaxAhead) const {
	// Pulled in along the facing, never pushed out; its height and sideways offset are kept.
	const FVector Guide = GuidePoint->GetComponentLocation();
	const FVector Forward = GetOwner()->GetActorForwardVector().GetSafeNormal2D();
	const double Ahead = FVector::DotProduct(Guide - GetOwner()->GetActorLocation(), Forward);
	return Guide - Forward * FMath::Max(Ahead - MaxAhead, 0.0);
}

void UPlayerBallInteractionComponent::TryCapture() {
	AActor* Owner = GetOwner();
	AActor* Ball = FindBall();
	const UBallMovementComponent* Movement = Ball ? Ball->FindComponentByClass<UBallMovementComponent>() : nullptr;
	if (!Movement) {
		return;
	}
	// At the feet: where the agile dribble holds the ball, whichever dribble is on.
	const double Distance = FVector::Dist(Ball->GetActorLocation(), GuideWithin(AgileGuideDistance));
	if (!GetOwnerFlag(Owner, TEXT("bCanCollect"))) {
		if (Distance > CaptureRadius) {
			SetOwnerFlag(Owner, TEXT("bCanCollect"), true); // a kicked ball has got clear of the feet
		}
		return;
	}
	const UBallStateComponent* State = Ball->FindComponentByClass<UBallStateComponent>();
	const AActor* Holder = State ? State->GetBallOwner() : nullptr;
	if (Distance <= CaptureRadius && (!Holder || Holder == Owner) && (Movement->GetBallVelocity() - Owner->GetVelocity()).Size() <= CaptureMaxSpeed) {
		SetOwnerBall(Owner, Ball);
	}
}

AActor* UPlayerBallInteractionComponent::FindBall() {
	if (!KnownBall.IsValid()) {
		for (TObjectIterator<UBallMovementComponent> It; It; ++It) {
			if (!It->IsTemplate() && It->GetWorld() == GetWorld() && It->GetOwner()) {
				KnownBall = It->GetOwner();
				break;
			}
		}
	}
	return KnownBall.Get();
}

// ---- the touch dribble -------------------------------------------------------------------------------------------------
// FIFA (EA's Pitch Notes, FIFA 22 / FIFA 23 / FC 26): a dribble is a series of touches at a speed-dependent interval, a loose
// ball is taken by a first touch ("Active Touch"), the player is moved to the ball between touches and sharp turns get their
// own touch. The ball is never pulled: here it changes only when a touch at the feet sends it, and rolls freely in between.

void UPlayerBallInteractionComponent::UpdateTouchDribble(bool bHasBall, AActor* Ball) {
	AActor* Owner = GetOwner();
	// A skill working with the ball has it until the skill ends (doc §31: skill > dribble).
	if (const UFootballSkillComponent* Skill = Owner->FindComponentByClass<UFootballSkillComponent>()) {
		if (Skill->IsSkillUsingBall()) {
			return;
		}
	}
	UBallMovementComponent* Held = (bHasBall && Ball) ? Ball->FindComponentByClass<UBallMovementComponent>() : nullptr;
	if (DribbledBall.IsValid() && DribbledBall.Get() != Held) {
		DribbledBall->EndDribble(Owner);
		DribbledBall = nullptr;
	}
	AActor* Target = Held ? Ball : FindBall();
	UBallMovementComponent* Movement = Held ? Held : (Target ? Target->FindComponentByClass<UBallMovementComponent>() : nullptr);
	if (!Movement) {
		return;
	}
	FDribbleTouchQuery Query = MakeTouchQuery(Target, Movement);
	UBallStateComponent* State = Target->FindComponentByClass<UBallStateComponent>();
	if (!Held) {
		// A loose ball is taken only by a first touch at the feet: one out of reach stays where it is.
		Query.bFirstTouch = true;
		const FDribbleTouchResult First = PlanDribbleTouch(Query);
		DrawDribbleDebug(First, Target);
		if (!GetOwnerFlag(Owner, TEXT("bCanCollect"))) {
			if (First.Distance > Query.Profile.TouchReach) {
				SetOwnerFlag(Owner, TEXT("bCanCollect"), true); // a kicked ball has got clear of the feet
			}
			return;
		}
		const AActor* Holder = State ? State->GetBallOwner() : nullptr;
		if (First.Reason == EDribbleTouchReason::FirstTouch && (!Holder || Holder == Owner)
			&& FVector::Dist2D(Query.BallVelocity, Query.PlayerVelocity) <= CaptureMaxSpeed) {
			SetOwnerBall(Owner, Target);
			MakeDribbleTouch(Target, Movement, First);
			DribbledBall = Movement;
		}
		return;
	}
	const FDribbleTouchResult Plan = PlanDribbleTouch(Query);
	DrawDribbleDebug(Plan, Target);
	if (Plan.Distance > SeparationLimit) {
		// Spec §78: the ball got away (a skill's exit, a deflection, a miss) - let it go instead of pulling it back.
		SetOwnerBall(Owner, nullptr);
		SetOwnerFlag(Owner, TEXT("bCanCollect"), true);
		Movement->EndDribble(Owner);
		DribbledBall = nullptr;
		return;
	}
	// Held between touches: Controlled (the owner's own body ignores it, lower priorities can't take it), but nothing moves it.
	if (State && (State->GetState() != EBallState::Controlled || State->GetBallOwner() != Owner)) {
		State->SetState(EBallState::Controlled, Owner);
	}
	DribbledBall = Movement;
	if (Plan.Reason != EDribbleTouchReason::None) {
		MakeDribbleTouch(Target, Movement, Plan);
	}
}

FVector UPlayerBallInteractionComponent::GetDesiredVelocity() const {
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const UCharacterMovementComponent* Movement = GetOwner()->FindComponentByClass<UCharacterMovementComponent>();
	if (!Pawn || !Movement) {
		return FVector::ZeroVector;
	}
	// The input the character movement used this frame (it ticks before the player's Blueprint, which calls UpdateDribble).
	const FVector Input = Pawn->GetLastMovementInputVector();
	return FVector(Input.X, Input.Y, 0.0).GetClampedToMaxSize(1.0) * Movement->GetMaxSpeed();
}

FVector UPlayerBallInteractionComponent::SteerDribbleInput(const FVector& Input, float MaxSpeed) const {
	const UBallMovementComponent* Ball = DribbledBall.Get();
	const FVector Ask(Input.X, Input.Y, 0.0);
	const double AskSize = FMath::Min(Ask.Size(), 1.0);
	if (!bTouchDribble || !bSteerDribble || !Ball || !Ball->IsGrounded() || AskSize < UE_KINDA_SMALL_NUMBER) {
		return Input;
	}
	if (const UFootballSkillComponent* Skill = GetOwner()->FindComponentByClass<UFootballSkillComponent>()) {
		if (Skill->IsSkillUsingBall()) {
			return Input; // a skill moves the player itself
		}
	}
	const FVector Want = Ask / Ask.Size();
	const FVector Player = GetOwner()->GetActorLocation(), At = Ball->GetOwner()->GetActorLocation();
	const FVector ToBall(At.X - Player.X, At.Y - Player.Y, 0.0);
	const FVector BallVelocity = Ball->GetBallVelocity() * FVector(1.0, 1.0, 0.0);
	// Running where the stick says, is the ball still at the feet ReleaseLookAhead from now (within reach, not behind)? Then go.
	const FVector Then = ToBall + (BallVelocity - Want * (AskSize * MaxSpeed)) * ReleaseLookAhead;
	if (Then.Size() <= HeldTouchReach && FVector::DotProduct(Then, Want) >= -DribbleBehindReach) {
		return Input;
	}
	// Not yet: go to the ball at its pace, closing to CollectGap behind it, so the turn touches reach it and bring it round.
	const double Gap = ToBall.Size();
	const FVector ToDir = Gap > 1.0 ? ToBall / Gap : Want;
	const double Pace = FMath::Max(FVector::DotProduct(BallVelocity, ToDir), 0.0) + CollectGain * (Gap - CollectGap);
	return ToDir * FMath::Clamp(Pace / FMath::Max(double(MaxSpeed), 1.0), 0.0, AskSize);
}

bool UPlayerBallInteractionComponent::IsCarryingBall() const {
	if (!bTouchDribble || !DribbledBall.IsValid()) {
		return false;
	}
	const UFootballSkillComponent* Skill = GetOwner()->FindComponentByClass<UFootballSkillComponent>();
	return !(Skill && Skill->IsSkillUsingBall());
}

FKickFootChoice UPlayerBallInteractionComponent::ChooseKickFoot(FVector PlayerLocation, FVector PlayerForward, FVector BallLocation, FVector KickDirection,
	EFootSelection PreferredFoot, float WeakFoot) {
	const FVector Forward = FVector(PlayerForward.X, PlayerForward.Y, 0.0).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
	const FVector Aim = FVector(KickDirection.X, KickDirection.Y, 0.0).GetSafeNormal(UE_SMALL_NUMBER, Forward);
	const double BallSide = FVector::DotProduct(BallLocation - PlayerLocation, Right); // cm, + = to his right
	const double AimSide = FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(Aim, Right), FVector::DotProduct(Aim, Forward))); // deg, + = right
	const EFootSelection Strong = PreferredFoot == EFootSelection::Left ? EFootSelection::Left : EFootSelection::Right;
	const double Weakness = FMath::Clamp((100.0 - double(WeakFoot)) / 100.0, 0.0, 1.0);
	// Sign: +1 the right foot, -1 the left.
	const auto Cost = [&](double Sign, bool bWeak) {
		return FMath::Max(-Sign * BallSide, 0.0) / KickBallAcrossCost
			+ FMath::Max(Sign * AimSide - KickOpenAngle, 0.0) / KickOutsideAngleCost
			+ (bWeak ? Weakness : 0.0);
	};
	const double RightCost = Cost(1.0, Strong != EFootSelection::Right), LeftCost = Cost(-1.0, Strong != EFootSelection::Left);
	FKickFootChoice Choice;
	Choice.Foot = RightCost < LeftCost ? EFootSelection::Right : LeftCost < RightCost ? EFootSelection::Left : Strong;
	Choice.bWeakFoot = Choice.Foot != Strong;
	if (Choice.bWeakFoot) {
		Choice.SpeedScale = float(1.0 - KickWeakSpeedLoss * Weakness);
		Choice.ErrorDeg = float(KickWeakMaxError * Weakness);
	}
	return Choice;
}

FVector UPlayerBallInteractionComponent::ApplyKickFoot(const FVector& VelocityChange, const FVector& BallLocation) {
	EFootSelection Preferred = EFootSelection::Right;
	float WeakFoot = 50.f;
	if (const UFootballActionComponent* Actions = GetOwner()->FindComponentByClass<UFootballActionComponent>()) {
		if (const UPlayerAttributesData* Attributes = Actions->GetAttributes()) {
			Preferred = Attributes->PreferredFoot;
			WeakFoot = Attributes->Attributes.WeakFoot;
		}
	}
	LastKickFoot = ChooseKickFoot(GetOwner()->GetActorLocation(), GetOwner()->GetActorForwardVector(), BallLocation, VelocityChange, Preferred, WeakFoot);
	const double Off = FMath::FRandRange(-1.0, 1.0) * LastKickFoot.ErrorDeg;
	const FVector Flat = FVector(VelocityChange.X, VelocityChange.Y, 0.0).RotateAngleAxis(Off, FVector::UpVector) * LastKickFoot.SpeedScale;
	UE_LOG(LogFootballPlayerBall, Log, TEXT("Kick: %s foot%s (speed x%.3f, aim %+.1f deg)"), LastKickFoot.Foot == EFootSelection::Left ? TEXT("left") : TEXT("right"),
		LastKickFoot.bWeakFoot ? TEXT(", weak") : TEXT(""), LastKickFoot.SpeedScale, Off);
	return FVector(Flat.X, Flat.Y, VelocityChange.Z * LastKickFoot.SpeedScale);
}

FBallControlProfile UPlayerBallInteractionComponent::GetActiveControlProfile() const {
	if (bAgileDribble) {
		return AgileControl;
	}
	const double Speed = FMath::Max(GetOwner()->GetVelocity().Size2D(), GetDesiredVelocity().Size2D());
	const double Span = FMath::Max(double(SprintControlSpeed - WalkControlSpeed), 1.0);
	return BlendControlProfiles(WalkControl, SprintControl, float(FMath::Clamp((Speed - WalkControlSpeed) / Span, 0.0, 1.0)));
}

FBallControlProfile UPlayerBallInteractionComponent::BlendControlProfiles(const FBallControlProfile& A, const FBallControlProfile& B, float Alpha) {
	const float T = FMath::Clamp(Alpha, 0.f, 1.f);
	return MakeControlProfile(FMath::Lerp(A.TouchDistance, B.TouchDistance, T), FMath::Lerp(A.TouchReach, B.TouchReach, T),
		FMath::Lerp(A.ControlDistance, B.ControlDistance, T), FMath::Lerp(A.TouchInterval, B.TouchInterval, T),
		FMath::Lerp(A.MaxCorrectionSpeed, B.MaxCorrectionSpeed, T), FMath::Lerp(A.MaxBallSpeed, B.MaxBallSpeed, T));
}

FDribbleTouchQuery UPlayerBallInteractionComponent::MakeTouchQuery(const AActor* Ball, const UBallMovementComponent* Movement) const {
	const AActor* Owner = GetOwner();
	FDribbleTouchQuery Q;
	Q.PlayerLocation = Owner->GetActorLocation();
	Q.PlayerVelocity = Owner->GetVelocity();
	Q.PlayerForward = Owner->GetActorForwardVector().GetSafeNormal2D();
	Q.DesiredVelocity = GetDesiredVelocity();
	if (const UCharacterMovementComponent* CharacterMovement = Owner->FindComponentByClass<UCharacterMovementComponent>()) {
		Q.PlayerAcceleration = CharacterMovement->GetMaxAcceleration();
		Q.PlayerBraking = CharacterMovement->GetMaxBrakingDeceleration();
	}
	Q.BallLocation = Ball->GetActorLocation();
	Q.BallVelocity = Movement->GetBallVelocity();
	Q.bBallGrounded = Movement->IsGrounded();
	Movement->GetRollModel(Q.RollDecel, Q.RollDragK);
	Q.Profile = GetActiveControlProfile();
	const double Now = GetWorld()->GetTimeSeconds();
	Q.TimeSinceTouch = float(Now - LastTouchTime);
	Q.TimeToPlannedTouch = float(Now - NextTouchTime);
	Q.LastReason = LastTouchReason;
	Q.HeldReach = HeldTouchReach;
	Q.MinTouchInterval = MinTouchInterval;
	Q.TurnTouchAngle = TurnTouchAngle;
	Q.StopTouchSpeed = StopTouchSpeed;
	// The feet's sideways places (the body regions at the feet) and the preferred foot (the player's attributes).
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Q.PlayerForward);
	FVector Point, Normal;
	if (GetRegionPoint(EBodyRegion::LeftFootCenter, Point, Normal)) {
		Q.LeftFootSide = float(FVector::DotProduct(Point - Q.PlayerLocation, Right));
	}
	if (GetRegionPoint(EBodyRegion::RightFootCenter, Point, Normal)) {
		Q.RightFootSide = float(FVector::DotProduct(Point - Q.PlayerLocation, Right));
	}
	if (const UFootballActionComponent* Actions = Owner->FindComponentByClass<UFootballActionComponent>()) {
		if (const UPlayerAttributesData* Attributes = Actions->GetAttributes()) {
			Q.PreferredFoot = Attributes->PreferredFoot;
		}
	}
	// In step with the feet while he runs and both feet's windows keep coming (the run loops play); not agile dribbling.
	Q.RightWindowAge = float(Now - FootWindowTime[0]);
	Q.LeftWindowAge = float(Now - FootWindowTime[1]);
	Q.bSyncToFeet = bFootSyncDribble && !bAgileDribble && StridePeriod > 0.f && Q.PlayerVelocity.Size2D() > FootSyncMinSpeed
		&& Q.RightWindowAge < FootWindowTimeout && Q.LeftWindowAge < FootWindowTimeout;
	Q.bRightWindow = bFootWindowPending[0];
	Q.bLeftWindow = bFootWindowPending[1];
	Q.StridePeriod = StridePeriod;
	Q.MaxWindowWait = MaxFootWindowWait;
	Q.TurnWindowWait = TurnFootWindowWait;
	return Q;
}

void UPlayerBallInteractionComponent::OnDribbleFootWindow(EFootSelection Foot) {
	if (Foot == EFootSelection::Both || !GetWorld()) {
		return;
	}
	const int32 i = Foot == EFootSelection::Left ? 1 : 0;
	const double Now = GetWorld()->GetTimeSeconds();
	const double Since = Now - FootWindowTime[i];
	if (Since > FootStrideMin && Since < FootWindowTimeout) {
		StridePeriod = StridePeriod > 0.f ? FMath::Lerp(StridePeriod, float(Since), 0.5f) : float(Since);
	}
	FootWindowTime[i] = Now;
	bFootWindowPending[i] = true;
}

FVector UPlayerBallInteractionComponent::PredictPlayerDisplacement(FVector Velocity, FVector DesiredVelocity, float Acceleration, float Braking, float Time) {
	const FVector V0(Velocity.X, Velocity.Y, 0.0), V1(DesiredVelocity.X, DesiredVelocity.Y, 0.0);
	const double T = FMath::Max(double(Time), 0.0);
	const FVector Change = V1 - V0;
	const double Rate = FMath::Max(double(V1.Size() >= V0.Size() ? Acceleration : Braking), 1.0);
	const double Reach = Change.Size() / Rate; // seconds until the velocity is the asked one (a straight-line change)
	if (Reach <= T) {
		return V0 * Reach + Change * (0.5 * Reach) + V1 * (T - Reach);
	}
	return V0 * T + Change * (0.5 * T * T / Reach);
}

// In step with the feet: a planned touch at the touching foot's window, once a stride, or - the run's rhythm broke (a stop, a
// skill, the ball changed sides) - a stride late without it, never sooner than MaxWindowWait. Otherwise when the interval is up.
static bool PlannedTouchDue(const FDribbleTouchQuery& Q, EFootSelection Foot) {
	if (!Q.bSyncToFeet) {
		return Q.TimeToPlannedTouch >= 0.f;
	}
	const bool bWindow = Foot == EFootSelection::Left ? Q.bLeftWindow : Q.bRightWindow;
	return (bWindow && Q.TimeSinceTouch >= 0.5f * Q.StridePeriod) || Q.TimeToPlannedTouch >= FMath::Max(Q.MaxWindowWait, Q.StridePeriod);
}

// Seconds to the touching foot's next window that a planned touch may use (at least half a stride after this touch).
static double NextFootWindowIn(const FDribbleTouchQuery& Q, EFootSelection Foot) {
	const double Stride = Q.StridePeriod;
	const double Age = FMath::Max(double(Foot == EFootSelection::Left ? Q.LeftWindowAge : Q.RightWindowAge), 0.0);
	double Next = Stride - FMath::Fmod(Age, Stride);
	if (Next < 0.5 * Stride) {
		Next += Stride;
	}
	return Next;
}

FDribbleTouchResult UPlayerBallInteractionComponent::PlanDribbleTouch(const FDribbleTouchQuery& Q) {
	FDribbleTouchResult R;
	const FBallControlProfile& P = Q.Profile;
	const FVector Forward = FVector(Q.PlayerForward.X, Q.PlayerForward.Y, 0.0).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
	const FVector Player(Q.PlayerLocation.X, Q.PlayerLocation.Y, 0.0);
	const FVector Ball(Q.BallLocation.X, Q.BallLocation.Y, 0.0);
	const FVector BallVelocity(Q.BallVelocity.X, Q.BallVelocity.Y, 0.0);
	const FVector PlayerVelocity(Q.PlayerVelocity.X, Q.PlayerVelocity.Y, 0.0);
	const FVector Desired(Q.DesiredVelocity.X, Q.DesiredVelocity.Y, 0.0);
	// The foot that meets the ball: the one on the ball's side of the body, the preferred one with the ball in the middle;
	// the ball is met, and later sent, in front of that foot.
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
	const double BallSide = FVector::DotProduct(Ball - Player, Right);
	R.Foot = BallSide > DribbleFootSideMargin ? EFootSelection::Right
		: BallSide < -DribbleFootSideMargin ? EFootSelection::Left
		: (Q.PreferredFoot == EFootSelection::Left ? EFootSelection::Left : EFootSelection::Right);
	double FootSide = R.Foot == EFootSelection::Left ? Q.LeftFootSide : Q.RightFootSide;
	R.TouchSpot = Player + Forward * P.TouchDistance + Right * FootSide;
	R.Distance = float(FVector::Dist(Ball, R.TouchSpot));
	// A loose ball must be at the touch point to be taken; a held one wherever the legs reach (so a turn can bring it round).
	const FVector FromPlayer = Ball - Player;
	// "Behind" is judged along where the player is going, not where he faces: on a sharp turn the facing whips round at
	// the rotation rate while the body still moves the old way, so the ball just left in front would count as behind.
	const FVector Going = PlayerVelocity.Size() > DribbleMovingSpeed ? PlayerVelocity.GetSafeNormal() : Forward;
	const bool bNearBody = FromPlayer.Size() <= Q.HeldReach && (FVector::DotProduct(FromPlayer, Going) >= -DribbleBehindReach
		|| (PlayerVelocity.Size() <= DribbleMovingSpeed && FromPlayer.Size() <= DribbleCloseReach));
	R.bInReach = Q.bBallGrounded && (Q.bFirstTouch ? R.Distance <= P.TouchReach : bNearBody);
	// The anti-magnetism rule (spec §70): out of the foot's reach nothing touches the ball; and never touch it every tick.
	if (!R.bInReach || (!Q.bFirstTouch && Q.TimeSinceTouch < Q.MinTouchInterval)) {
		return R;
	}
	const double DesiredSpeed = Desired.Size(), BallSpeed = BallVelocity.Size();
	const bool bStop = DesiredSpeed < DribbleStopInputSpeed;
	const FVector Direction = bStop ? Forward : Desired / DesiredSpeed;
	// Off course: where the ball rolls to by the next planned touch, against where the foot will want it then.
	const auto OffCourse = [&]() {
		const double Ahead = -double(Q.TimeToPlannedTouch);
		if (Ahead <= 0.0) {
			return false;
		}
		FVector Unused;
		const FVector BallThen = UBallMovementComponent::PredictFreeMotion(Ball, BallVelocity, false, 0.f, -980.f, Q.RollDecel, Q.RollDragK, float(Ahead), Unused);
		const FVector SpotThen = Player + PredictPlayerDisplacement(PlayerVelocity, Desired, Q.PlayerAcceleration, Q.PlayerBraking, float(Ahead))
			+ Direction * P.ControlDistance;
		return FVector::Dist(FVector(BallThen.X, BallThen.Y, 0.0), SpotThen) > P.TouchReach;
	};
	EDribbleTouchReason Reason = EDribbleTouchReason::None;
	if (Q.bFirstTouch) {
		Reason = EDribbleTouchReason::FirstTouch;
	} else if (bStop) {
		if (BallSpeed > Q.StopTouchSpeed && (Q.LastReason != EDribbleTouchReason::Stop || Q.TimeSinceTouch >= P.TouchInterval)) {
			Reason = EDribbleTouchReason::Stop;
		}
	} else if (BallSpeed < DribbleRestBallSpeed) {
		Reason = EDribbleTouchReason::Start;
	} else if (FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(BallVelocity / BallSpeed, Direction), -1.0, 1.0))) > Q.TurnTouchAngle) {
		Reason = EDribbleTouchReason::Turn;
	} else if (FVector::DotProduct(Ball - R.TouchSpot, Forward) < 0.0 && FVector::DotProduct(PlayerVelocity - BallVelocity, Forward) > 0.0) {
		Reason = EDribbleTouchReason::Overrun;
	} else if (PlannedTouchDue(Q, R.Foot)) {
		Reason = EDribbleTouchReason::Planned;
	} else if (OffCourse()) {
		Reason = EDribbleTouchReason::Correct;
	}
	if (Reason == EDribbleTouchReason::None) {
		return R;
	}
	// In step with the feet, a turn or a correction waits a moment for a foot to come through - either foot: in a turn the ball
	// changes sides - and is made by that foot; with no foot coming within TurnWindowWait it is made at once.
	if (Q.bSyncToFeet && Q.StridePeriod > 0.f && Q.TurnWindowWait > 0.f
		&& (Reason == EDribbleTouchReason::Turn || Reason == EDribbleTouchReason::Correct)) {
		if (Q.bLeftWindow != Q.bRightWindow) {
			R.Foot = Q.bLeftWindow ? EFootSelection::Left : EFootSelection::Right;
			FootSide = R.Foot == EFootSelection::Left ? Q.LeftFootSide : Q.RightFootSide;
			R.TouchSpot = Player + Forward * P.TouchDistance + Right * FootSide;
		} else if (!Q.bLeftWindow) {
			const double Stride = Q.StridePeriod;
			const double NextR = Stride - FMath::Fmod(FMath::Max(double(Q.RightWindowAge), 0.0), Stride);
			const double NextL = Stride - FMath::Fmod(FMath::Max(double(Q.LeftWindowAge), 0.0), Stride);
			if (FMath::Min(NextR, NextL) <= Q.TurnWindowWait) {
				return R;
			}
		}
	}
	// Where the ball should be when the next touch is due: ControlDistance in front of where the player will be then (in step
	// with the feet: at the touching foot's next usable window). With the stick let go it comes to rest TouchDistance in front of
	// where the player stops (a stop touch / a trap).
	const double Time = bStop ? DribbleStopPlanTime
		: Q.bSyncToFeet && Q.StridePeriod > 0.f ? NextFootWindowIn(Q, R.Foot) : double(P.TouchInterval);
	const FVector Moved = PredictPlayerDisplacement(PlayerVelocity, bStop ? FVector::ZeroVector : Desired, Q.PlayerAcceleration, Q.PlayerBraking, float(Time));
	R.Target = Player + Moved + Direction * (bStop ? P.TouchDistance : P.ControlDistance)
		+ FVector::CrossProduct(FVector::UpVector, Direction) * FootSide;
	const FVector Chord = R.Target - Ball;
	const double Speed = UBallMovementComponent::SpeedToRoll(float(Chord.Size()), float(Time), Q.RollDecel, Q.RollDragK);
	const FVector Want = Chord.GetSafeNormal() * FMath::Min(Speed, double(P.MaxBallSpeed));
	// Spec §21 / §70: a bounded correction - a sharp turn takes more than one touch, the ball curves round instead of snapping.
	FVector Change = Want - BallVelocity;
	R.bCapped = Change.Size() > P.MaxCorrectionSpeed;
	Change = Change.GetClampedToMaxSize(P.MaxCorrectionSpeed);
	R.Correction = float(Change.Size());
	R.Velocity = FVector(BallVelocity.X + Change.X, BallVelocity.Y + Change.Y, Q.BallVelocity.Z);
	R.Interval = float(Time);
	R.Reason = Reason;
	return R;
}

void UPlayerBallInteractionComponent::MakeDribbleTouch(AActor* Ball, UBallMovementComponent* Movement, const FDribbleTouchResult& Touch) {
	AActor* Owner = GetOwner();
	UBallInteractionComponent* Contacts = Ball->FindComponentByClass<UBallInteractionComponent>();
	// Doc §31: a dribble touch outranks body and environment contacts, never a skill or a strike.
	if (Contacts && !Contacts->TryClaim(EContactPriority::Dribble, Owner)) {
		return;
	}
	// The ball leaves the touch rolling (no skid), keeping its twist about the vertical (a curled ball's spin).
	FVector VelocityNow = Movement->GetBallVelocity(), SpinDeg = FVector::ZeroVector;
	if (Contacts) {
		Contacts->GetPendingMotion(VelocityNow, SpinDeg);
	}
	const double Radius = FMath::Max(double(Movement->GetBallRadius()), 1.0);
	const FVector Roll = FVector::CrossProduct(FVector::UpVector, FVector(Touch.Velocity.X, Touch.Velocity.Y, 0.0)) / Radius;
	Movement->ApplyTouch(Touch.Velocity, FMath::RadiansToDegrees(Roll) + FVector(0.0, 0.0, SpinDeg.Z), EBallState::Controlled, Owner);
	const double Now = GetWorld()->GetTimeSeconds();
	LastTouchTime = Now;
	// A capped touch didn't finish its correction (a sharp turn): the next one follows as soon as touches may.
	NextTouchTime = Now + (Touch.bCapped ? MinTouchInterval : Touch.Interval);
	LastTouchReason = Touch.Reason;
	++DribbleTouchCount;
	LastDribbleTouch = Touch;
	LastDribbleTouch.Time = float(Now);
	if (CVarFootballDebugDribble.GetValueOnGameThread() != 0) {
		const FVector From = Ball->GetActorLocation();
		DrawDebugDirectionalArrow(GetWorld(), From, From + FVector(Touch.Velocity.X, Touch.Velocity.Y, 0.0) * 0.15, 8.f, FColor::Yellow, false, 1.f, 0, 1.5f);
		DrawDebugSphere(GetWorld(), FVector(Touch.Target.X, Touch.Target.Y, From.Z), 6.f, 8, FColor::Cyan, false, 1.f);
		DrawDebugString(GetWorld(), From + FVector(0.0, 0.0, 30.0), FString::Printf(TEXT("%s %s  %.0f cm/s%s"),
			Touch.Foot == EFootSelection::Left ? TEXT("L") : TEXT("R"), *UEnum::GetDisplayValueAsText(Touch.Reason).ToString(),
			Touch.Velocity.Size2D(), Touch.bCapped ? TEXT(" (capped)") : TEXT("")), nullptr,
			FColor::Yellow, 1.f);
	}
}

void UPlayerBallInteractionComponent::DrawDribbleDebug(const FDribbleTouchResult& Plan, const AActor* Ball) const {
	if (CVarFootballDebugDribble.GetValueOnGameThread() == 0) {
		return;
	}
	const double Ground = Ball->GetActorLocation().Z - 10.0;
	const FVector Spot(Plan.TouchSpot.X, Plan.TouchSpot.Y, Ground);
	DrawDebugCircle(GetWorld(), Spot, GetActiveControlProfile().TouchReach, 24, Plan.bInReach ? FColor::Green : FColor(128, 128, 128), false, -1.f, 0, 1.f,
		FVector(1.0, 0.0, 0.0), FVector(0.0, 1.0, 0.0), false);
	if (DribbledBall.IsValid()) { // held: where the legs can still reach it
		const FVector At = GetOwner()->GetActorLocation();
		DrawDebugCircle(GetWorld(), FVector(At.X, At.Y, Ground), HeldTouchReach, 32, FColor(60, 120, 255), false, -1.f, 0, 0.5f, FVector(1.0, 0.0, 0.0),
			FVector(0.0, 1.0, 0.0), false);
	}
	DrawDebugString(GetWorld(), Spot + FVector(0.0, 0.0, 60.0), FString::Printf(TEXT("touches %d  ball %.0f cm from the foot"), DribbleTouchCount,
		Plan.Distance), nullptr, FColor::White, 0.f);
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
