#include "BallInteractionComponent.h"
#include "BallContactResolver.h"
#include "BallMovementComponent.h"
#include "BallStateComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PlayerBallInteractionComponent.h"

static TAutoConsoleVariable<int32> CVarFootballDebugPhysics(TEXT("fb.Debug.Physics"), 0,
	TEXT("1 = draw every arbitrated response command at the ball for 1 s: green = applied (arrow = the impulse or velocity change), red = dropped."));
static TAutoConsoleVariable<int32> CVarFootballDebugBall(TEXT("fb.Debug.Ball"), 0,
	TEXT("1 = draw the ball's velocity (yellow), spin axis (magenta, longer = faster) and state, speed and spin."));
static constexpr int32 MaxHistory = 64;

TAutoConsoleVariable<int32> CVarFootballDebugContacts(TEXT("fb.Debug.Contact"), 0,
	TEXT("1 = draw every body contact the ball handles (body point red, normal blue, velocity in yellow / out green, with region and priority) and a running skill's contact: its region's previous point and path this tick (yellow) and where the sweep meets the ball (cyan)."));
TAutoConsoleVariable<int32> CVarFootballDebugRegions(TEXT("fb.Debug.Regions"), 0,
	TEXT("1 = draw the players' body regions (point, normal, name) and the ball's 19 regions."));

UBallInteractionComponent::UBallInteractionComponent() {
	PrimaryComponentTick.bCanEverTick = true; // only draws debug; does nothing when the debug CVars are off
}

void UBallInteractionComponent::BeginPlay() {
	Super::BeginPlay();
	BallState = GetOwner()->FindComponentByClass<UBallStateComponent>();
	BallMovement = GetOwner()->FindComponentByClass<UBallMovementComponent>();
	if (UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent())) {
		Body->OnComponentBeginOverlap.AddDynamic(this, &UBallInteractionComponent::OnBallBeginOverlap);
	}
	if (BallState) {
		BallState->OnBallStateChanged.AddDynamic(this, &UBallInteractionComponent::OnBallStateChanged);
	}
	// The arbitration point (spec §147, §148): once per frame, after every pre-physics tick, just before the step.
	if (FPhysScene_Chaos* Scene = GetWorld()->GetPhysicsScene()) {
		PreTickHandle = Scene->OnPhysScenePreTick.AddUObject(this, &UBallInteractionComponent::OnPhysScenePreTick);
	}
}

void UBallInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
	if (FPhysScene_Chaos* Scene = GetWorld() ? GetWorld()->GetPhysicsScene() : nullptr) {
		Scene->OnPhysScenePreTick.Remove(PreTickHandle);
	}
	PreTickHandle.Reset();
	PendingCommands.Reset();
	Super::EndPlay(EndPlayReason);
}

EContactPriority UBallInteractionComponent::ControllingStatePriority() const {
	if (!BallState) {
		return EContactPriority::None;
	}
	switch (BallState->GetState()) {
	case EBallState::Controlled: return EContactPriority::Dribble;
	case EBallState::Receiving: return EContactPriority::Receive;
	case EBallState::SkillControlled: return EContactPriority::Skill;
	case EBallState::GoalkeeperControlled: return EContactPriority::Capture;
	default: return EContactPriority::None;
	}
}

bool UBallInteractionComponent::SubmitResponse(const FBallResponseCommand& Command) {
	const int32 Best = PickWinningCommand(PendingCommands);
	const bool bLeads = Best == INDEX_NONE || uint8(Command.Priority) >= uint8(PendingCommands[Best].Priority);
	PendingCommands.Add(Command);
	return bLeads;
}

int32 UBallInteractionComponent::PickWinningCommand(const TArray<FBallResponseCommand>& Commands) {
	int32 Best = INDEX_NONE;
	for (int32 i = 0; i < Commands.Num(); ++i) {
		if (Best == INDEX_NONE || uint8(Commands[i].Priority) >= uint8(Commands[Best].Priority)) {
			Best = i;
		}
	}
	return Best;
}

void UBallInteractionComponent::GetPendingMotion(FVector& Velocity, FVector& AngularVelocityDeg) const {
	const UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
	Velocity = Body ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector;
	AngularVelocityDeg = Body ? Body->GetPhysicsAngularVelocityInDegrees() : FVector::ZeroVector;
	const int32 Winner = PickWinningCommand(PendingCommands);
	if (Winner == INDEX_NONE) {
		return;
	}
	const FBallResponseCommand& C = PendingCommands[Winner];
	if (C.bUseDesiredVelocity) {
		Velocity = C.MaxAllowedCorrection > 0.f ? Velocity + (C.DesiredVelocity - Velocity).GetClampedToMaxSize(C.MaxAllowedCorrection) : C.DesiredVelocity;
	}
	if (C.bSetAngularVelocity) {
		AngularVelocityDeg = C.AngularVelocityDeg;
	}
}

void UBallInteractionComponent::OnPhysScenePreTick(FPhysScene_Chaos* Scene, float DeltaTime) {
	// a command queued in the frame of a respawn (BP ResetBall) still lands after it; clear the queue there once
	// the respawn moves to C++ (master plan 7.2).
	if (PendingCommands.Num() == 0) {
		return;
	}
	const TArray<FBallResponseCommand> Commands = MoveTemp(PendingCommands);
	PendingCommands.Reset();
	const int32 Winner = PickWinningCommand(Commands);
	const UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
	const FVector Before = Body ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector;
	for (int32 i = 0; i < Commands.Num(); ++i) {
		if (i != Winner) {
			LogCommand(Commands[i], false, FString::Printf(TEXT("dropped: %s has priority"), *Commands[Winner].Source.ToString()), Before);
		}
	}
	ApplyCommand(Commands[Winner]);
	LogCommand(Commands[Winner], true, TEXT("applied"), Before);
}

void UBallInteractionComponent::ApplyCommand(const FBallResponseCommand& C) {
	UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
	if (!Body || !Body->IsSimulatingPhysics()) {
		return;
	}
	if (!C.LinearImpulse.IsZero()) {
		if (C.bImpulseAtContactPoint && !C.bVelocityChange) {
			Body->AddImpulseAtLocation(C.LinearImpulse, C.ContactPoint); // the engine adds the spin from the offset
		} else {
			Body->AddImpulse(C.LinearImpulse, NAME_None, C.bVelocityChange);
		}
	}
	if (!C.AngularImpulse.IsZero()) {
		Body->AddAngularImpulseInRadians(C.AngularImpulse, NAME_None, C.bVelocityChange);
	}
	if (C.bUseDesiredVelocity) {
		const FVector Current = Body->GetPhysicsLinearVelocity();
		Body->SetPhysicsLinearVelocity(C.MaxAllowedCorrection > 0.f
			? Current + (C.DesiredVelocity - Current).GetClampedToMaxSize(C.MaxAllowedCorrection) : C.DesiredVelocity);
	}
	if (C.bSetAngularVelocity) {
		Body->SetPhysicsAngularVelocityInDegrees(C.AngularVelocityDeg);
	}
	if (C.bSetStateAfter && BallState) {
		BallState->SetState(C.BallStateAfter, C.OwnerAfter);
	}
}

void UBallInteractionComponent::LogCommand(const FBallResponseCommand& C, bool bApplied, const FString& Note, const FVector& VelocityBefore) {
	FBallCommandRecord& Record = CommandLog.AddDefaulted_GetRef();
	Record.Time = float(GetWorld()->GetTimeSeconds());
	Record.Source = C.Source;
	Record.Instigator = GetNameSafe(C.Instigator);
	Record.ResponseType = C.ResponseType;
	Record.Priority = C.Priority;
	Record.bApplied = bApplied;
	Record.Note = Note;
	Record.LinearImpulse = C.LinearImpulse;
	Record.DesiredVelocity = C.DesiredVelocity;
	Record.VelocityBefore = VelocityBefore;
	if (CommandLog.Num() > MaxHistory) {
		CommandLog.RemoveAt(0);
	}
	if (CVarFootballDebugPhysics.GetValueOnGameThread() != 0) {
		const FVector Center = GetOwner()->GetActorLocation();
		if (bApplied) {
			const FVector Change = C.bUseDesiredVelocity ? C.DesiredVelocity - VelocityBefore : C.LinearImpulse;
			DrawDebugDirectionalArrow(GetWorld(), Center, Center + Change * 0.05, 6.f, FColor::Green, false, 1.f);
		}
		DrawDebugString(GetWorld(), Center + FVector(0.0, 0.0, bApplied ? 40.0 : 55.0), FString::Printf(TEXT("%s %s: %s"),
			*C.Source.ToString(), *UEnum::GetDisplayValueAsText(C.Priority).ToString(), *Note), nullptr, bApplied ? FColor::Green : FColor::Red, 1.f);
	}
}

EContactPriority UBallInteractionComponent::GetHeldPriority() const {
	const EContactPriority Frame = ClaimFrame == GFrameCounter ? ClaimPriority : EContactPriority::None;
	const EContactPriority State = ControllingStatePriority();
	return uint8(Frame) >= uint8(State) ? Frame : State;
}

bool UBallInteractionComponent::TryClaim(EContactPriority Priority, AActor* Instigator) {
	AActor* Holder = BallState ? BallState->GetBallOwner() : nullptr;
	EContactPriority Best = EContactPriority::None;
	AActor* BestBy = nullptr;
	if (ClaimFrame == GFrameCounter) {
		Best = ClaimPriority;
		BestBy = ClaimInstigator.Get();
	}
	// The controller of a Controlled / SkillControlled ball keeps that priority across frames, against everyone else.
	const EContactPriority Held = ControllingStatePriority();
	if (Instigator != Holder && uint8(Held) > uint8(Best)) {
		Best = Held;
		BestBy = Holder;
	}
	const bool bWins = uint8(Priority) > uint8(Best)
		|| (Priority == Best && (BestBy == nullptr || Instigator == BestBy || Instigator == Holder));
	if (!bWins) {
		return false;
	}
	ClaimFrame = GFrameCounter;
	ClaimPriority = Priority;
	ClaimInstigator = Instigator;
	return true;
}

void UBallInteractionComponent::OnBallStateChanged(EBallState OldState, EBallState NewState, AActor* Instigator) {
	const bool bWasHeld = OldState == EBallState::Controlled || OldState == EBallState::SkillControlled
		|| OldState == EBallState::Passing || OldState == EBallState::Shooting || OldState == EBallState::GoalkeeperControlled;
	const bool bNowLoose = NewState == EBallState::Free || NewState == EBallState::Airborne || NewState == EBallState::Deflected;
	if (bWasHeld && bNowLoose) {
		OwnerReleaseTime = GetWorld()->GetTimeSeconds();
	}
}

void UBallInteractionComponent::OnBallBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult) {
	USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(OtherComp);
	UPlayerBallInteractionComponent* Character = OtherActor ? OtherActor->FindComponentByClass<UPlayerBallInteractionComponent>() : nullptr;
	if (!Mesh || !Character || !BallState || !BallMovement || !OverlappedComponent) {
		return;
	}
	// Which bone's body the ball touched (needs multi-body overlaps on the mesh).
	FName Bone = NAME_None;
	if (Mesh->Bodies.IsValidIndex(OtherBodyIndex) && Mesh->Bodies[OtherBodyIndex]) {
		Bone = Mesh->GetBoneName(Mesh->Bodies[OtherBodyIndex]->InstanceBoneIndex);
	}
	const FVector Center = OverlappedComponent->GetComponentLocation();
	EBodyRegion Region;
	FVector RegionPoint, RegionNormal;
	const FBodyRegionRow* Row = nullptr;
	if (!Character->FindContactRegion(Bone, Center, Region, RegionPoint, RegionNormal, Row)) {
		return;
	}
	// Contact point and normal from the touched body itself where possible; the region gives the response.
	FVector BodyPoint = RegionPoint;
	FVector Normal = RegionNormal;
	FVector Closest;
	if (Mesh->GetClosestPointOnCollision(Center, Closest, Bone) > 0.f) {
		BodyPoint = Closest;
		Normal = (Center - Closest).GetSafeNormal();
	}
	FVector VelocityIn, SpinIn;
	GetPendingMotion(VelocityIn, SpinIn); // a second contact in this frame responds to the first one's result
	const FVector BodyVelocity = OtherActor->GetVelocity();
	const EContactPriority Priority = UBallContactResolver::IsFootRegion(Region) ? EContactPriority::Foot : EContactPriority::Body;

	// The owner's own body never knocks the ball it is holding, or one it has just let go of.
	if (OtherActor == BallState->GetBallOwner()) {
		const EBallState State = BallState->GetState();
		const bool bHeld = State == EBallState::Controlled || State == EBallState::SkillControlled
			|| State == EBallState::Passing || State == EBallState::Shooting || State == EBallState::GoalkeeperControlled;
		if (bHeld || GetWorld()->GetTimeSeconds() - OwnerReleaseTime < OwnerGraceTime) {
			RecordContact(OtherActor, Region, Priority, false, TEXT("own body, ignored"), BodyPoint, Normal, Row->Radius, VelocityIn, VelocityIn);
			return;
		}
	}
	const double Closing = -FVector::DotProduct(VelocityIn - BodyVelocity, Normal);
	if (Closing < MinDeflectSpeed) {
		RecordContact(OtherActor, Region, Priority, false, TEXT("resting contact, ignored"), BodyPoint, Normal, Row->Radius, VelocityIn, VelocityIn);
		return;
	}
	if (!TryClaim(Priority, OtherActor)) {
		RecordContact(OtherActor, Region, Priority, false, TEXT("lower priority than the ball's controller"), BodyPoint, Normal, Row->Radius, VelocityIn, VelocityIn);
		return;
	}
	const USphereComponent* Sphere = Cast<USphereComponent>(OverlappedComponent);
	const float Radius = Sphere ? Sphere->GetScaledSphereRadius() : 11.f;
	FVector SpinOut;
	const FVector VelocityOut = UBallContactResolver::ComputeDeflection(VelocityIn, BodyVelocity, Normal, Row->Restitution, Row->TangentDamping,
		SpinIn, Row->SpinTransfer, Radius, SpinOut);
	BallMovement->ApplyDeflection(VelocityOut, SpinOut, OtherActor);
	RecordContact(OtherActor, Region, Priority, true, TEXT("deflected"), BodyPoint, Normal, Row->Radius, VelocityIn, VelocityOut);
}

void UBallInteractionComponent::RecordContact(AActor* Instigator, EBodyRegion Region, EContactPriority Priority, bool bApplied, const TCHAR* Note,
	const FVector& BodyPoint, const FVector& Normal, float ContactRadius, const FVector& VelocityIn, const FVector& VelocityOut) {
	const USphereComponent* Sphere = Cast<USphereComponent>(GetOwner()->GetRootComponent());
	const FVector Center = GetOwner()->GetActorLocation();
	LastContact.Time = float(GetWorld()->GetTimeSeconds());
	LastContact.Instigator = GetNameSafe(Instigator);
	LastContact.Region = Region;
	LastContact.Priority = Priority;
	LastContact.bApplied = bApplied;
	LastContact.Note = Note;
	LastContact.BodyPoint = BodyPoint;
	LastContact.Normal = Normal;
	LastContact.BallSurfacePoint = Center - Normal * (Sphere ? Sphere->GetScaledSphereRadius() : 11.f);
	LastContact.ContactRadius = ContactRadius;
	LastContact.VelocityIn = VelocityIn;
	LastContact.VelocityOut = VelocityOut;
	LastContact.BallStateAfter = BallState ? BallState->GetState() : EBallState::Free;
	ContactHistory.Add(LastContact);
	if (ContactHistory.Num() > MaxHistory) {
		ContactHistory.RemoveAt(0);
	}
	if (CVarFootballDebugContacts.GetValueOnGameThread() != 0) {
		// Doc §33: player contact point, ball centre, calculated ball surface point, contact normal, contact
		// radius, ball velocity in and out, ball state and contact time.
		const UWorld* World = GetWorld();
		const FQuat Facing = Normal.ToOrientationQuat();
		DrawDebugSphere(World, BodyPoint, 2.f, 8, FColor::Red, false, 2.f);
		DrawDebugCircle(World, BodyPoint, ContactRadius, 16, FColor::Red, false, 2.f, 0, 0.f, Facing.GetAxisY(), Facing.GetAxisZ(), false);
		DrawDebugDirectionalArrow(World, BodyPoint, BodyPoint + Normal * 20.f, 6.f, FColor::Blue, false, 2.f);
		DrawDebugSphere(World, Center, 11.f, 12, FColor::White, false, 2.f);
		DrawDebugPoint(World, LastContact.BallSurfacePoint, 8.f, FColor::Magenta, false, 2.f);
		DrawDebugDirectionalArrow(World, Center, Center + VelocityIn * 0.05f, 6.f, FColor::Yellow, false, 2.f);
		if (bApplied) {
			DrawDebugDirectionalArrow(World, Center, Center + VelocityOut * 0.05f, 6.f, FColor::Green, false, 2.f);
		}
		DrawDebugString(World, Center + FVector(0.f, 0.f, 25.f), FString::Printf(TEXT("t=%.2f %s %s: %s -> %s"), LastContact.Time,
			*UEnum::GetDisplayValueAsText(Region).ToString(), *UEnum::GetDisplayValueAsText(Priority).ToString(), Note,
			*UEnum::GetDisplayValueAsText(LastContact.BallStateAfter).ToString()), nullptr, FColor::White, 2.f);
	}
}

void UBallInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (CVarFootballDebugBall.GetValueOnGameThread() != 0) {
		if (const UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent())) {
			const FVector Center = GetOwner()->GetActorLocation();
			const FVector Velocity = Body->GetPhysicsLinearVelocity();
			const FVector Spin = Body->GetPhysicsAngularVelocityInDegrees();
			DrawDebugDirectionalArrow(GetWorld(), Center, Center + Velocity * 0.05, 6.f, FColor::Yellow);
			DrawDebugLine(GetWorld(), Center, Center + Spin * 0.02, FColor::Magenta, false, -1.f, 0, 1.f);
			DrawDebugString(GetWorld(), Center + FVector(0.0, 0.0, 25.0), FString::Printf(TEXT("%s  %.0f cm/s  %.1f rev/s"),
				BallState ? *UEnum::GetDisplayValueAsText(BallState->GetState()).ToString() : TEXT("-"), Velocity.Size(), Spin.Size() / 360.0),
				nullptr, FColor::White, 0.f);
		}
	}
	if (CVarFootballDebugRegions.GetValueOnGameThread() == 0) {
		return;
	}
	// The 19 ball regions in the owner's facing (world X if nobody owns the ball).
	const AActor* Owner = BallState ? BallState->GetBallOwner() : nullptr;
	const FVector Center = GetOwner()->GetActorLocation();
	const FSkillBasis Basis = UBallContactResolver::MakeSkillBasis(Center, Owner ? Owner->GetActorForwardVector() : FVector::ForwardVector, false);
	const UEnum* Enum = StaticEnum<EBallRegion>();
	for (int32 i = 0; i < Enum->NumEnums() - 1; ++i) {
		const EBallRegion Region = EBallRegion(Enum->GetValueByIndex(i));
		const FVector Point = UBallContactResolver::BallSurfacePoint(Center, 11.f, Region, Basis);
		DrawDebugPoint(GetWorld(), Point, 6.f, Region == EBallRegion::Front ? FColor::Green : FColor::Cyan);
	}
}
