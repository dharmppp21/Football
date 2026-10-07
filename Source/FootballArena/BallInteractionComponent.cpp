#include "BallInteractionComponent.h"
#include "BallContactResolver.h"
#include "BallMovementComponent.h"
#include "BallStateComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PlayerBallInteractionComponent.h"

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
}

EContactPriority UBallInteractionComponent::ControllingStatePriority() const {
	if (!BallState) {
		return EContactPriority::None;
	}
	switch (BallState->GetState()) {
	case EBallState::Controlled: return EContactPriority::Dribble;
	case EBallState::SkillControlled:
	case EBallState::Receiving: return EContactPriority::Skill;
	default: return EContactPriority::None;
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
		|| OldState == EBallState::Passing || OldState == EBallState::Shooting;
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
	const FVector VelocityIn = OverlappedComponent->GetPhysicsLinearVelocity();
	const FVector BodyVelocity = OtherActor->GetVelocity();
	const EContactPriority Priority = UBallContactResolver::IsFootRegion(Region) ? EContactPriority::Foot : EContactPriority::Body;

	// The owner's own body never knocks the ball it is holding, or one it has just let go of.
	if (OtherActor == BallState->GetBallOwner()) {
		const EBallState State = BallState->GetState();
		const bool bHeld = State == EBallState::Controlled || State == EBallState::SkillControlled
			|| State == EBallState::Passing || State == EBallState::Shooting;
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
		OverlappedComponent->GetPhysicsAngularVelocityInDegrees(), Row->SpinTransfer, Radius, SpinOut);
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
