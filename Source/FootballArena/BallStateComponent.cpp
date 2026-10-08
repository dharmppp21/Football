#include "BallStateComponent.h"
#include "Engine/World.h"
#include "PlayerBallInteractionComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogFootballBall, Log, All);

static bool TakesTheBall(EBallState State) {
	return State == EBallState::Controlled || State == EBallState::SkillControlled || State == EBallState::Receiving
		|| State == EBallState::Passing || State == EBallState::Shooting || State == EBallState::GoalkeeperControlled;
}

bool UBallStateComponent::IsTransitionAllowed(EBallState From, EBallState To) {
	switch (To) {
	case EBallState::SkillControlled:
		// A skill only takes a ball at the feet, a loose one, or one being received; never one in flight from a strike.
		return From == EBallState::Free || From == EBallState::Controlled || From == EBallState::Receiving || From == EBallState::Airborne;
	case EBallState::Receiving:
		// Nobody receives a ball that is already under control.
		return From != EBallState::Controlled && From != EBallState::SkillControlled && From != EBallState::GoalkeeperControlled;
	case EBallState::GoalkeeperControlled:
		// A catch is physical -> receiving -> captured (spec §128).
		return From == EBallState::Receiving;
	case EBallState::Controlled:
		// The dribble never picks up a ball a goalkeeper holds.
		return From != EBallState::GoalkeeperControlled;
	default:
		// Free / Airborne / Deflected: physics can always take over. Controlled: the dribble picks up any loose ball
		// (as the Blueprint possession does today, airborne included). Passing / Shooting: any strike, first-time included.
		return true;
	}
}

bool UBallStateComponent::SetState(EBallState NewState, AActor* Instigator) {
	const bool bTakes = Instigator && TakesTheBall(NewState);
	if (NewState == State) {
		// A second skill must not grab a ball another skill is moving (doc §11).
		if (State == EBallState::SkillControlled && Instigator && Instigator != BallOwner.Get()) {
			UE_LOG(LogFootballBall, Warning, TEXT("%s refused: the ball is SkillControlled by %s"), *GetNameSafe(Instigator), *GetNameSafe(BallOwner.Get()));
			return false;
		}
		if (bTakes) {
			SetBallOwner(Instigator);
		}
		return true;
	}
	if (!IsTransitionAllowed(State, NewState)) {
		UE_LOG(LogFootballBall, Warning, TEXT("Ball state %s -> %s refused (by %s)"),
			*UEnum::GetValueAsString(State), *UEnum::GetValueAsString(NewState), *GetNameSafe(Instigator));
		return false;
	}
	const EBallState Old = State;
	State = NewState;
	StateStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (bTakes) {
		SetBallOwner(Instigator);
	}
	OnBallStateChanged.Broadcast(Old, NewState, Instigator);
	return true;
}

void UBallStateComponent::ResetState() {
	const EBallState Old = State;
	State = EBallState::Free;
	StateStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	SetBallOwner(nullptr);
	if (Old != EBallState::Free) {
		OnBallStateChanged.Broadcast(Old, EBallState::Free, nullptr);
	}
}

float UBallStateComponent::GetTimeInState() const {
	return GetWorld() ? float(GetWorld()->GetTimeSeconds() - StateStartTime) : 0.f;
}

void UBallStateComponent::SetBallOwner(AActor* NewOwner) {
	EBallOwnerType NewType = EBallOwnerType::None;
	if (NewOwner) {
		const UPlayerBallInteractionComponent* Interaction = NewOwner->FindComponentByClass<UPlayerBallInteractionComponent>();
		NewType = Interaction ? Interaction->OwnerType : EBallOwnerType::Player;
	}
	if (BallOwner.Get() == NewOwner && OwnerType == NewType) {
		return;
	}
	BallOwner = NewOwner;
	OwnerType = NewType;
	OnBallOwnerChanged.Broadcast(NewOwner, NewType);
}
