#include "FootballSkillComponent.h"
#include "AnimNotifyState_BallContactWindow.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BallAssist.h"
#include "BallContactResolver.h"
#include "BallInteractionComponent.h"
#include "BallMovementComponent.h"
#include "BallPhysicsResponse.h"
#include "BallStateComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "FootballActionComponent.h"
#include "FootballSkillData.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "PlayerAttributesData.h"
#include "PlayerBallInteractionComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogFootballSkill, Log, All);

// Spec §122. Carries: the body stays on the ball and pushes it along (a controlled drag, a trap, a capture) every tick it
// presses; the ball separates by itself when the body stops pushing. The other modes are one collision.
static bool IsCarry(EBallResponseMode Mode) {
	return Mode == EBallResponseMode::ControlledContact || Mode == EBallResponseMode::EnergyAbsorption || Mode == EBallResponseMode::Capture;
}

static float Smooth01(float X) {
	return FMath::SmoothStep(0.f, 1.f, FMath::Clamp(X, 0.f, 1.f));
}

// Upward speed (cm/s) below which a touched grounded ball stays on the ground and rolls: a hop of under ~1.3 cm.
// one fixed bound; make it per contact when a skill flicks the ball up on purpose.
static constexpr double MaxRollingLift = 50.0;

UFootballSkillComponent::UFootballSkillComponent() {
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false; // ticks only while a skill plays, and for the recorder's tail
	// After physics (master plan 6.1/6.4): the ball is where this frame's step left it, the same moment as the capsule and
	// the pose the skill moves and reads, so the swept touch and the predictions share one clock (before physics the ball
	// was a frame old: the foot met it one frame early, more at 30 fps). A touch lands in the next step, which starts there.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
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

void UFootballSkillComponent::ApplyAlignment(float T) {
	if (!bWarping) {
		return;
	}
	const float Span = Alignment.WindowEnd - Alignment.WindowStart;
	const float X = Span > UE_KINDA_SMALL_NUMBER ? FMath::Clamp((T - Alignment.WindowStart) / Span, 0.f, 1.f) : 1.f;
	const float Alpha = FMath::SmoothStep(0.f, 1.f, X);
	const FVector Step = Alignment.PositionCorrection * double(Alpha - WarpAlpha);
	WarpAlpha = Alpha;
	if (!Step.IsNearlyZero()) {
		FHitResult Hit;
		const FVector Before = GetOwner()->GetActorLocation();
		GetOwner()->AddActorWorldOffset(Step, true, &Hit);
		AlignmentApplied += GetOwner()->GetActorLocation() - Before;
	}
	bWarping = Alpha < 1.f;
}

void UFootballSkillComponent::Realign(int32 NextIndex, float T) {
	const FWindow& Next = Windows[NextIndex];
	const FBallContactDescriptor D = ActiveSkill->GetTunedDescriptor(Next.ContactIndex, bActiveMirrored);
	if (D.ExpectedBallOffset.IsNearlyZero() || D.ContactTime <= T) {
		return;
	}
	const float Rate = GetPlayRate();
	// What is left of the current warp still moves the player; the new one adds what the next contact needs on top.
	const FVector Remaining = bWarping ? Alignment.PositionCorrection * double(1.f - WarpAlpha) : FVector::ZeroVector;
	FAlignmentQuery Query;
	FVector BallVelocity;
	Query.PredictedBall = BallMovement->PredictFreeBall((D.ContactTime - T) / Rate, BallVelocity);
	Query.Pivot = GroundRoot();
	const FVector2D Travel = SampleTrack(D.ContactTime) - PrevTrack;
	Query.PredictedRoot = Query.Pivot + Basis.TransformVector(FVector(Travel.X, Travel.Y, 0.0)) + Remaining;
	Query.EntryOffset = EntryDisplacement(T, D.ContactTime);
	Query.Basis = Basis;
	Query.ExpectedBallOffset = D.ExpectedBallOffset;
	Query.Tolerance = D.ContactTolerance;
	Query.TimeAvailable = (D.ContactTime - T) / Rate;
	Query.WindowStart = T;
	Query.WindowEnd = D.ContactTime;
	Query.ContactIndex = Next.ContactIndex;
	Query.bAllowRotation = false;
	Query.Profile = ActiveSkill->AlignmentProfile;
	Alignment = UFootballActionComponent::CalculateAlignmentTarget(Query);
	Alignment.PositionCorrection += Remaining;
	WarpAlpha = 0.f;
	bWarping = !Alignment.PositionCorrection.IsNearlyZero();
	if (!Alignment.bReachable) {
		UE_LOG(LogFootballSkill, Log, TEXT("%s: re-aligning for contact %d leaves the ball %.1f cm off (%.1f needed)"), *ActiveSkill->GetName(),
			Next.ContactIndex, Alignment.Residual, Alignment.RequiredCorrection);
	}
}

FVector UFootballSkillComponent::EntryDisplacement(float FromT, float ToT) const {
	if (!bEntryMomentum || ToT <= FromT) {
		return FVector::ZeroVector;
	}
	const double Rate = GetPlayRate();
	const double Hold = EntryHoldTime / Rate;
	const double Tau = FMath::Max(double(EntryDecayTime), 0.01);
	// Seconds of full-speed travel by montage time T: all of it until the hold ends, then fading.
	const auto FullSpeedSeconds = [&](float T) {
		const double S = T / Rate;
		return S <= Hold ? S : Hold + Tau * (1.0 - FMath::Exp(-(S - Hold) / Tau));
	};
	return EntryVelocity * (FullSpeedSeconds(ToT) - FullSpeedSeconds(FromT));
}

void UFootballSkillComponent::ApplyEntryMomentum(float T) {
	const FVector Step = EntryDisplacement(LastMontageTime, T);
	if (!Step.IsNearlyZero()) {
		GetOwner()->AddActorWorldOffset(Step, true);
	}
}

FVector UFootballSkillComponent::PredictRootAt(float ContactTime, float T) const {
	const FVector2D Travel = SampleTrack(ContactTime) - PrevTrack;
	FVector Root = GroundRoot() + Basis.TransformVector(FVector(Travel.X, Travel.Y, 0.0));
	if (bWarping) {
		const float Span = Alignment.WindowEnd - Alignment.WindowStart;
		const float By = Span > UE_KINDA_SMALL_NUMBER ? Smooth01((ContactTime - Alignment.WindowStart) / Span) : 1.f;
		Root += Alignment.PositionCorrection * double(FMath::Max(By - WarpAlpha, 0.f));
	}
	return Root;
}

FVector UFootballSkillComponent::PredictContactSpot(int32 Index, float T) const {
	const FBallContactDescriptor D = ActiveSkill->GetTunedDescriptor(Windows[Index].ContactIndex, bActiveMirrored);
	return PredictRootAt(D.ContactTime, T) + EntryDisplacement(T, D.ContactTime) + Basis.TransformVector(D.ExpectedBallOffset);
}

FVector UFootballSkillComponent::IntentToward(int32 NextIndex, float T, const FVector& From) const {
	const FBallContactDescriptor D = ActiveSkill->GetTunedDescriptor(Windows[NextIndex].ContactIndex, bActiveMirrored);
	const FVector Spot = PredictContactSpot(NextIndex, T);
	const FVector Chord(Spot.X - From.X, Spot.Y - From.Y, 0.0);
	const float Time = FMath::Max((D.ContactTime - T) / GetPlayRate(), 0.05f);
	float Decel = 0.f, DragK = 0.f;
	BallMovement->GetRollModel(Decel, DragK);
	return Chord.GetSafeNormal() * double(UBallMovementComponent::SpeedToRoll(float(Chord.Size()), Time, Decel, DragK));
}

FBallAssistInput UFootballSkillComponent::ScaleAssistForPlayer(const FBallAssistInput& Input) const {
	if (!ActiveSkill->AssistProfile.bScaleWithPlayerSkill) {
		return Input;
	}
	const UFootballActionComponent* Actions = GetOwner()->FindComponentByClass<UFootballActionComponent>();
	const UPlayerAttributesData* Attributes = Actions ? Actions->GetAttributes() : nullptr;
	if (!Attributes) {
		return Input;
	}
	const float Skill = UBallAssist::GetAttribute(Attributes->Attributes, ActiveSkill->AttributeProfile.AssistAttribute);
	return Skill >= 0.f ? UBallAssist::ScaleForSkill(Input, Skill, Attributes->SkillToAssistScale) : Input;
}

FVector UFootballSkillComponent::GroundRoot() const {
	const FVector Location = GetOwner()->GetActorLocation();
	return FVector(Location.X, Location.Y, BallMovement ? BallMovement->GetGroundZ() : Location.Z);
}

FVector UFootballSkillComponent::GroundRootAt(float At, float Now) const {
	const FVector2D Travel = SampleTrack(Now) - SampleTrack(At);
	FVector Since = Basis.TransformVector(FVector(Travel.X, Travel.Y, 0.0)) + EntryDisplacement(At, Now);
	const float Span = Alignment.WindowEnd - Alignment.WindowStart;
	if (Span > UE_KINDA_SMALL_NUMBER) {
		Since += Alignment.PositionCorrection * double(Smooth01((Now - Alignment.WindowStart) / Span) - Smooth01((At - Alignment.WindowStart) / Span));
	}
	return GroundRoot() - Since;
}

bool UFootballSkillComponent::BakedRegionAt(const FBallContactDescriptor& D, float At, float Now, FVector& OutPoint, FVector& OutNormal) const {
	const int32 N = D.RegionPath.Num();
	if (N < 2 || D.RegionPathNormal.Num() != N || D.RegionPathStep <= 0.f) {
		return false;
	}
	const float K = FMath::Clamp((At - D.RegionPathStart) / D.RegionPathStep, 0.f, float(N - 1));
	const int32 I = FMath::Min(FMath::FloorToInt(K), N - 2);
	const float A = K - float(I);
	OutPoint = GroundRootAt(At, Now) + Basis.TransformVector(FMath::Lerp(D.RegionPath[I], D.RegionPath[I + 1], A));
	OutNormal = Basis.TransformVector(FMath::Lerp(D.RegionPathNormal[I], D.RegionPathNormal[I + 1], A)).GetSafeNormal();
	return true;
}

bool UFootballSkillComponent::RequestSkill(UFootballSkillData* Skill, AActor* Ball, FVector InputDirection, bool bMirrored) {
	UFootballActionComponent* Actions = GetOwner()->FindComponentByClass<UFootballActionComponent>();
	if (!Actions) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s refused: the player needs a FootballAction component (master plan Phase 5)"), *GetNameSafe(Skill));
		return false;
	}
	FActionContext Context;
	Context.BallActor = Ball;
	Context.InputDirection = InputDirection;
	Context.bMirrored = bMirrored;
	return Actions->RequestActionData(Skill, Context);
}

bool UFootballSkillComponent::PrepareSkill(const UFootballSkillData* Skill, const FActionContext& Context, const FSkillBasis& InBasis,
	FActionAlignmentTarget& OutAlignment, bool& bOutReachable, FVector& OutTravel) const {
	OutAlignment = FActionAlignmentTarget();
	bOutReachable = true;
	OutTravel = FVector::ZeroVector;
	const FString SkillName = GetNameSafe(Skill);
	UAnimInstance* Anim = GetAnimInstance();
	if (!Skill || !Cast<ACharacter>(GetOwner()) || !Anim) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s can't play: no skill, or the owner is not a Character with an AnimBP"), *SkillName);
		return false;
	}
	const UAnimMontage* NewMontage = Skill->GetMontage(Context.bMirrored);
	if (!NewMontage || NewMontage->SlotAnimTracks.Num() == 0) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s can't play: no %s montage"), *SkillName, Context.bMirrored ? TEXT("mirrored") : TEXT("right-foot"));
		return false;
	}
	const FName Slot = NewMontage->SlotAnimTracks[0].SlotName;
	if (Anim->GetSlotNodeGlobalWeight(Slot) <= 0.f) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s can't play: the AnimBP %s has no active slot %s"), *SkillName, *Anim->GetClass()->GetName(), *Slot.ToString());
		return false;
	}
	const FActionAlignmentProfile& Profile = Skill->AlignmentProfile;
	const float Rate = FMath::Max(Skill->PlayRate * NewMontage->RateScale, UE_KINDA_SMALL_NUMBER);
	const FVector EntryVelocity2D = Profile.bCarryEntryVelocity ? FVector(Context.PlayerVelocity.X, Context.PlayerVelocity.Y, 0.0) : FVector::ZeroVector;
	const FVector2D Track0 = SampleTrackOf(*Skill, *NewMontage, 0.f);
	const FVector2D TrackEnd = SampleTrackOf(*Skill, *NewMontage, NewMontage->GetPlayLength()) - Track0;
	const TArray<FWindow> NewWindows = CollectWindows(*Skill, *NewMontage);
	const UBallMovementComponent* Movement = Context.BallActor ? Context.BallActor->FindComponentByClass<UBallMovementComponent>() : nullptr;
	const FBallContactDescriptor D = NewWindows.Num() > 0 ? Skill->GetTunedDescriptor(NewWindows[0].ContactIndex, Context.bMirrored) : FBallContactDescriptor();
	// The whole move for the collision-space check: the travel, the entry momentum (held to the first contact, then its fade).
	const float EntryHold = NewWindows.Num() > 0 ? D.ContactTime / Rate : 0.f;
	OutTravel = InBasis.TransformVector(FVector(TrackEnd.X, TrackEnd.Y, 0.0)) + EntryVelocity2D * double(EntryHold + Profile.EntryDecayTime);
	if (NewWindows.Num() == 0 || !Movement) {
		return true; // no ball work, or no usable ball (the action layer's ValidateBall says so)
	}
	if (!Regions) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s can't play: it needs PlayerBallInteraction on the player"), *SkillName);
		return false;
	}
	if (D.ExpectedBallOffset.IsNearlyZero()) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s: contact %d has no ExpectedBallOffset (re-run the bake) - it can't be reached"), *SkillName, NewWindows[0].ContactIndex);
		bOutReachable = false;
		return true;
	}
	// Spec §72-73: where the free ball will be at the planned touch, and where the expected spot would be by then.
	FAlignmentQuery Query;
	const float WarpStart = FMath::Min(Profile.WarpStartTime, D.ContactTime);
	FVector BallVelocity;
	Query.PredictedBall = Movement->PredictFreeBall(D.ContactTime / Rate, BallVelocity);
	Query.Pivot = FVector(Context.PlayerLocation.X, Context.PlayerLocation.Y, Movement->GetGroundZ());
	const FVector2D Travel = SampleTrackOf(*Skill, *NewMontage, D.ContactTime) - Track0;
	Query.PredictedRoot = Query.Pivot + InBasis.TransformVector(FVector(Travel.X, Travel.Y, 0.0));
	Query.EntryOffset = EntryVelocity2D * double(EntryHold);
	Query.Basis = InBasis;
	Query.ExpectedBallOffset = D.ExpectedBallOffset;
	Query.Tolerance = D.ContactTolerance;
	Query.TimeAvailable = (D.ContactTime - WarpStart) / Rate;
	Query.WindowStart = WarpStart;
	Query.WindowEnd = D.ContactTime;
	Query.ContactIndex = NewWindows[0].ContactIndex;
	Query.bAllowRotation = true;
	Query.Profile = Profile;
	if (!Profile.bAlignPlayer) {
		// No player correction: the action only starts when the ball is already where its first contact expects it.
		Query.Profile.MaxPositionCorrection = 0.f;
		Query.Profile.MaxRotationCorrection = 0.f;
	}
	OutAlignment = UFootballActionComponent::CalculateAlignmentTarget(Query);
	OutAlignment.TimeToContact = D.ContactTime / Rate;
	OutTravel += OutAlignment.PositionCorrection;
	bOutReachable = OutAlignment.bReachable;
	return true;
}

bool UFootballSkillComponent::StartSkill(UFootballSkillData* Skill, const FActionContext& Context, const FSkillBasis& NewBasis, ESkillDirection Direction,
	const FActionAlignmentTarget& InAlignment) {
	ACharacter* Character = Cast<ACharacter>(GetOwner());
	UAnimInstance* Anim = GetAnimInstance();
	UAnimMontage* NewMontage = Skill ? Skill->GetMontage(Context.bMirrored) : nullptr;
	if (ActiveSkill || !Skill || !Character || !Anim || !NewMontage) {
		return false;
	}
	const FString SkillName = Skill->GetName();
	const bool bMirrored = Context.bMirrored;
	AActor* Ball = Context.BallActor;
	TArray<FWindow> NewWindows = CollectWindows(*Skill, *NewMontage);
	UBallMovementComponent* Movement = Ball ? Ball->FindComponentByClass<UBallMovementComponent>() : nullptr;
	UBallStateComponent* State = Ball ? Ball->FindComponentByClass<UBallStateComponent>() : nullptr;

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
	// The player is moved to the ball (spec §15); the ball is left to physics until the first touch, and stays the
	// dribbler's (Controlled) until then.
	bWantsBall = Windows.Num() > 0;
	bAlign = bWantsBall && Skill->AlignmentProfile.bAlignPlayer;
	Alignment = InAlignment;
	bWarping = bAlign && !Alignment.PositionCorrection.IsNearlyZero();
	WarpAlpha = 0.f;
	AlignmentApplied = FVector::ZeroVector;
	bEntryMomentum = Skill->AlignmentProfile.bCarryEntryVelocity;
	EntryVelocity = FVector(Context.PlayerVelocity.X, Context.PlayerVelocity.Y, 0.0);
	EntryHoldTime = bWantsBall ? Skill->GetTunedDescriptor(Windows[0].ContactIndex, bMirrored).ContactTime : 0.f;
	EntryDecayTime = Skill->AlignmentProfile.EntryDecayTime;
	bBallTakenAway = false;
	HoldBallUntil = 0.0;
	bOwned = false;
	bEverTouched = false;
	bReleased = false;
	LastMontageTime = 0.f;
	PrevTrack = SampleTrack(0.f);
	TrackedWindow = INDEX_NONE;
	UBallContactResolver::StopTracking(Tracker);
	LastEvaluated = INDEX_NONE;
	bTouchedThisTick = false;
	LastTarget = Alignment.ContactIndex != INDEX_NONE ? Alignment.PredictedBall : (Ball ? Ball->GetActorLocation() : FVector::ZeroVector);
	StartTime = GetWorld()->GetTimeSeconds();
	TurnFrom = Character->GetActorQuat();
	TurnTo = Basis.Forward.ToOrientationQuat();
	TurnDuration = NewMontage->BlendIn.GetBlendTime();
	bTurning = true;
	Character->GetCharacterMovement()->StopMovementImmediately();
	// The input already queued this frame would still move the capsule once, by a frame-length-dependent amount (master plan
	// 6.7: 1.5 cm more at 30 fps than at 60, which moved the elastico's touches).
	Character->ConsumeMovementInputVector();
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
	UFootballActionComponent* Actions = GetOwner()->FindComponentByClass<UFootballActionComponent>();
	if (!Actions) {
		UE_LOG(LogFootballSkill, Warning, TEXT("%s refused: the player needs a FootballAction component (master plan Phase 5)"), *SkillTag.ToString());
		return false;
	}
	return Actions->RequestActionWithInput(SkillTag, Ball, InputDirection, bMirrored);
}

bool UFootballSkillComponent::IsSkillUsingBall() const {
	return (ActiveSkill != nullptr && Windows.Num() > 0) || (HoldBallUntil > 0.0 && GetWorld() && GetWorld()->GetTimeSeconds() < HoldBallUntil);
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
	ApplyAlignment(T);
	ApplyEntryMomentum(T);
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
	// Taken away by someone or something else (a tackle, a kick, a reset): the action layer ends the move (spec §65 BallLost)
	// unless the action plays on without it.
	if (bOwned && (BallStateComp->GetState() != EBallState::SkillControlled || BallStateComp->GetBallOwner() != GetOwner())) {
		bOwned = false;
		bBallTakenAway = true;
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
			if (!Window.bTouched || IsCarry(ActiveSkill->ContactDescriptors[Window.ContactIndex].ResponseMode)) {
				EvaluateWindow(i, T, DeltaTime);
			}
			if (T >= Window.End) {
				CloseWindow(i);
				Window.Phase = EWindowPhase::Done;
			}
		}
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
	if (!bWantsBall && !bOwned) {
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
	const FVector BallVelocity = BallMovement->GetBallVelocity();
	// Spec §11 (master plan 2.3 / 6.4): the region's path this tick swept against the ball decides the touch - the two
	// really meet, no tolerance (§164). The resolver's other tests still apply at the moment they meet: the ball on the
	// side the region faces (T4), the body closing on it (T2) and the body's minimum speed (T5).
	const FBodyRegionRow* Row = Regions->GetRegionRow(D.BodyContactRegion);
	FContactQuery Query;
	Query.BallVelocity = BallVelocity;
	Query.BallRadius = float(Radius);
	Query.PlayerVelocity = TrackVelocity(T);
	Query.Descriptor = D;
	Query.Descriptor.ContactTolerance = UE_BIG_NUMBER; // the sweep is the distance test
	Query.Basis = Basis;
	Query.bWindowOpen = true;
	// One moment of the sweep: the resolver at the region and the ball then (Gap > 0 = they don't meet). True = touched.
	const auto Candidate = [&](const FVector& BodyPoint, const FVector& BodyNormal, const FVector& BodyVelocity, const FVector& BallAt, float Gap,
		float Alpha) {
		Query.BallCenter = BallAt;
		Query.BodyPoint = BodyPoint;
		Query.BodyNormal = BodyNormal;
		Query.BodyVelocity = BodyVelocity;
		Window.Result = UBallContactResolver::ResolveContact(Query);
		Window.Result.ClosestApproach = Gap;
		Window.Result.SweepAlpha = Alpha;
		Window.Result.ContactPoint = BallAt + (BodyPoint - BallAt).GetSafeNormal() * Radius;
		if (Gap > 0.f && Window.Result.Validation == EContactValidation::Valid) {
			Window.Result.Validation = EContactValidation::TooFar;
		}
		if (Window.Result.Validation != EContactValidation::Valid) {
			return false;
		}
		const bool bWasTouched = Window.bTouched;
		Touch(Index, T, BallAt, BodyVelocity);
		return Window.bTouched && (!bWasTouched || IsCarry(D.ResponseMode)); // Touch still refuses a ball not closing on the body
	};
	FVector P, N;
	if (!BakedRegionAt(D, T, T, P, N)) {
		// No baked path: the live region, swept along its chord this tick.
		float Gap = UE_BIG_NUMBER, Alpha = 1.f;
		FVector ContactPoint;
		UBallContactResolver::SweepRegionToBall(Tracker.Point, Tracker.Velocity, Row ? Row->Radius : 0.f, Ball, BallVelocity, float(Radius), DeltaTime,
			Gap, Alpha, ContactPoint);
		const double Back = double(DeltaTime) * (1.0 - Alpha); // to where the touch starts
		Candidate(Tracker.Point - Tracker.Velocity * Back, Tracker.Normal, Tracker.Velocity, Ball - BallVelocity * Back, Gap, Alpha);
		return;
	}
	// Master plan 6.7: the baked path at fixed moments of montage time - a 1/240 s grid, the window's ends, and where the region
	// first meets the ball (bisected between grid points) - from the last tick to now: the same touch at any frame rate. The
	// ball rolls straight within the tick.
	const double Rate = GetPlayRate();
	const float From = FMath::Max(LastMontageTime, Window.Start), To = FMath::Min(T, Window.End);
	const double Reach = Radius + (Row ? Row->Radius : 0.f);
	const float Grid = float(Rate / 240.0);
	FVector B;
	const auto GapAt = [&](float S) { // sets P, N, B to moment S
		BakedRegionAt(D, S, T, P, N);
		B = Ball - BallVelocity * (double(T - S) / Rate);
		return float(FVector::Dist(P, B) - Reach);
	};
	const auto TryAt = [&](float S, float G) { // the resolver at moment S, after GapAt(S)
		const FVector Point = P, Normal = N, BallAt = B;
		FVector Before, Unused;
		BakedRegionAt(D, S - float(Rate / 480.0), T, Before, Unused);
		return Candidate(Point, Normal, (Point - Before) * 480.0, BallAt, G, To > From ? (S - From) / (To - From) : 1.f);
	};
	// This tick takes the grid points after From up to To (by index, so ticks tile the grid without gaps), plus the window's
	// end if it falls in this tick; the one at or before From brackets a meeting that starts now (the last tick tested it).
	const int32 J0 = FMath::FloorToInt(From / Grid), J1 = FMath::FloorToInt(To / Grid);
	const bool bWindowEnds = T >= Window.End;
	float PrevS = FMath::Max(float(J0) * Grid, Window.Start);
	float PrevG = GapAt(PrevS);
	if (PrevG <= 0.f && LastMontageTime < Window.Start && TryAt(PrevS, PrevG)) {
		return; // on the ball as the window opens
	}
	float BestS = PrevS, BestG = PrevG;
	for (int32 J = J0 + 1; J <= J1 + (bWindowEnds ? 1 : 0); ++J) {
		const float S = J > J1 ? Window.End : float(J) * Grid;
		if (S <= PrevS) {
			continue; // the window's end on the last grid point
		}
		const float G = GapAt(S);
		if (G < BestG) {
			BestS = S;
			BestG = G;
		}
		if (G <= 0.f) {
			if (PrevG > 0.f) {
				float Lo = PrevS, Hi = S;
				for (int32 i = 0; i < 12; ++i) {
					const float Mid = 0.5f * (Lo + Hi);
					if (GapAt(Mid) > 0.f) {
						Lo = Mid;
					} else {
						Hi = Mid;
					}
				}
				if (TryAt(Hi, GapAt(Hi))) {
					return; // where they first meet
				}
				GapAt(S);
			}
			if (TryAt(S, G)) {
				return;
			}
		}
		PrevS = S;
		PrevG = G;
	}
	TryAt(BestS, GapAt(BestS)); // no touch this tick: the record shows the closest moment
}

void UFootballSkillComponent::Touch(int32 Index, float T, const FVector& BallCenter, const FVector& BodyVelocity) {
	FWindow& Window = Windows[Index];
	const bool bFirstContact = !Window.bTouched;
	const FBallContactDescriptor D = ActiveSkill->GetTunedDescriptor(Window.ContactIndex, bActiveMirrored);
	const double Radius = BallMovement->GetBallRadius();
	FVector BallVelocity = BallMovement->GetBallVelocity(), SpinDeg = FVector::ZeroVector;
	if (BallContacts) {
		BallContacts->GetPendingMotion(BallVelocity, SpinDeg); // this frame's queued response included
	}
	const UPrimitiveComponent* BallBody = Cast<UPrimitiveComponent>(ActiveBall->GetRootComponent());
	const bool bBodyOk = BallBody && BallBody->IsSimulatingPhysics();
	const double Mass = bBodyOk ? FMath::Max(double(BallBody->GetMass()), 0.01) : 0.43;
	const bool bCarry = IsCarry(D.ResponseMode);
	const bool bLeaves = !bCarry && !Windows.IsValidIndex(Index + 1);
	// Spec §122 / §7: one raw collision of the ball with the moving body region, at the swept contact point (or, for a
	// carry, an inelastic push that grips).
	const FBodyRegionRow* Row = Regions->GetRegionRow(D.BodyContactRegion);
	const FActionBallResponseProfile& Surface = ActiveSkill->BallResponseProfile;
	FBallResponseInput In;
	In.Mode = bCarry ? EBallResponseMode::PhysicalRedirect : D.ResponseMode;
	In.BallCenter = BallCenter;
	In.BallVelocity = BallVelocity;
	In.BallAngularVelocity = FMath::DegreesToRadians(SpinDeg);
	In.BallRadius = float(Radius);
	In.BallMass = float(Mass);
	In.InertiaFactor = bBodyOk ? float(BallBody->GetInertiaTensor().X / (Mass * Radius * Radius)) : 0.667f;
	const FVector Normal = (In.BallCenter - Window.Result.ContactPoint).GetSafeNormal();
	In.ContactNormal = Normal.IsNearlyZero() ? Window.Result.ContactNormal : Normal;
	In.ContactPoint = In.BallCenter - In.ContactNormal * Radius;
	In.BodyVelocity = BodyVelocity;
	In.Restitution = bCarry ? 0.f : (D.Restitution >= 0.f ? D.Restitution : (Row ? Row->Restitution : 0.5f)) * Surface.RestitutionScale;
	In.Friction = bCarry ? 10.f : (D.TangentialGrip >= 0.f ? D.TangentialGrip : DefaultFriction) * Surface.FrictionScale;
	In.ContactStrength = Surface.ContactStrength;
	const FBallResponse Raw = UBallPhysicsResponse::ComputeResponse(In);
	if (!bCarry && !Raw.bContact) {
		// The ball's surface isn't closing on the body after all (its spin, a low contact): no touch, so no assist either;
		// the window stays open.
		Window.Result.Validation = EContactValidation::WrongDirection;
		return;
	}
	Window.bTouched = true;
	FVector Physical = Raw.bContact ? Raw.VelocityAfter : BallVelocity;
	FVector Spin = Raw.bContact ? Raw.AngularVelocityAfter : In.BallAngularVelocity; // rad/s
	// A grounded ball that isn't lifted leaves the touch rolling: ground friction takes its skid at once, so the rolling
	// predictions and the intent hold (master plan 6.4: left to skid, the elastico's outside touch lost ~70% of its
	// sideways speed and the inside touch missed).
	const bool bRolls = BallMovement->IsGrounded() && Physical.Z < MaxRollingLift;
	if (bRolls) {
		Physical = UBallPhysicsResponse::RollingVelocity(Physical, Spin, In.BallRadius, In.InertiaFactor);
	}
	// Spec §14: bounded assist toward the intent - the next contact's spot in time, or after the last contact its exit.
	FVector Needed = Physical, Applied = Physical;
	if (!bCarry) {
		Needed = bLeaves ? Basis.TransformVector(D.PreferredDirection).GetSafeNormal() * double(D.PreferredSpeed)
			: IntentToward(Index + 1, T, ActiveBall->GetActorLocation()); // from where the new velocity starts (this tick's end)
		const FActionAssistProfile& AssistProfile = ActiveSkill->AssistProfile;
		FBallAssistInput Assist;
		Assist.RawVelocity = Physical;
		Assist.IntendedDirection = Needed.GetSafeNormal();
		Assist.IntendedSpeed = float(Needed.Size());
		Assist.Strength = D.AssistStrength * AssistProfile.StrengthScale;
		Assist.MaxAngleDegrees = D.MaxAssistAngleDegrees * AssistProfile.BoundsScale;
		Assist.MaxSpeedChange = D.MaxAssistSpeed * AssistProfile.BoundsScale;
		Assist.bArcade = D.bArcadeAssist;
		Applied = UBallAssist::BoundedBlend(ScaleAssistForPlayer(Assist)).Velocity;
	}
	if (bRolls) {
		Applied.Z = 0.0;
		Spin = FVector::CrossProduct(FVector::UpVector, Applied) / Radius + FVector(0.0, 0.0, Spin.Z); // rolling, its twist kept
	}
	if (BallContacts) {
		BallContacts->TryClaim(EContactPriority::Skill, GetOwner());
	}
	const EBallState NewState = bLeaves ? (BallMovement->IsGrounded() ? EBallState::Free : EBallState::Airborne) : EBallState::SkillControlled;
	BallMovement->ApplyTouch(Applied, FMath::RadiansToDegrees(Spin), NewState, bLeaves ? nullptr : GetOwner());
	bWantsBall = false;
	bEverTouched = true;
	bOwned = !bLeaves;
	bReleased = bReleased || bLeaves;
	bTouchedThisTick = true;
	TouchPhysical = Physical;
	TouchNeeded = Needed;
	TouchApplied = Applied;
	TouchImpulse = (Applied - BallVelocity) * Mass;
	if (bAlign && bFirstContact && !bLeaves && Windows.IsValidIndex(Index + 1) && ActiveSkill->AlignmentProfile.bRealignBeforeEachContact) {
		Realign(Index + 1, T);
	}
	LastTarget = (bLeaves || bCarry || !Windows.IsValidIndex(Index + 1)) ? In.BallCenter + Applied * 0.25 : PredictContactSpot(Index + 1, T);
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
		const FBallContactDescriptor& Desc = ActiveSkill->ContactDescriptors[Window.ContactIndex];
		if (bOwned && IsCarry(Desc.ResponseMode) && (Desc.ContactType == EContactType::Release || Desc.bReleaseAfterContact || Index == Windows.Num() - 1)) {
			// The carry ends: the ball leaves the skill with the velocity the body gave it.
			LetGo();
			bReleased = true;
			bTouchedThisTick = true;
			TouchPhysical = TouchNeeded = TouchApplied = BallMovement->GetBallVelocity();
		}
		return;
	}
	if (bWantsBall && Index == 0) {
		// The first touch never happened (out of reach, blocked, deflected): the move carries on without the ball.
		bWantsBall = false;
		BallMovement->EndDribble(GetOwner());
		UE_LOG(LogFootballSkill, Log, TEXT("%s: touch %d missed (%s, the foot passed %.1f cm off the ball), carrying on without the ball"),
			*ActiveSkill->GetName(), Window.ContactIndex, *UEnum::GetValueAsString(Window.Result.Validation), FMath::Max(Window.Result.ClosestApproach, 0.f));
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
		return W.Phase == EWindowPhase::Open && (!W.bTouched || IsCarry(ActiveSkill->ContactDescriptors[W.ContactIndex].ResponseMode));
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
	if (bWantsBall) {
		BallMovement->EndDribble(GetOwner());
	}
	bWantsBall = false;
	bWarping = false;
	bEntryMomentum = false;
	// Recovery (FActionRecoveryProfile): the normal control stays off the ball a little longer if the skill asks.
	HoldBallUntil = Ended->RecoveryProfile.ResumeControlDelay > 0.f ? GetWorld()->GetTimeSeconds() + Ended->RecoveryProfile.ResumeControlDelay : 0.0;
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
	Sample.bBallDriven = bOwned;
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
		Sample.ContactNormal = Window.Result.ContactNormal;
		Sample.BodyVelocity = Tracker.Velocity;
	}
	Sample.TargetPos = LastTarget;
	Sample.AlignmentApplied = AlignmentApplied;
	if (bTouchedThisTick) {
		Sample.TouchPhysical = TouchPhysical;
		Sample.TouchNeeded = TouchNeeded;
		Sample.TouchApplied = TouchApplied;
		Sample.TouchImpulse = TouchImpulse;
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
		if (bWantsBall || bOwned) {
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
	const FString Text = FString::Printf(TEXT("%s  %s  ball %s  t=%.2f%s"), *ActiveSkill->ActionTag.ToString(), *UEnum::GetDisplayValueAsText(SkillState).ToString(),
		BallStateComp ? *UEnum::GetDisplayValueAsText(BallStateComp->GetState()).ToString() : TEXT("-"), T, *ContactText);
	DrawDebugString(World, GetOwner()->GetActorLocation() + FVector(0.0, 0.0, 120.0), Text, nullptr, FColor::White, 0.f);
}
