#include "FootballSkillComponent.h"
#include "AnimNotifyState_BallContactWindow.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BallContactResolver.h"
#include "BallInteractionComponent.h"
#include "BallMovementComponent.h"
#include "BallStateComponent.h"
#include "BallTrajectoryData.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "FootballSkillData.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "PlayerBallInteractionComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogFootballSkill, Log, All);

// Carries: the body stays on the ball and pushes it along (a sweep, a drag, a trap); the ball separates by itself when
// the body stops pushing. The other contact types are one collision (a touch, a strike, a header).
static bool IsCarry(EContactType Type) {
	return Type == EContactType::Control || Type == EContactType::Trap || Type == EContactType::Redirect || Type == EContactType::Release;
}

UFootballSkillComponent::UFootballSkillComponent() {
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false; // ticks only while a skill plays, and for the recorder's tail
	PrimaryComponentTick.TickGroup = TG_PrePhysics;     // ball moves made here go into this frame's physics step
	SkillDirectory.Path = TEXT("/Game/Football/Data/Skills");
}

void UFootballSkillComponent::BeginPlay() {
	Super::BeginPlay();
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s: the skill component needs a Character with a mesh"), *GetNameSafe(GetOwner()));
		return;
	}
	// Body regions are read after this frame's pose is evaluated.
	AddTickPrerequisiteComponent(Mesh);
	// The montages play on the AnimBP's slot. Switched here, at runtime, so the assets and placed instances stay untouched.
	if (UClass* Class = AnimClass.LoadSynchronous()) {
		if (Mesh->GetAnimationMode() != EAnimationMode::AnimationBlueprint || Mesh->GetAnimClass() != Class) {
			Mesh->SetAnimInstanceClass(Class);
		}
	} else if (!AnimClass.IsNull()) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s: AnimBP %s not found"), *GetNameSafe(GetOwner()), *AnimClass.ToString());
	}
	Regions = GetOwner()->FindComponentByClass<UPlayerBallInteractionComponent>();
	LoadSkills();
}

void UFootballSkillComponent::LoadSkills() {
	LoadedSkills.Reset();
	TArray<FAssetData> Assets;
	if (const IAssetRegistry* Registry = IAssetRegistry::Get()) {
		Registry->GetAssetsByPath(FName(*SkillDirectory.Path), Assets, true);
	}
	for (const FAssetData& Asset : Assets) {
		if (Asset.IsInstanceOf(UFootballSkillData::StaticClass())) {
			if (UFootballSkillData* Skill = Cast<UFootballSkillData>(Asset.GetAsset())) {
				LoadedSkills.Add(Skill);
			}
		}
	}
	UE_LOG(LogFootballSkill, Log, TEXT("%s: %d skills in %s"), *GetNameSafe(GetOwner()), LoadedSkills.Num(), *SkillDirectory.Path);
}

USkeletalMeshComponent* UFootballSkillComponent::GetMesh() const {
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	return Character ? Character->GetMesh() : nullptr;
}

UAnimInstance* UFootballSkillComponent::GetAnimInstance() const {
	const USkeletalMeshComponent* Mesh = GetMesh();
	return Mesh ? Mesh->GetAnimInstance() : nullptr;
}

FAnimMontageInstance* UFootballSkillComponent::GetMontageInstance() const {
	UAnimInstance* Anim = GetAnimInstance();
	return Anim ? Anim->GetMontageInstanceForID(MontageInstanceId) : nullptr;
}

float UFootballSkillComponent::GetPlayRate() const {
	const FAnimMontageInstance* Instance = GetMontageInstance();
	const float Rate = (Instance ? Instance->GetPlayRate() : (ActiveSkill ? ActiveSkill->PlayRate : 1.f)) * (ActiveMontage ? ActiveMontage->RateScale : 1.f);
	return FMath::Max(Rate, UE_KINDA_SMALL_NUMBER);
}

TArray<UFootballSkillComponent::FWindow> UFootballSkillComponent::CollectWindows(const UFootballSkillData& Skill, const UAnimMontage& InMontage) {
	TArray<FWindow> Result;
	for (const FAnimNotifyEvent& Event : InMontage.Notifies) {
		const UAnimNotifyState_BallContactWindow* Notify = Cast<UAnimNotifyState_BallContactWindow>(Event.NotifyStateClass);
		if (!Notify) {
			continue;
		}
		if (!Skill.ContactDescriptors.IsValidIndex(Notify->ContactIndex)) {
			UE_LOG(LogFootballSkill, Warning, TEXT("%s: the window at %.2f s points at contact %d, but the skill has %d"),
				*Skill.GetName(), Event.GetTriggerTime(), Notify->ContactIndex, Skill.ContactDescriptors.Num());
			continue;
		}
		FWindow& Window = Result.AddDefaulted_GetRef();
		Window.Start = Event.GetTriggerTime();
		Window.End = Event.GetEndTriggerTime();
		Window.ContactIndex = Notify->ContactIndex;
		Window.Label = Notify->DebugLabel;
	}
	Result.Sort([](const FWindow& A, const FWindow& B) { return A.Start < B.Start; });
	return Result;
}

FVector2D UFootballSkillComponent::SampleTrackOf(const UFootballSkillData& Skill, const UAnimMontage& InMontage, float T) {
	if (Skill.RootMotionMode != ESkillRootMotion::BakedTrack || InMontage.SlotAnimTracks.Num() == 0) {
		return FVector2D::ZeroVector;
	}
	const FAnimTrack& Track = InMontage.SlotAnimTracks[0].AnimTrack;
	const float Clamped = FMath::Clamp(T, 0.f, Track.GetLength());
	const FAnimSegment* Segment = Track.GetSegmentAtTime(Clamped);
	const UAnimSequenceBase* Sequence = Segment ? Segment->GetAnimReference().Get() : nullptr;
	if (!Sequence) {
		return FVector2D::ZeroVector;
	}
	const FAnimExtractContext Context(double(Segment->ConvertTrackPosToAnimPos(Clamped)));
	return FVector2D(Sequence->EvaluateCurveData(Skill.RootForwardCurve, Context), Sequence->EvaluateCurveData(Skill.RootRightCurve, Context));
}

FVector2D UFootballSkillComponent::SampleTrack(float T) const {
	return (ActiveSkill && ActiveMontage) ? SampleTrackOf(*ActiveSkill, *ActiveMontage, T) : FVector2D::ZeroVector;
}

FVector UFootballSkillComponent::TrackVelocity(float T) const {
	const float Step = 1.f / 60.f;
	const float A = FMath::Max(T - Step, 0.f);
	const float B = FMath::Min(T + Step, MontageLength);
	if (B <= A) {
		return FVector::ZeroVector;
	}
	const FVector2D Velocity = (SampleTrack(B) - SampleTrack(A)) / (B - A) * GetPlayRate();
	return Basis.TransformVector(FVector(Velocity.X, Velocity.Y, 0.0));
}

void UFootballSkillComponent::ApplyTravel(float T) {
	const FVector2D Track = SampleTrack(T);
	const FVector2D Delta = Track - PrevTrack;
	PrevTrack = Track;
	if (!Delta.IsNearlyZero()) {
		GetOwner()->AddActorWorldOffset(Basis.TransformVector(FVector(Delta.X, Delta.Y, 0.0)), true);
	}
}

void UFootballSkillComponent::UpdateTurn() {
	if (!bTurning) {
		return;
	}
	const float Alpha = TurnDuration > 0.f ? FMath::Clamp(float(GetWorld()->GetTimeSeconds() - StartTime) / TurnDuration, 0.f, 1.f) : 1.f;
	GetOwner()->SetActorRotation(FQuat::Slerp(TurnFrom, TurnTo, Alpha));
	bTurning = Alpha < 1.f;
}

FVector UFootballSkillComponent::GroundRoot() const {
	const FVector Location = GetOwner()->GetActorLocation();
	return FVector(Location.X, Location.Y, BallMovement ? BallMovement->GetGroundZ() : Location.Z);
}

bool UFootballSkillComponent::RequestSkill(UFootballSkillData* Skill, AActor* Ball, FVector InputDirection, bool bMirrored) {
	ACharacter* Character = Cast<ACharacter>(GetOwner());
	UAnimInstance* Anim = GetAnimInstance();
	const FString SkillName = GetNameSafe(Skill);
	if (ActiveSkill) {
		UE_LOG(LogFootballSkill, Log, TEXT("%s refused: %s is still playing"), *SkillName, *ActiveSkill->GetName());
		return false;
	}
	if (!Skill || !Character || !Anim) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s refused: no skill, or the owner is not a Character with an AnimBP"), *SkillName);
		return false;
	}
	if ((bMirrored && Skill->AllowedFeet == EFootSelection::Right) || (!bMirrored && Skill->AllowedFeet == EFootSelection::Left)) {
		UE_LOG(LogFootballSkill, Log, TEXT("%s refused: not allowed with the %s foot"), *SkillName, bMirrored ? TEXT("left") : TEXT("right"));
		return false;
	}
	UAnimMontage* NewMontage = Skill->GetMontage(bMirrored);
	if (!NewMontage || NewMontage->SlotAnimTracks.Num() == 0) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s refused: no %s montage"), *SkillName, bMirrored ? TEXT("mirrored") : TEXT("right-foot"));
		return false;
	}
	const FName Slot = NewMontage->SlotAnimTracks[0].SlotName;
	if (Anim->GetSlotNodeGlobalWeight(Slot) <= 0.f) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s refused: the AnimBP %s has no active slot %s"), *SkillName, *Anim->GetClass()->GetName(), *Slot.ToString());
		return false;
	}

	TArray<FWindow> NewWindows = CollectWindows(*Skill, *NewMontage);
	ESkillDirection Direction = ESkillDirection::Forward;
	const FSkillBasis NewBasis = UBallContactResolver::MakeSkillBasisFromInput(Character->GetActorLocation(), Character->GetActorForwardVector(),
		InputDirection, Skill->AllowedDirections, bMirrored, Direction);
	UBallMovementComponent* Movement = Ball ? Ball->FindComponentByClass<UBallMovementComponent>() : nullptr;
	UBallStateComponent* State = Ball ? Ball->FindComponentByClass<UBallStateComponent>() : nullptr;
	if (NewWindows.Num() > 0) {
		if (!Movement || !State || !Skill->TrajectoryProfile || !Regions) {
			UE_LOG(LogFootballSkill, Warning, TEXT("%s refused: it needs a ball with BallMovement and BallState, a TrajectoryProfile, and PlayerBallInteraction on the player"), *SkillName);
			return false;
		}
		const UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Ball->GetRootComponent());
		const EBallState BallNow = State->GetState();
		const AActor* BallOwner = State->GetBallOwner();
		if (!Body || !Body->IsSimulatingPhysics() || !UBallStateComponent::IsTransitionAllowed(BallNow, EBallState::SkillControlled)
			|| (BallOwner && BallOwner != Character && (BallNow == EBallState::Controlled || BallNow == EBallState::Receiving))) {
			UE_LOG(LogFootballSkill, Log, TEXT("%s refused: the ball is %s (owner %s)"), *SkillName, *UEnum::GetValueAsString(BallNow), *GetNameSafe(BallOwner));
			return false;
		}
		const double Distance = FVector::Dist2D(Ball->GetActorLocation(), Character->GetActorLocation());
		if (Distance > Skill->MaxStartDistance) {
			UE_LOG(LogFootballSkill, Log, TEXT("%s refused: the ball is %.0f cm away (max %.0f)"), *SkillName, Distance, Skill->MaxStartDistance);
			return false;
		}
		// The ball has to be able to reach the first contact in time (the path is relative to the skill's start).
		const FWindow& First = NewWindows[0];
		const FVector2D Track0 = SampleTrackOf(*Skill, *NewMontage, 0.f);
		const FVector Path = Skill->TrajectoryProfile->Evaluate(First.Start / FMath::Max(NewMontage->GetPlayLength(), UE_KINDA_SMALL_NUMBER));
		const FVector FirstTarget = Character->GetActorLocation() + NewBasis.TransformVector(FVector(Path.X - Track0.X, Path.Y - Track0.Y, 0.0));
		const double Gap = FVector::Dist2D(Ball->GetActorLocation(), FirstTarget);
		const double Reach = Skill->MaxApproachSpeed * First.Start / FMath::Max(Skill->PlayRate * NewMontage->RateScale, UE_KINDA_SMALL_NUMBER)
			+ Skill->GetTunedDescriptor(First.ContactIndex, bMirrored).ContactTolerance;
		if (Gap > Reach) {
			UE_LOG(LogFootballSkill, Log, TEXT("%s refused: the ball is %.0f cm from the first contact, only %.0f cm reachable in time"), *SkillName, Gap, Reach);
			return false;
		}
	}

	if (Anim->Montage_Play(NewMontage, Skill->PlayRate) <= 0.f) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s refused: %s did not play"), *SkillName, *NewMontage->GetName());
		return false;
	}
	const FAnimMontageInstance* Instance = Anim->GetActiveInstanceForMontage(NewMontage);
	if (!Instance) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s refused: %s has no montage instance"), *SkillName, *NewMontage->GetName());
		return false;
	}
	MontageInstanceId = Instance->GetInstanceID();
	FOnMontageEnded EndDelegate = FOnMontageEnded::CreateUObject(this, &UFootballSkillComponent::OnMontageEnded, ++RequestId);
	Anim->Montage_SetEndDelegate(EndDelegate, NewMontage);

	ActiveSkill = Skill;
	ActiveMontage = NewMontage;
	MontageLength = FMath::Max(NewMontage->GetPlayLength(), UE_KINDA_SMALL_NUMBER);
	Windows = MoveTemp(NewWindows);
	Basis = NewBasis;
	bActiveMirrored = bMirrored;
	SkillDirection = Direction;
	ActiveBall = Ball;
	BallMovement = Movement;
	BallStateComp = State;
	BallContacts = Ball ? Ball->FindComponentByClass<UBallInteractionComponent>() : nullptr;
	bApproaching = Windows.Num() > 0;
	bOwned = false;
	bEverTouched = false;
	bReleased = false;
	LastMontageTime = 0.f;
	PrevTrack = SampleTrack(0.f);
	TrackedWindow = INDEX_NONE;
	UBallContactResolver::StopTracking(Tracker);
	LastEvaluated = INDEX_NONE;
	bTouchedThisTick = false;
	LastTarget = Ball ? Ball->GetActorLocation() : FVector::ZeroVector;
	StartTime = GetWorld()->GetTimeSeconds();
	TurnFrom = Character->GetActorQuat();
	TurnTo = Basis.Forward.ToOrientationQuat();
	TurnDuration = NewMontage->BlendIn.GetBlendTime();
	bTurning = true;
	Character->GetCharacterMovement()->StopMovementImmediately();
	if (AController* Controller = Character->GetController()) {
		Controller->SetIgnoreMoveInput(true); // undone in Finish
		IgnoredController = Controller;
	}
	Record.Reset();
	RecordUntil = 0.0;
	SetSkillState(ESkillState::Starting, Skill);
	RecordSample(0.f);
	SetComponentTickEnabled(true);
	UE_LOG(LogFootballSkill, Log, TEXT("%s started (%s, %s foot, %d contacts)"), *SkillName, *UEnum::GetValueAsString(Direction),
		bMirrored ? TEXT("left") : TEXT("right"), Windows.Num());
	return true;
}

bool UFootballSkillComponent::RequestSkillByTag(FGameplayTag SkillTag, AActor* Ball, FVector InputDirection, bool bMirrored) {
	for (UFootballSkillData* Skill : LoadedSkills) {
		if (Skill && Skill->SkillTag == SkillTag) {
			return RequestSkill(Skill, Ball, InputDirection, bMirrored);
		}
	}
	UE_LOG(LogFootballSkill, Log, TEXT("No skill tagged %s in %s"), *SkillTag.ToString(), *SkillDirectory.Path);
	return false;
}

void UFootballSkillComponent::CancelSkill() {
	if (!ActiveSkill) {
		return;
	}
	UAnimMontage* Playing = ActiveMontage;
	Finish(true);
	if (UAnimInstance* Anim = GetAnimInstance()) {
		Anim->Montage_Stop(Playing->BlendOut.GetBlendTime(), Playing); // its end event then finds no active skill
	}
}

void UFootballSkillComponent::OnMontageEnded(UAnimMontage* InMontage, bool bInterrupted, int32 InRequestId) {
	if (InRequestId == RequestId && ActiveSkill) {
		Finish(bInterrupted);
	}
}

void UFootballSkillComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!ActiveSkill) {
		if (GetWorld()->GetTimeSeconds() < RecordUntil) {
			RecordSample(LastMontageTime);
		} else {
			SetComponentTickEnabled(false);
		}
		return;
	}
	const FAnimMontageInstance* Instance = GetMontageInstance();
	if (!Instance) {
		Finish(true); // stopped from outside without an end event
		return;
	}
	// The instance keeps its position while blending out (Montage_GetPosition would read 0 then).
	const float T = Instance->GetPosition();
	UpdateTurn();
	ApplyTravel(T);
	if (const ACharacter* Character = Cast<ACharacter>(GetOwner())) {
		Character->GetCharacterMovement()->StopMovementImmediately();
	}
	UpdateBall(T, DeltaTime);
	UpdateSkillState();
	LastMontageTime = T;
	RecordSample(T);
	if (CVarFootballDebugContacts.GetValueOnGameThread() != 0) {
		DrawDebug(T);
	}
}

void UFootballSkillComponent::UpdateBall(float T, float DeltaTime) {
	LastEvaluated = INDEX_NONE;
	if (Windows.Num() == 0 || !BallMovement || !BallStateComp || !ActiveBall) {
		return;
	}
	// Taken away by someone or something else (a tackle, a kick, a reset): the move carries on without it.
	if (bOwned && (BallStateComp->GetState() != EBallState::SkillControlled || BallStateComp->GetBallOwner() != GetOwner())) {
		bOwned = false;
	}
	for (FWindow& Window : Windows) {
		if (Window.Phase == EWindowPhase::Pending && T >= Window.Start) {
			Window.Phase = EWindowPhase::Open;
		}
	}
	// One body region per tick: the first open window's, else the next one's (so its velocity is known when it opens).
	// A window opening in the same tick as the one before it closes is checked from the next tick on.
	int32 Tracked = Windows.IndexOfByPredicate([](const FWindow& W) { return W.Phase == EWindowPhase::Open; });
	if (Tracked == INDEX_NONE) {
		Tracked = Windows.IndexOfByPredicate([](const FWindow& W) { return W.Phase == EWindowPhase::Pending; });
	}
	if (Tracked != INDEX_NONE) {
		TrackContactRegion(Tracked, DeltaTime);
	}
	for (int32 i = 0; i < Windows.Num(); ++i) {
		FWindow& Window = Windows[i];
		if (Window.Phase == EWindowPhase::Open) {
			// A collision happens once; a carry keeps pushing every tick the body is on the ball.
			if (!Window.bTouched || IsCarry(ActiveSkill->ContactDescriptors[Window.ContactIndex].ContactType)) {
				EvaluateWindow(i, T, DeltaTime);
			}
			if (T >= Window.End) {
				CloseWindow(i);
				Window.Phase = EWindowPhase::Done;
			}
		}
	}
	if (bApproaching) {
		LegacyApproach(T, DeltaTime);
	}
}

void UFootballSkillComponent::TrackContactRegion(int32 Index, float DeltaTime) {
	const FBallContactDescriptor D = ActiveSkill->GetTunedDescriptor(Windows[Index].ContactIndex, bActiveMirrored);
	FVector Point, Normal;
	if (!Regions || !Regions->GetRegionPoint(D.BodyContactRegion, Point, Normal)) {
		TrackedWindow = INDEX_NONE;
		UBallContactResolver::StopTracking(Tracker);
		return;
	}
	// Same body region as last tick (also across back-to-back windows on one surface): its velocity carries on.
	UBallContactResolver::TrackRegion(Tracker, D.BodyContactRegion, Point, Normal, DeltaTime);
	TrackedWindow = Index;
}

void UFootballSkillComponent::EvaluateWindow(int32 Index, float T, float DeltaTime) {
	if (!bApproaching && !bOwned) {
		return; // the skill no longer has the ball (missed, taken away)
	}
	FWindow& Window = Windows[Index];
	LastEvaluated = Index;
	if (!Tracker.bTracking || TrackedWindow != Index) {
		Window.Result = FContactResult();
		Window.Result.Validation = EContactValidation::WrongRegion;
		return;
	}
	const FBallContactDescriptor D = ActiveSkill->GetTunedDescriptor(Window.ContactIndex, bActiveMirrored);
	const FVector Ball = ActiveBall->GetActorLocation();
	const double Radius = BallMovement->GetBallRadius();
	// Where the body region has to be for this touch (the resolver's T1 turned round), and the point of the region's
	// path this tick closest to it: a fast foot moves several cm a tick and must not skip past the ball.
	const FVector Needed = Ball + UBallContactResolver::RegionDirection(D.BallContactRegion, Basis) * Radius - Basis.TransformVector(D.LocalBallOffset);
	const FVector From = Tracker.Point - Tracker.Velocity * DeltaTime;
	const FVector Path = Tracker.Point - From;
	const double Along = Path.SizeSquared() > UE_KINDA_SMALL_NUMBER ? FMath::Clamp(FVector::DotProduct(Needed - From, Path) / Path.SizeSquared(), 0.0, 1.0) : 1.0;
	FContactQuery Query;
	Query.BallCenter = Ball;
	Query.BallVelocity = BallMovement->GetBallVelocity();
	Query.BallRadius = float(Radius);
	Query.BodyPoint = From + Path * Along;
	Query.BodyNormal = Tracker.Normal;
	Query.BodyVelocity = Tracker.Velocity;
	Query.PlayerVelocity = TrackVelocity(T);
	Query.Descriptor = D;
	Query.Basis = Basis;
	Query.bWindowOpen = true;
	Window.Result = UBallContactResolver::ResolveContact(Query);
	// The generic sweep of the region against the ball itself (spec §11), for fb.Debug.Contact; the frozen skill still
	// decides with the check above until Phase 6 switches it over.
	const FBodyRegionRow* Row = Regions->GetRegionRow(D.BodyContactRegion);
	UBallContactResolver::SweepRegionToBall(Tracker.Point, Tracker.Velocity, Row ? Row->Radius : 0.f, Ball, Query.BallVelocity, Query.BallRadius,
		DeltaTime, Window.Result.ClosestApproach, Window.Result.SweepAlpha, Window.Result.ContactPoint);
	if (Window.Result.Validation == EContactValidation::Valid) {
		Touch(Index, T);
	}
}

void UFootballSkillComponent::Touch(int32 Index, float T) {
	FWindow& Window = Windows[Index];
	const bool bFirstContact = !Window.bTouched;
	Window.bTouched = true;
	const FBallContactDescriptor D = ActiveSkill->GetTunedDescriptor(Window.ContactIndex, bActiveMirrored);
	const double Radius = BallMovement->GetBallRadius();
	const FVector BallVelocity = BallMovement->GetBallVelocity();
	FVector Physical, Needed, Applied, Spin;
	bool bLeaves = false;
	if (IsCarry(D.ContactType)) {
		// A carry: while the body pushes, the ball goes with it (no bounce, full grip). It separates by itself when the
		// body stops pushing (the check's T2 fails from then on), and the end of the window lets it go.
		Physical = Needed = Applied = LegacyCarryVelocity(Window, BallVelocity, Radius);
	} else {
		// One collision of the ball with the moving body region (doc §19), from the contact's real geometry...
		const FBodyRegionRow* Row = Regions->GetRegionRow(D.BodyContactRegion);
		const UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(ActiveBall->GetRootComponent());
		Physical = UBallContactResolver::ComputeDeflection(BallVelocity, Tracker.Velocity, Window.Result.ContactNormal,
			Row ? Row->Restitution : 0.5f, Row ? Row->TangentDamping : 0.2f, Body ? Body->GetPhysicsAngularVelocityInDegrees() : FVector::ZeroVector,
			Row ? Row->SpinTransfer : 0.f, float(Radius), Spin);
		// ...moved toward what the skill needs: the next touch's spot in time, or after the last one the authored exit (doc §20).
		bLeaves = !Windows.IsValidIndex(Index + 1);
		Applied = LegacyAim(Index, T, D, Physical, BallVelocity, bLeaves, Needed);
	}
	// Rolling spin, so the ball rolls off the touch instead of skidding (a skid would lose ~2/7 of the speed).
	const FVector Roll = FMath::RadiansToDegrees(FVector::CrossProduct(FVector::UpVector, FVector(Applied.X, Applied.Y, 0.0)) / FMath::Max(Radius, 1.0));
	if (BallContacts) {
		BallContacts->TryClaim(EContactPriority::Skill, GetOwner());
	}
	const EBallState NewState = bLeaves ? (BallMovement->IsGrounded() ? EBallState::Free : EBallState::Airborne) : EBallState::SkillControlled;
	BallMovement->ApplyTouch(Applied, Roll, NewState, bLeaves ? nullptr : GetOwner());
	bApproaching = false;
	bEverTouched = true;
	bOwned = !bLeaves;
	bReleased = bReleased || bLeaves;
	bTouchedThisTick = true;
	TouchPhysical = Physical;
	TouchNeeded = Needed;
	TouchApplied = Applied;
	LastTarget = bLeaves || IsCarry(D.ContactType) ? ActiveBall->GetActorLocation() + Applied * 0.25
		: LegacyTrajectoryTarget(ActiveSkill->GetTunedDescriptor(Windows[Index + 1].ContactIndex, bActiveMirrored).ContactTime);
	if (bFirstContact && CVarFootballDebugContacts.GetValueOnGameThread() != 0) {
		// What the collision gave (orange), what the skill needed (cyan) and what the ball got (green), for 1.5 s.
		const FVector From = ActiveBall->GetActorLocation();
		DrawDebugDirectionalArrow(GetWorld(), From, From + Physical * 0.2, 8.f, FColor::Orange, false, 1.5f);
		DrawDebugDirectionalArrow(GetWorld(), From, From + Needed * 0.2, 8.f, FColor::Cyan, false, 1.5f);
		DrawDebugDirectionalArrow(GetWorld(), From, From + Applied * 0.2, 8.f, FColor::Green, false, 1.5f, 0, 1.5f);
	}
}

void UFootballSkillComponent::CloseWindow(int32 Index) {
	const FWindow& Window = Windows[Index];
	if (Window.bTouched) {
		const EContactType Type = ActiveSkill->ContactDescriptors[Window.ContactIndex].ContactType;
		if (bOwned && IsCarry(Type) && (Type == EContactType::Release || Index == Windows.Num() - 1)) {
			// The carry ends: the ball leaves the skill with the velocity the body gave it.
			LetGo();
			bReleased = true;
			bTouchedThisTick = true;
			TouchPhysical = TouchNeeded = TouchApplied = BallMovement->GetBallVelocity();
		}
		return;
	}
	if (bApproaching && Index == 0) {
		// The first touch never happened (out of reach, blocked, deflected): the move carries on without the ball.
		bApproaching = false;
		BallMovement->EndDribble(GetOwner());
		UE_LOG(LogFootballSkill, Log, TEXT("%s: touch %d missed (%s, %.1f cm), carrying on without the ball"), *ActiveSkill->GetName(),
			Window.ContactIndex, *UEnum::GetValueAsString(Window.Result.Validation), Window.Result.Distance);
	} else if (bOwned && Index == Windows.Num() - 1) {
		UE_LOG(LogFootballSkill, Log, TEXT("%s: last touch missed (%s, %.1f cm), the ball rolls on"), *ActiveSkill->GetName(),
			*UEnum::GetValueAsString(Window.Result.Validation), Window.Result.Distance);
		LetGo();
	}
}

void UFootballSkillComponent::LetGo() {
	bOwned = false;
	if (BallStateComp && BallStateComp->GetState() == EBallState::SkillControlled && BallStateComp->GetBallOwner() == GetOwner()) {
		BallStateComp->SetState(BallMovement->IsGrounded() ? EBallState::Free : EBallState::Airborne, nullptr);
	}
}

void UFootballSkillComponent::UpdateSkillState() {
	// Between touches the ball is on its way to the next one: Redirecting (doc §11, §18).
	ESkillState New = bEverTouched ? ESkillState::Redirecting : ESkillState::Starting;
	const FWindow* Open = Windows.FindByPredicate([this](const FWindow& W) {
		return W.Phase == EWindowPhase::Open && (!W.bTouched || IsCarry(ActiveSkill->ContactDescriptors[W.ContactIndex].ContactType));
	});
	if (bReleased || (Windows.Num() > 0 && Windows.Last().Phase == EWindowPhase::Done)) {
		New = ESkillState::Recovering;
	} else if (Open) {
		switch (ActiveSkill->ContactDescriptors[Open->ContactIndex].ContactType) {
		case EContactType::Redirect:
			New = ESkillState::Redirecting;
			break;
		case EContactType::Release:
		case EContactType::Strike:
		case EContactType::Pass:
		case EContactType::Shot:
		case EContactType::Header:
			New = ESkillState::Releasing;
			break;
		default:
			New = ESkillState::Contacting;
			break;
		}
	}
	SetSkillState(New, ActiveSkill);
}

void UFootballSkillComponent::SetSkillState(ESkillState NewState, UFootballSkillData* Skill) {
	if (NewState != SkillState) {
		SkillState = NewState;
		OnSkillStateChanged.Broadcast(Skill, NewState);
	}
}

void UFootballSkillComponent::Finish(bool bCancelled) {
	if (!ActiveSkill) {
		return;
	}
	UFootballSkillData* Ended = ActiveSkill;
	// Windows still open close now (a missed first touch leaves the ball, a missed last one lets it go).
	for (int32 i = 0; i < Windows.Num(); ++i) {
		if (Windows[i].Phase == EWindowPhase::Open) {
			CloseWindow(i);
			Windows[i].Phase = EWindowPhase::Done;
		}
	}
	if (bOwned) {
		LetGo();
	}
	if (bApproaching) {
		BallMovement->EndDribble(GetOwner());
	}
	bApproaching = false;
	if (AController* Controller = IgnoredController.Get()) {
		Controller->SetIgnoreMoveInput(false);
	}
	IgnoredController = nullptr;
	SetSkillState(bCancelled ? ESkillState::Cancelled : ESkillState::Completed, Ended);
	RecordSample(LastMontageTime);
	ActiveSkill = nullptr;
	ActiveMontage = nullptr;
	Windows.Reset();
	MontageInstanceId = INDEX_NONE;
	RecordUntil = GetWorld()->GetTimeSeconds() + RecordTail;
	UE_LOG(LogFootballSkill, Log, TEXT("%s %s"), *Ended->GetName(), bCancelled ? TEXT("cancelled") : TEXT("completed"));
	OnSkillEnded.Broadcast(Ended, bCancelled);
	SetSkillState(ESkillState::None, Ended);
}

void UFootballSkillComponent::RecordSample(float T) {
	FSkillDebugSample& Sample = Record.AddDefaulted_GetRef();
	Sample.Time = float(GetWorld()->GetTimeSeconds() - StartTime);
	Sample.MontageTime = T;
	Sample.SkillState = SkillState;
	Sample.bBallDriven = bApproaching || bOwned;
	if (BallStateComp) {
		Sample.BallState = BallStateComp->GetState();
	}
	if (ActiveBall) {
		Sample.BallPos = ActiveBall->GetActorLocation();
	}
	if (BallMovement) {
		Sample.BallVel = BallMovement->GetBallVelocity();
	}
	if (Windows.IsValidIndex(LastEvaluated)) {
		const FWindow& Window = Windows[LastEvaluated];
		Sample.ContactIndex = Window.ContactIndex;
		Sample.Validation = Window.Result.Validation;
		Sample.ContactDistance = Window.Result.Distance;
		Sample.BodyPoint = Window.Result.BodyPoint;
		Sample.SurfacePoint = Window.Result.SurfacePoint;
	}
	Sample.TargetPos = LastTarget;
	if (bTouchedThisTick) {
		Sample.TouchPhysical = TouchPhysical;
		Sample.TouchNeeded = TouchNeeded;
		Sample.TouchApplied = TouchApplied;
		bTouchedThisTick = false;
	}
	Sample.ActorPos = GetOwner()->GetActorLocation();
	Sample.ActorYaw = float(GetOwner()->GetActorRotation().Yaw);
	if (const USkeletalMeshComponent* Mesh = GetMesh()) {
		Sample.HipsPos = Mesh->GetBoneLocation(TEXT("Hips"));
		Sample.LeftFootPos = Mesh->GetBoneLocation(TEXT("LeftFoot"));
		Sample.RightFootPos = Mesh->GetBoneLocation(TEXT("RightFoot"));
	}
}

void UFootballSkillComponent::DrawDebug(float T) const {
	// Doc §33: skill direction, ball centre, ball velocity, ball target, player contact point, ball surface point,
	// contact normal, contact radius, current skill, skill state, ball state, contact time.
	const UWorld* World = GetWorld();
	const FVector Root = GroundRoot() + FVector(0.0, 0.0, 2.0);
	DrawDebugDirectionalArrow(World, Root, Root + Basis.Forward * 80.0, 20.f, FColor::Blue);
	DrawDebugDirectionalArrow(World, Root, Root + Basis.TransformVector(FVector(0.0, 40.0, 0.0)), 12.f, FColor::Cyan);
	if (ActiveBall && BallMovement) {
		const FVector Center = ActiveBall->GetActorLocation();
		const float Radius = BallMovement->GetBallRadius();
		DrawDebugSphere(World, Center, Radius, 12, FColor::White);
		DrawDebugDirectionalArrow(World, Center, Center + BallMovement->GetBallVelocity() * 0.1, 8.f, FColor::Yellow);
		if (bApproaching || bOwned) {
			DrawDebugSphere(World, LastTarget, Radius, 12, FColor::Green);
		}
	}
	FString ContactText;
	if (Windows.IsValidIndex(LastEvaluated) && Regions) {
		const FWindow& Window = Windows[LastEvaluated];
		const FContactResult& Result = Window.Result;
		const FBallContactDescriptor D = ActiveSkill->GetTunedDescriptor(Window.ContactIndex, bActiveMirrored);
		DrawDebugSphere(World, Result.BodyPoint, 2.f, 8, FColor::Red);
		DrawDebugDirectionalArrow(World, Result.BodyPoint, Result.BodyPoint + Result.ContactNormal * 20.0, 6.f, FColor::Blue);
		DrawDebugPoint(World, Result.SurfacePoint, 8.f, FColor::Magenta);
		DrawDebugSphere(World, Result.TargetBallCenter, D.ContactTolerance, 8, Result.Validation == EContactValidation::Valid ? FColor::Green : FColor::Orange);
		if (const FBodyRegionRow* Row = Regions->GetRegionRow(D.BodyContactRegion)) {
			const FQuat Facing = Tracker.Normal.ToOrientationQuat();
			DrawDebugCircle(World, Result.BodyPoint, Row->Radius, 16, FColor::Red, false, -1.f, 0, 0.f, Facing.GetAxisY(), Facing.GetAxisZ(), false);
		}
		// Spec §103: the region's previous point and its path this tick (yellow), and where the sweep meets the ball (cyan).
		DrawDebugPoint(World, Tracker.PrevPoint, 6.f, FColor::Yellow);
		DrawDebugLine(World, Tracker.PrevPoint, Tracker.Point, FColor::Yellow, false, -1.f, 0, 0.5f);
		if (Result.ClosestApproach <= 0.f) {
			DrawDebugPoint(World, Result.ContactPoint, 10.f, FColor::Cyan);
		}
		ContactText = FString::Printf(TEXT("  contact %d%s%s %s %s %.1f cm, gap %.1f cm"), Window.ContactIndex, Window.Label.IsNone() ? TEXT("") : TEXT(" "),
			Window.Label.IsNone() ? TEXT("") : *Window.Label.ToString(), *UEnum::GetDisplayValueAsText(D.BodyContactRegion).ToString(),
			*UEnum::GetDisplayValueAsText(Result.Validation).ToString(), Result.Distance, Result.ClosestApproach);
	}
	const FString Text = FString::Printf(TEXT("%s  %s  ball %s  t=%.2f%s"), *ActiveSkill->SkillTag.ToString(), *UEnum::GetDisplayValueAsText(SkillState).ToString(),
		BallStateComp ? *UEnum::GetDisplayValueAsText(BallStateComp->GetState()).ToString() : TEXT("-"), T, *ContactText);
	DrawDebugString(World, GetOwner()->GetActorLocation() + FVector(0.0, 0.0, 120.0), Text, nullptr, FColor::White, 0.f);
}


// ---- LEGACY ball-trajectory authority: frozen in master plan Phase 1 (2026-10-06), deleted in Phase 6 ----------------
// The master spec forbids it (§15, §138, §171): see the header. Bodies unchanged from P3; Phase 6 replaces them with
// player alignment, a raw physical impulse and bounded assist.

FVector UFootballSkillComponent::LegacyTrajectoryTarget(float T) const {
	// PrevTrack is the baked travel so far (ApplyTravel has run this tick). Unblocked, GroundRoot = start + that travel, so
	// the two cancel exactly and the target is the authored path, whatever either curve's interpolation.
	const FVector Path = ActiveSkill->TrajectoryProfile->Evaluate(T / MontageLength);
	return GroundRoot() + Basis.TransformVector(FVector(Path.X - PrevTrack.X, Path.Y - PrevTrack.Y, Path.Z));
}

FVector UFootballSkillComponent::LegacyNeededVelocity(int32 NextIndex, float T) const {
	// A freely rolling ball covers x(t) = v0 (1 - e^(-k t)) / k, so to cover d in t: v0 = k d / (1 - e^(-k t)).
	const FBallContactDescriptor Next = ActiveSkill->GetTunedDescriptor(Windows[NextIndex].ContactIndex, bActiveMirrored);
	const FVector Spot = LegacyTrajectoryTarget(Next.ContactTime);
	const FVector Ball = ActiveBall->GetActorLocation();
	const FVector Delta(Spot.X - Ball.X, Spot.Y - Ball.Y, 0.0);
	const double Time = FMath::Max((Next.ContactTime - T) / GetPlayRate(), 0.05);
	const double K = FMath::Max(double(BallRollDecay), 0.01);
	return Delta * (K / (1.0 - FMath::Exp(-K * Time)));
}

FVector UFootballSkillComponent::LegacyAim(int32 Index, float T, const FBallContactDescriptor& D, const FVector& Physical, const FVector& BallVelocity,
	bool bLeaves, FVector& OutNeeded) const {
	OutNeeded = bLeaves ? UBallContactResolver::ComputeExitVelocity(D, Basis, TrackVelocity(T), BallVelocity) : LegacyNeededVelocity(Index + 1, T);
	return Physical + (OutNeeded - Physical) * FMath::Clamp(D.AssistStrength, 0.f, 1.f);
}

FVector UFootballSkillComponent::LegacyCarryVelocity(const FWindow& Window, const FVector& BallVelocity, double Radius) const {
	FVector Spin;
	return UBallContactResolver::ComputeDeflection(BallVelocity, Tracker.Velocity, Window.Result.ContactNormal, 0.f, 1.f,
		FVector::ZeroVector, 0.f, float(Radius), Spin);
}

void UFootballSkillComponent::LegacyApproach(float T, float DeltaTime) {
	// Approach (doc §18): a physics ball, steered to come to rest on the first touch's spot when its window opens:
	// time-to-go control from its own motion (a = 6 x / t^2 - 4 v / t: arrive at x with zero speed in t), so it
	// decelerates onto the spot instead of overshooting it; from MinApproachTime before the window, held there.
	const FWindow& First = Windows[0];
	LastTarget = LegacyTrajectoryTarget(ActiveSkill->GetTunedDescriptor(First.ContactIndex, bActiveMirrored).ContactTime);
	if (BallContacts && !BallContacts->TryClaim(EContactPriority::Skill, GetOwner())) {
		return;
	}
	const FVector Ball = ActiveBall->GetActorLocation();
	const FVector Velocity = BallMovement->GetBallVelocity();
	const FVector ToSpot(LastTarget.X - Ball.X, LastTarget.Y - Ball.Y, 0.0);
	const FVector Flat(Velocity.X, Velocity.Y, 0.0);
	const double TimeToGo = (First.Start - T) / GetPlayRate();
	const FVector Desired = TimeToGo > MinApproachTime
		? Flat + (ToSpot * (6.0 / (TimeToGo * TimeToGo)) - Flat * (4.0 / TimeToGo)) * DeltaTime
		: ToSpot / FMath::Max(double(MinApproachTime), 0.01);
	// ApplyDribble with no spring and full blend sets exactly this velocity (capped, keeping the ball's own vertical motion).
	BallMovement->ApplyDribble(GetOwner(), LastTarget, Desired, 0.f, ActiveSkill->MaxApproachSpeed, 1.f);
}
