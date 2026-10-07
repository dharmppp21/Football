#include "FootballSkillData.h"
#include "Animation/AnimMontage.h"
#include "BallContactResolver.h"

float UFootballSkillData::GetDuration() const {
	return Montage ? Montage->GetPlayLength() / FMath::Max(PlayRate * Montage->RateScale, KINDA_SMALL_NUMBER) : 0.f;
}

FBallContactDescriptor UFootballSkillData::GetTunedDescriptor(int32 Index, bool bMirrored) const {
	if (!ContactDescriptors.IsValidIndex(Index)) {
		return FBallContactDescriptor();
	}
	FBallContactDescriptor D = ContactDescriptors[Index];
	if (bMirrored) {
		// The ball region and the local vectors are in skill space, which the mirrored basis flips already.
		D.BodyContactRegion = UBallContactResolver::MirrorBodyRegion(D.BodyContactRegion);
	}
	if (ContactTolerance > 0.f) {
		D.ContactTolerance = ContactTolerance;
	}
	if (D.ContactType == EContactType::Release) {
		if (ExitSpeed > 0.f) {
			D.PreferredSpeed = ExitSpeed;
		}
		D.PreferredDirection = D.PreferredDirection.RotateAngleAxis(ExitAngle, FVector::UpVector);
	}
	return D;
}
