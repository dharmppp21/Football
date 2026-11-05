#include "FootballActionComponent.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BallContactResolver.h"
#include "BallMovementComponent.h"
#include "BallStateComponent.h"
#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "FootballActionData.h"
#include "FootballSkillComponent.h"
#include "FootballSkillData.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "HAL/IConsoleManager.h"
#include "PlayerAttributesData.h"

DEFINE_LOG_CATEGORY_STATIC(LogFootballAction, Log, All);

static TAutoConsoleVariable<int32> CVarFootballDebugAction(TEXT("fb.Debug.Action"), 0,
	TEXT("Draw the running action: tag, state, its alignment target (predicted ball yellow, target root cyan, facing blue) and correction."));

static FString EnumText(EActionValidation Value) { return UEnum::GetDisplayValueAsText(Value).ToString(); }
static FString EnumText(EActionInterruptReason Value) { return UEnum::GetDisplayValueAsText(Value).ToString(); }

UFootballActionComponent::UFootballActionComponent() {
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false; // ticks while an action runs, a request waits, or the recorder's tail
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	ActionDirectory.Path = TEXT("/Game/Football/Data");
}

void UFootballActionComponent::BeginPlay() {
	Super::BeginPlay();
	Skills = GetOwner()->FindComponentByClass<UFootballSkillComponent>();
	if (Skills) {
		Skills->OnSkillStateChanged.AddDynamic(this, &UFootballActionComponent::OnSkillStateChanged);
		Skills->OnSkillEnded.AddDynamic(this, &UFootballActionComponent::OnSkillEnded);
		AddTickPrerequisiteComponent(Skills); // the recorder reads the skill's tick of this frame
	}
	LoadedAttributes = AttributesAsset.LoadSynchronous();
	LoadedActions.Reset();
	TArray<FAssetData> Assets;
	if (const IAssetRegistry* Registry = IAssetRegistry::Get()) {
		Registry->GetAssetsByPath(FName(*ActionDirectory.Path), Assets, true);
	}
	for (const FAssetData& Asset : Assets) {
		if (Asset.IsInstanceOf(UFootballActionData::StaticClass())) {
			if (UFootballActionData* Action = Cast<UFootballActionData>(Asset.GetAsset())) {
				LoadedActions.Add(Action);
			}
		}
	}
	UE_LOG(LogFootballAction, Log, TEXT("%s: %d actions in %s"), *GetNameSafe(GetOwner()), LoadedActions.Num(), *ActionDirectory.Path);
}

UFootballActionData* UFootballActionComponent::FindAction(FGameplayTag ActionTag) const {
	for (UFootballActionData* Action : LoadedActions) {
		if (Action && Action->ActionTag == ActionTag) {
			return Action;
		}
	}
	return nullptr;
}

bool UFootballActionComponent::RequestAction(FGameplayTag ActionTag, FActionContext Context) {
	UFootballActionData* Action = FindAction(ActionTag);
	if (!Action) {
		UE_LOG(LogFootballAction, Log, TEXT("No action tagged %s in %s"), *ActionTag.ToString(), *ActionDirectory.Path);
		LastRejection = EActionValidation::UnknownAction;
		OnActionRejected.Broadcast(ActionTag, LastRejection);
		return false;
	}
	return RequestActionData(Action, Context);
}

bool UFootballActionComponent::RequestActionWithInput(FGameplayTag ActionTag, AActor* Ball, FVector InputDirection, bool bMirrored) {
	FActionContext Context;
	Context.BallActor = Ball;
	Context.InputDirection = InputDirection;
	Context.bMirrored = bMirrored;
	return RequestAction(ActionTag, Context);
}

bool UFootballActionComponent::RequestActionData(UFootballActionData* Action, FActionContext Context) {
	Context.RequestTime = float(GetWorld()->GetTimeSeconds());
	// Never a queue (§63): a new request replaces one still waiting.
	bPending = false;
	PendingAction = nullptr;
	EActionValidation Reason = EActionValidation::Valid;
	if (TryStart(Action, Context, Reason, true)) {
		return true;
	}
	if (Action && ShouldBuffer(Reason, 0.f, Action->InputBufferTime)) {
		bPending = true;
		PendingAction = Action;
		PendingContext = Context;
		PendingReason = Reason;
		SetComponentTickEnabled(true);
	} else if (WantsFallback(Reason)) {
		RunFallback(Action, Context, Reason);
	}
	return false;
}

void UFootballActionComponent::FillContext(FActionContext& Context) const {
	const AActor* Owner = GetOwner();
	Context.PlayerLocation = Owner->GetActorLocation();
	Context.PlayerVelocity = Owner->GetVelocity();
	Context.PlayerForward = Owner->GetActorForwardVector();
	Context.PlayerRight = Owner->GetActorRightVector();
	if (const APawn* Pawn = Cast<APawn>(Owner)) {
		if (const AController* Controller = Pawn->GetController()) {
			Context.CameraDirection = Controller->GetControlRotation().Vector();
		}
	}
	Context.BallLocation = Context.BallActor ? Context.BallActor->GetActorLocation() : FVector::ZeroVector;
	const UBallMovementComponent* Movement = Context.BallActor ? Context.BallActor->FindComponentByClass<UBallMovementComponent>() : nullptr;
	Context.BallVelocity = Movement ? Movement->GetBallVelocity() : FVector::ZeroVector;
	if (LoadedAttributes) {
		Context.AttributeSnapshot = LoadedAttributes->Attributes;
		Context.PreferredFoot = LoadedAttributes->PreferredFoot;
	}
	Context.OpponentPressure = 0.f;
}

float UFootballActionComponent::CooldownLeft(const UFootballActionData* Action) const {
	const double* End = Action ? LastEndTime.Find(Action->ActionTag) : nullptr;
	return End ? FMath::Max(0.f, Action->Cooldown - float(GetWorld()->GetTimeSeconds() - *End)) : 0.f;
}

bool UFootballActionComponent::IsSpaceFree(const FVector& Travel, const AActor* Ball) const {
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	const UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
	if (!Capsule || Travel.SizeSquared2D() < 1.0) {
		return true;
	}
	// A slightly smaller capsule lifted off the floor, so only what the action would walk into counts.
	const float Radius = FMath::Max(Capsule->GetScaledCapsuleRadius() - 2.f, 1.f);
	const float HalfHeight = FMath::Max(Capsule->GetScaledCapsuleHalfHeight() - 2.f, Radius);
	const FVector From = Character->GetActorLocation() + FVector(0.0, 0.0, 2.0);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FootballActionSpace), false, Character);
	if (Ball) {
		Params.AddIgnoredActor(Ball);
	}
	FHitResult Hit;
	const bool bHit = GetWorld()->SweepSingleByChannel(Hit, From, From + FVector(Travel.X, Travel.Y, 0.0), FQuat::Identity, Capsule->GetCollisionObjectType(),
		FCollisionShape::MakeCapsule(Radius, HalfHeight), Params, FCollisionResponseParams(Capsule->GetCollisionResponseToChannels()));
	return !(bHit && Hit.bBlockingHit && !Hit.bStartPenetrating);
}

bool UFootballActionComponent::TryStart(UFootballActionData* Action, FActionContext& Context, EActionValidation& OutReason, bool bLog) {
	const FGameplayTag Tag = Action ? Action->ActionTag : FGameplayTag();
	const FString Name = GetNameSafe(Action);
	const bool bIdle = ActiveAction == nullptr;
	if (bIdle && bLog) {
		SetActionState(EActionState::Requested, Tag);
		SetActionState(EActionState::Validating, Tag);
	}
	FillContext(Context);
	const double Now = GetWorld()->GetTimeSeconds();
	ESkillDirection Direction = ESkillDirection::Forward;
	bool bHasDirection = true;
	FSkillBasis Basis = MakeActionBasis(Action, Context, Direction, bHasDirection);
	FActionValidationInput In;
	FActionAlignmentTarget Target;
	FVector Travel = FVector::ZeroVector;
	UFootballSkillData* Skill = Cast<UFootballSkillData>(Action);
	OutReason = EActionValidation::Valid;
	if (!Action) {
		OutReason = EActionValidation::UnknownAction;
	} else if (!Skill || !Skills) {
		OutReason = EActionValidation::CannotPlay; // only skills have an executor until master plan Phases 8-13
	} else if (!Skills->PrepareSkill(Skill, Context, Basis, Target, In.bReachable, Travel)) {
		OutReason = EActionValidation::CannotPlay;
	}
	if (OutReason == EActionValidation::Valid) {
		const AActor* Ball = Context.BallActor;
		const UPrimitiveComponent* Body = Ball ? Cast<UPrimitiveComponent>(Ball->GetRootComponent()) : nullptr;
		const UBallStateComponent* State = Ball ? Ball->FindComponentByClass<UBallStateComponent>() : nullptr;
		const UBallMovementComponent* Movement = Ball ? Ball->FindComponentByClass<UBallMovementComponent>() : nullptr;
		const ACharacter* Character = Cast<ACharacter>(GetOwner());
		In.bPlayerCanAct = !(Character && Character->GetCharacterMovement() && Character->GetCharacterMovement()->IsFalling());
		In.ActivePriority = ActiveAction ? ActiveAction->Priority : -1;
		In.bActiveProtected = ActiveAction && !AllowsInterrupt(ActiveAction->InterruptionProfile, EActionInterruptReason::Superseded);
		In.RecoveringPriority = (bIdle && Now < RecoverUntil) ? RecoverPriority : -1;
		In.CooldownLeft = CooldownLeft(Action);
		In.bBallValid = Body && Body->IsSimulatingPhysics() && State && Movement;
		if (State) {
			// every action is a skill so far, so the ball state it needs is SkillControlled.
			const EBallState BallNow = State->GetState();
			const AActor* BallOwner = State->GetBallOwner();
			In.bBallStateAllows = UBallStateComponent::IsTransitionAllowed(BallNow, EBallState::SkillControlled);
			In.bBallOwnedByOther = BallOwner && BallOwner != GetOwner() && (BallNow == EBallState::Controlled || BallNow == EBallState::Receiving);
		}
		In.BallDistance = Ball ? float(FVector::Dist2D(Ball->GetActorLocation(), Context.PlayerLocation)) : 0.f;
		In.bHasDirection = bHasDirection;
		In.PlayerSpeed = float(Context.PlayerVelocity.Size2D());
		In.bSpaceFree = IsSpaceFree(Travel, Ball);
		OutReason = ValidateAction(Action, Context, In);
	}
	if (OutReason != EActionValidation::Valid) {
		LastRejection = OutReason;
		if (bLog) {
			FString Why;
			switch (OutReason) {
			case EActionValidation::Busy:
				Why = ActiveAction ? FString::Printf(TEXT("%s is still playing"), *ActiveAction->GetName()) : FString(TEXT("still recovering from the last action"));
				break;
			case EActionValidation::Cooldown:
				Why = FString::Printf(TEXT("%.2f s of cooldown left"), In.CooldownLeft);
				break;
			case EActionValidation::WrongFoot:
				Why = FString::Printf(TEXT("not allowed with the %s foot"), Context.bMirrored ? TEXT("left") : TEXT("right"));
				break;
			case EActionValidation::BallState:
			case EActionValidation::NotOwner:
			case EActionValidation::NoBall: {
				const UBallStateComponent* State = Context.BallActor ? Context.BallActor->FindComponentByClass<UBallStateComponent>() : nullptr;
				Why = State ? FString::Printf(TEXT("the ball is %s (owner %s)"), *UEnum::GetValueAsString(State->GetState()), *GetNameSafe(State->GetBallOwner()))
					: FString(TEXT("no ball with BallMovement and BallState"));
				break;
			}
			case EActionValidation::TooFar:
				Why = FString::Printf(TEXT("the ball is %.0f cm away (max %.0f)"), In.BallDistance, Action->StartDistance);
				break;
			case EActionValidation::Unreachable:
				Why = Action->AlignmentProfile.bAlignPlayer
					? FString::Printf(TEXT("the ball would be %.0f cm from contact %d's spot, %.1f cm of it out of the alignment limits"), Target.RequiredCorrection,
						Target.ContactIndex, Target.Residual)
					: FString::Printf(TEXT("the ball is %.0f cm from the first contact, only %.0f cm reachable in time"), Target.RequiredCorrection,
						Target.RequiredCorrection - Target.Residual);
				break;
			case EActionValidation::WrongSpeed:
				Why = FString::Printf(TEXT("the player moves at %.0f cm/s (allowed %.0f-%s)"), In.PlayerSpeed, Action->MinSpeed,
					Action->MaxSpeed > 0.f ? *FString::Printf(TEXT("%.0f"), Action->MaxSpeed) : TEXT("any"));
				break;
			default:
				Why = EnumText(OutReason);
				break;
			}
			UE_LOG(LogFootballAction, Log, TEXT("%s refused (%s): %s"), *Name, *EnumText(OutReason), *Why);
		}
		OnActionRejected.Broadcast(Tag, OutReason);
		if (bIdle && bLog) {
			SetActionState(EActionState::None, Tag);
		}
		return false;
	}
	// §64: a higher priority takes over the running action (ValidateAction only lets it through when it may).
	if (ActiveAction) {
		InterruptAction(EActionInterruptReason::Superseded);
	}
	// §120: the start turn turns the whole action.
	if (!FMath::IsNearlyZero(Target.RotationCorrection)) {
		Basis = UBallContactResolver::MakeSkillBasis(Basis.Origin, Basis.Forward.RotateAngleAxis(Target.RotationCorrection, FVector::UpVector), Basis.bMirrored);
	}
	ActiveAction = Action;
	ActiveContext = Context;
	bInterrupted = false;
	bBallLossHandled = false;
	StartTime = Now;
	Record.Reset();
	RecordTag = Tag;
	RecordUntil = 0.0;
	LastPlayerLocation = GetOwner()->GetActorLocation();
	SetActionState(EActionState::Starting, Tag);
	if (!Skills->StartSkill(Skill, Context, Basis, Direction, Target)) {
		ActiveAction = nullptr;
		OutReason = LastRejection = EActionValidation::CannotPlay;
		UE_LOG(LogFootballAction, Warning, TEXT("%s refused (CannotPlay): the skill did not start"), *Name);
		OnActionRejected.Broadcast(Tag, OutReason);
		SetActionState(EActionState::None, Tag);
		return false;
	}
	if (Action->AlignmentProfile.bAlignPlayer && Target.ContactIndex != INDEX_NONE) {
		UE_LOG(LogFootballAction, Log, TEXT("%s aligns for contact %d: move %.1f cm, turn %.1f deg (ball %.1f cm off, %.1f left)"), *Name, Target.ContactIndex,
			Target.PositionCorrection.Size(), Target.RotationCorrection, Target.RequiredCorrection, Target.Residual);
	}
	SetComponentTickEnabled(true);
	return true;
}

void UFootballActionComponent::RunFallback(UFootballActionData* Action, const FActionContext& Context, EActionValidation Reason) {
	const FGameplayTag Fallback = Action ? Action->FallbackActionTag : FGameplayTag();
	if (!Fallback.IsValid()) {
		UE_LOG(LogFootballAction, Log, TEXT("%s dropped (%s), it has no fallback"), *GetNameSafe(Action), *EnumText(Reason));
		return;
	}
	UFootballActionData* FallbackAction = FindAction(Fallback);
	if (FallbackAction && FallbackAction != Action) {
		FActionContext FallbackContext = Context;
		EActionValidation FallbackReason = EActionValidation::Valid;
		const bool bStarted = TryStart(FallbackAction, FallbackContext, FallbackReason, true);
		UE_LOG(LogFootballAction, Log, TEXT("%s falls back to %s (%s)"), *GetNameSafe(Action), *FallbackAction->GetName(), bStarted ? TEXT("started") : *EnumText(FallbackReason));
		return;
	}
	// Spec §121: e.g. "Elastico but ball too far -> NormalDribble" - the normal control simply carries on.
	UE_LOG(LogFootballAction, Log, TEXT("%s falls back to %s (%s): the current control carries on"), *GetNameSafe(Action), *Fallback.ToString(), *EnumText(Reason));
}

bool UFootballActionComponent::InterruptAction(EActionInterruptReason Reason) {
	if (!ActiveAction) {
		return false;
	}
	const bool bAlways = Reason == EActionInterruptReason::Cancelled || Reason == EActionInterruptReason::InvalidState;
	if (!bAlways && !AllowsInterrupt(ActiveAction->InterruptionProfile, Reason)) {
		UE_LOG(LogFootballAction, Log, TEXT("%s plays on through %s (its InterruptionProfile)"), *ActiveAction->GetName(), *EnumText(Reason));
		return false;
	}
	UE_LOG(LogFootballAction, Log, TEXT("%s interrupted: %s"), *ActiveAction->GetName(), *EnumText(Reason));
	LastInterruptReason = Reason;
	bInterrupted = Reason != EActionInterruptReason::Cancelled;
	// §65: the executor releases control of the ball (it keeps its velocity) and restores movement; its end event ends the action.
	if (Skills && Skills->IsSkillActive()) {
		Skills->CancelSkill();
	}
	if (ActiveAction) {
		EndAction(true); // nothing was playing any more
	}
	return true;
}

void UFootballActionComponent::EndAction(bool bCancelled) {
	UFootballActionData* Ended = ActiveAction;
	if (!Ended) {
		return;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	LastEndTime.Add(Ended->ActionTag, Now);
	RecoverUntil = Now + Ended->RecoveryProfile.RecoveryTime;
	RecoverPriority = Ended->Priority;
	SetActionState(bCancelled ? (bInterrupted ? EActionState::Interrupted : EActionState::Cancelled) : EActionState::Completed, Ended->ActionTag);
	RecordSample();
	ActiveAction = nullptr;
	RecordUntil = Now + RecordTail;
	SetActionState(EActionState::None, Ended->ActionTag);
}

void UFootballActionComponent::OnSkillStateChanged(UFootballSkillData* Skill, ESkillState NewState) {
	if (!ActiveAction || Skill != ActiveAction) {
		return;
	}
	const FGameplayTag Tag = ActiveAction->ActionTag;
	switch (NewState) {
	case ESkillState::Starting: {
		const FActionAlignmentTarget& Target = Skills->GetAlignment();
		const bool bAligning = ActiveAction->AlignmentProfile.bAlignPlayer && (!Target.PositionCorrection.IsNearlyZero() || !FMath::IsNearlyZero(Target.RotationCorrection));
		SetActionState(bAligning ? EActionState::Aligning : EActionState::Starting, Tag);
		break;
	}
	case ESkillState::Contacting:
		SetActionState(EActionState::Contacting, Tag);
		break;
	case ESkillState::Redirecting:
		SetActionState(EActionState::Playing, Tag); // between touches the ball rolls on by itself
		break;
	case ESkillState::Releasing:
		SetActionState(EActionState::Releasing, Tag);
		break;
	case ESkillState::Recovering:
		SetActionState(EActionState::Recovering, Tag);
		break;
	default:
		break; // Completed / Cancelled / None: EndAction
	}
}

void UFootballActionComponent::OnSkillEnded(UFootballSkillData* Skill, bool bCancelled) {
	if (ActiveAction && Skill == ActiveAction) {
		EndAction(bCancelled);
	}
}

void UFootballActionComponent::SetActionState(EActionState NewState, FGameplayTag Tag) {
	if (NewState != ActionState) {
		ActionState = NewState;
		OnActionStateChanged.Broadcast(Tag, NewState);
	}
}

FActionAlignmentTarget UFootballActionComponent::GetAlignmentTarget() const {
	return Skills ? Skills->GetAlignment() : FActionAlignmentTarget();
}

void UFootballActionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const double Now = GetWorld()->GetTimeSeconds();
	// §65 interruptions this component sees itself; tackles, collisions and stuns come from their systems through InterruptAction.
	if (ActiveAction && Skills && Skills->WasBallTakenAway() && !bBallLossHandled) {
		bBallLossHandled = true;
		InterruptAction(EActionInterruptReason::BallLost);
	}
	if (ActiveAction) {
		const ACharacter* Character = Cast<ACharacter>(GetOwner());
		if (Character && Character->GetCharacterMovement() && Character->GetCharacterMovement()->IsFalling()) {
			InterruptAction(EActionInterruptReason::InvalidState);
		}
	}
	// §63: the waiting request, judged by the situation now.
	if (bPending) {
		UFootballActionData* Action = PendingAction;
		FActionContext Context = PendingContext;
		const float Age = float(Now) - Context.RequestTime;
		EActionValidation Reason = EActionValidation::Valid;
		if (TryStart(Action, Context, Reason, false)) {
			bPending = false;
			PendingAction = nullptr;
			UE_LOG(LogFootballAction, Log, TEXT("%s started from the input buffer after %.0f ms"), *GetNameSafe(Action), Age * 1000.f);
		} else if (!ShouldBuffer(Reason, Age, Action ? Action->InputBufferTime : 0.f)) {
			bPending = false;
			PendingAction = nullptr;
			UE_LOG(LogFootballAction, Log, TEXT("%s left the input buffer after %.0f ms: %s"), *GetNameSafe(Action), Age * 1000.f, *EnumText(Reason));
			if (WantsFallback(Reason)) {
				RunFallback(Action, Context, Reason);
			}
		} else {
			PendingReason = Reason;
		}
	}
	if (ActiveAction || Now < RecordUntil) {
		RecordSample();
	}
	if (ActiveAction && CVarFootballDebugAction.GetValueOnGameThread() != 0) {
		DrawDebug();
	}
	if (!ActiveAction && !bPending && Now >= RecordUntil) {
		SetComponentTickEnabled(false);
	}
}

void UFootballActionComponent::RecordSample() {
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (!bAlwaysRecord && !(Pawn && Pawn->IsLocallyControlled())) {
		return;
	}
	const float DeltaTime = GetWorld()->GetDeltaSeconds();
	FActionRecordSample& Sample = Record.AddDefaulted_GetRef();
	Sample.Time = float(GetWorld()->GetTimeSeconds() - StartTime);
	Sample.ActionTag = RecordTag;
	Sample.State = ActionState;
	Sample.PlayerTransform = GetOwner()->GetActorTransform();
	const FVector Location = GetOwner()->GetActorLocation();
	Sample.PlayerVelocity = DeltaTime > 0.f ? (Location - LastPlayerLocation) / DeltaTime : FVector::ZeroVector;
	LastPlayerLocation = Location;
	if (const AActor* Ball = ActiveContext.BallActor) {
		Sample.BallTransform = Ball->GetActorTransform();
		if (const UBallMovementComponent* Movement = Ball->FindComponentByClass<UBallMovementComponent>()) {
			Sample.BallVelocity = Movement->GetBallVelocity();
		}
		if (const UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Ball->GetRootComponent())) {
			Sample.BallAngularVelocity = Body->GetPhysicsAngularVelocityInDegrees();
		}
	}
	if (const FSkillDebugSample* Skill = Skills ? Skills->GetLatestSample() : nullptr) {
		Sample.ContactIndex = Skill->ContactIndex;
		Sample.ContactValidity = Skill->Validation;
		Sample.ContactPoint = Skill->SurfacePoint;
		Sample.ContactNormal = Skill->ContactNormal;
		Sample.BodyVelocity = Skill->BodyVelocity;
		Sample.RawResponse = Skill->TouchPhysical;
		Sample.AssistedResponse = Skill->TouchApplied;
		Sample.FinalImpulse = Skill->TouchImpulse;
		Sample.AlignmentCorrection = Skill->AlignmentApplied;
	}
}

void UFootballActionComponent::DrawDebug() const {
	const UWorld* World = GetWorld();
	const FActionAlignmentTarget Target = GetAlignmentTarget();
	FString Text = FString::Printf(TEXT("%s  %s"), *ActiveAction->ActionTag.ToString(), *UEnum::GetDisplayValueAsText(ActionState).ToString());
	if (Target.ContactIndex != INDEX_NONE && ActiveAction->AlignmentProfile.bAlignPlayer) {
		Text += FString::Printf(TEXT("  %s: move %.0f cm, turn %.0f deg, %.1f cm left%s"), *Target.WarpTargetName.ToString(), Target.PositionCorrection.Size(),
			Target.RotationCorrection, Target.Residual, Target.bReachable ? TEXT("") : TEXT(" (out of reach)"));
		DrawDebugSphere(World, Target.PredictedBall, 11.f, 12, FColor::Yellow);
		DrawDebugSphere(World, Target.Location + FVector(0.0, 0.0, 2.0), 6.f, 8, FColor::Cyan);
		DrawDebugDirectionalArrow(World, Target.Location + FVector(0.0, 0.0, 2.0), Target.Location + FVector(0.0, 0.0, 2.0) + Target.Rotation.Vector() * 60.0,
			15.f, FColor::Blue);
	}
	DrawDebugString(World, GetOwner()->GetActorLocation() + FVector(0.0, 0.0, 140.0), Text, nullptr, FColor::White, 0.f);
}

// ---- the rules ------------------------------------------------------------------------------------------------------

EActionValidation UFootballActionComponent::ValidateAction(const UFootballActionData* Action, const FActionContext& Context, const FActionValidationInput& In) {
	if (!Action) {
		return EActionValidation::UnknownAction;
	}
	// ValidateState
	if (!In.bPlayerCanAct) {
		return EActionValidation::InvalidState;
	}
	if ((In.ActivePriority >= 0 && !Supersedes(Action->Priority, In.ActivePriority, In.bActiveProtected))
		|| (In.RecoveringPriority >= 0 && Action->Priority <= In.RecoveringPriority)) {
		return EActionValidation::Busy;
	}
	// ValidateCooldown
	if (In.CooldownLeft > 0.f) {
		return EActionValidation::Cooldown;
	}
	// ValidateFoot
	if ((Context.bMirrored && Action->AllowedFoot == EFootSelection::Right) || (!Context.bMirrored && Action->AllowedFoot == EFootSelection::Left)) {
		return EActionValidation::WrongFoot;
	}
	// ValidateBall / Ownership / Distance: only an action that touches the ball needs one.
	if (Action->ContactDescriptors.Num() > 0) {
		if (!In.bBallValid) {
			return EActionValidation::NoBall;
		}
		if (!In.bBallStateAllows) {
			return EActionValidation::BallState;
		}
		if (In.bBallOwnedByOther) {
			return EActionValidation::NotOwner;
		}
		if (In.BallDistance > Action->StartDistance) {
			return EActionValidation::TooFar;
		}
	}
	// ValidateDirection
	if (!In.bHasDirection) {
		return EActionValidation::WrongDirection;
	}
	// ValidateSpeed
	if (In.PlayerSpeed < Action->MinSpeed || (Action->MaxSpeed > 0.f && In.PlayerSpeed > Action->MaxSpeed)) {
		return EActionValidation::WrongSpeed;
	}
	// ValidateReachability (§72-73): never start a visually guaranteed miss.
	if (Action->ContactDescriptors.Num() > 0 && !In.bReachable) {
		return EActionValidation::Unreachable;
	}
	// ValidateCollisionSpace
	if (!In.bSpaceFree) {
		return EActionValidation::NoSpace;
	}
	return EActionValidation::Valid;
}

FActionAlignmentTarget UFootballActionComponent::CalculateAlignmentTarget(const FAlignmentQuery& Q) {
	FActionAlignmentTarget T;
	T.WarpTargetName = FName(*FString::Printf(TEXT("Contact%d"), Q.ContactIndex));
	T.ContactIndex = Q.ContactIndex;
	T.WindowStart = Q.WindowStart;
	T.WindowEnd = Q.WindowEnd;
	T.PredictedBall = Q.PredictedBall;
	T.TimeToContact = Q.TimeAvailable;
	const FVector Expected = Q.PredictedRoot + Q.Basis.TransformVector(Q.ExpectedBallOffset);
	T.RequiredCorrection = float(FVector::Dist(Q.PredictedBall, Expected + Q.EntryOffset));
	// Turn first, bounded: swing the baked move about the pivot so the expected spot points at the ball (the entry
	// momentum's share keeps its own direction, so it comes off the ball's side first).
	double Yaw = 0.0;
	const FVector From(Expected.X - Q.Pivot.X, Expected.Y - Q.Pivot.Y, 0.0);
	const FVector To(Q.PredictedBall.X - Q.EntryOffset.X - Q.Pivot.X, Q.PredictedBall.Y - Q.EntryOffset.Y - Q.Pivot.Y, 0.0);
	if (Q.bAllowRotation && Q.Profile.MaxRotationCorrection > 0.f && From.Size() > 1.0 && To.Size() > 1.0) {
		const double Angle = FMath::RadiansToDegrees(FMath::Atan2(FVector::CrossProduct(From, To).Z, FVector::DotProduct(From, To)));
		Yaw = FMath::Clamp(Angle, -double(Q.Profile.MaxRotationCorrection), double(Q.Profile.MaxRotationCorrection));
	}
	const FQuat Turn(FVector::UpVector, FMath::DegreesToRadians(Yaw));
	const FVector ExpectedTurned = Q.Pivot + Turn.RotateVector(Expected - Q.Pivot) + Q.EntryOffset;
	const FVector RootTurned = Q.Pivot + Turn.RotateVector(Q.PredictedRoot - Q.Pivot) + Q.EntryOffset;
	// Then move for the rest, on the ground plane, bounded by distance and by speed over the window ("do not over-warp").
	const FVector Move(Q.PredictedBall.X - ExpectedTurned.X, Q.PredictedBall.Y - ExpectedTurned.Y, 0.0);
	const double Limit = FMath::Min(double(Q.Profile.MaxPositionCorrection), double(Q.Profile.MaxCorrectionSpeed) * FMath::Max(double(Q.TimeAvailable), 0.0));
	T.PositionCorrection = Move.GetClampedToMaxSize(Limit);
	T.RotationCorrection = float(Yaw);
	T.Residual = float(FVector::Dist(Q.PredictedBall, ExpectedTurned + T.PositionCorrection)); // with the height the warp can't fix
	T.bReachable = T.Residual <= Q.Tolerance;
	T.Location = RootTurned + T.PositionCorrection;
	T.Rotation = Turn.RotateVector(Q.Basis.Forward).Rotation();
	return T;
}

FSkillBasis UFootballActionComponent::MakeActionBasis(const UFootballActionData* Action, const FActionContext& Context, ESkillDirection& OutDirection,
	bool& bOutHasDirection) {
	OutDirection = ESkillDirection::Forward;
	bOutHasDirection = true;
	const auto Along = [&](const FVector& Direction) {
		const FVector Flat(Direction.X, Direction.Y, 0.0);
		if (Flat.Size() < 0.3) {
			bOutHasDirection = false;
			return UBallContactResolver::MakeSkillBasis(Context.PlayerLocation, Context.PlayerForward, Context.bMirrored);
		}
		OutDirection = ESkillDirection::Custom;
		return UBallContactResolver::MakeSkillBasis(Context.PlayerLocation, Flat, Context.bMirrored);
	};
	switch (Action ? Action->DirectionMode : EActionDirectionMode::FacingSnapped) {
	case EActionDirectionMode::Input:
		return Along(Context.InputDirection);
	case EActionDirectionMode::Target: {
		FVector To = Context.TargetDirection;
		if (Context.TargetActor) {
			To = Context.TargetActor->GetActorLocation() - Context.PlayerLocation;
		} else if (!Context.TargetLocation.IsZero()) {
			To = Context.TargetLocation - Context.PlayerLocation;
		}
		return Along(To.GetSafeNormal2D());
	}
	case EActionDirectionMode::Camera:
		return Along(Context.CameraDirection.GetSafeNormal2D());
	default:
		return UBallContactResolver::MakeSkillBasisFromInput(Context.PlayerLocation, Context.PlayerForward, Context.InputDirection,
			Action ? Action->AllowedDirections : 0xFF, Context.bMirrored, OutDirection);
	}
}

bool UFootballActionComponent::ShouldBuffer(EActionValidation Reason, float Age, float BufferTime) {
	switch (Reason) {
	case EActionValidation::Valid:
	case EActionValidation::UnknownAction:
	case EActionValidation::CannotPlay:
	case EActionValidation::WrongFoot:
		return false; // nothing about these changes while the request waits
	default:
		return Age < BufferTime;
	}
}

bool UFootballActionComponent::WantsFallback(EActionValidation Reason) {
	return Reason == EActionValidation::TooFar || Reason == EActionValidation::Unreachable;
}

bool UFootballActionComponent::AllowsInterrupt(const FActionInterruptionProfile& Profile, EActionInterruptReason Reason) {
	return (Profile.AllowedReasons & (1 << int32(Reason))) != 0;
}

bool UFootballActionComponent::Supersedes(int32 IncomingPriority, int32 ActivePriority, bool bActiveProtected) {
	return !bActiveProtected && IncomingPriority > ActivePriority;
}
