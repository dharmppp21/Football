#include "BallContactResolver.h"

FSkillBasis UBallContactResolver::MakeSkillBasis(FVector Origin, FVector Facing, bool bMirrored) {
	FSkillBasis Basis;
	Basis.Origin = Origin;
	const FVector Flat = FVector(Facing.X, Facing.Y, 0.0).GetSafeNormal();
	Basis.Forward = Flat.IsNearlyZero() ? FVector::ForwardVector : Flat;
	Basis.Up = FVector::UpVector;
	Basis.Right = FVector::CrossProduct(Basis.Up, Basis.Forward);
	Basis.bMirrored = bMirrored;
	return Basis;
}

FSkillBasis UBallContactResolver::MakeSkillBasisFromInput(FVector Origin, FVector Facing, FVector InputDirection, int32 AllowedDirections,
	bool bMirrored, ESkillDirection& OutDirection) {
	const FSkillBasis Facing0 = MakeSkillBasis(Origin, Facing, bMirrored);
	OutDirection = ESkillDirection::Forward;
	const FVector Input(InputDirection.X, InputDirection.Y, 0.0);
	if (Input.Size() < 0.3) {
		return Facing0;
	}
	// Angle of the input from the facing, + = toward the right; the eight directions are 45 degrees apart from Forward.
	const double Angle = FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(Input, Facing0.Right), FVector::DotProduct(Input, Facing0.Forward)));
	double Turn = 0.0;
	if (AllowedDirections & (1 << int32(ESkillDirection::Custom))) {
		Turn = Angle;
		OutDirection = ESkillDirection::Custom;
	} else {
		double Best = TNumericLimits<double>::Max();
		for (int32 i = 0; i < 8; ++i) {
			const double Diff = FMath::Abs(FMath::FindDeltaAngleDegrees(Angle, i * 45.0));
			if ((AllowedDirections & (1 << i)) && Diff < Best) {
				Best = Diff;
				Turn = i * 45.0;
				OutDirection = ESkillDirection(i);
			}
		}
	}
	return MakeSkillBasis(Origin, Facing0.Forward.RotateAngleAxis(Turn, FVector::UpVector), bMirrored);
}

FVector UBallContactResolver::SkillToWorld(const FSkillBasis& Basis, FVector Local) {
	return Basis.TransformVector(Local);
}

FVector UBallContactResolver::RegionDirection(EBallRegion Region, const FSkillBasis& Basis) {
	// Named axes of each region: forward, right, up.
	double F = 0.0, R = 0.0, U = 0.0;
	switch (Region) {
	case EBallRegion::Center: break;
	case EBallRegion::Front: F = 1; break;
	case EBallRegion::Back: F = -1; break;
	case EBallRegion::Left: R = -1; break;
	case EBallRegion::Right: R = 1; break;
	case EBallRegion::Top: U = 1; break;
	case EBallRegion::Bottom: U = -1; break;
	case EBallRegion::FrontLeft: F = 1; R = -1; break;
	case EBallRegion::FrontRight: F = 1; R = 1; break;
	case EBallRegion::BackLeft: F = -1; R = -1; break;
	case EBallRegion::BackRight: F = -1; R = 1; break;
	case EBallRegion::TopFront: U = 1; F = 1; break;
	case EBallRegion::TopBack: U = 1; F = -1; break;
	case EBallRegion::TopLeft: U = 1; R = -1; break;
	case EBallRegion::TopRight: U = 1; R = 1; break;
	case EBallRegion::BottomFront: U = -1; F = 1; break;
	case EBallRegion::BottomBack: U = -1; F = -1; break;
	case EBallRegion::BottomLeft: U = -1; R = -1; break;
	case EBallRegion::BottomRight: U = -1; R = 1; break;
	}
	return Basis.TransformVector(FVector(F, R, U)).GetSafeNormal();
}

FVector UBallContactResolver::BallSurfacePoint(FVector BallCenter, float BallRadius, EBallRegion Region, const FSkillBasis& Basis) {
	return BallCenter + RegionDirection(Region, Basis) * BallRadius;
}

// Swaps the "Left" and "Right" parts of an enum value's name (FrontLeft <-> FrontRight, LeftHeel <-> RightHeel,
// ChestLeft <-> ChestRight); values without a side map to themselves.
template <typename TEnum>
static TEnum MirrorByName(TEnum Value) {
	const UEnum* Enum = StaticEnum<TEnum>();
	FString Name = Enum->GetNameStringByValue(int64(Value));
	if (Name.Contains(TEXT("Left"))) {
		Name = Name.Replace(TEXT("Left"), TEXT("Right"));
	} else if (Name.Contains(TEXT("Right"))) {
		Name = Name.Replace(TEXT("Right"), TEXT("Left"));
	} else {
		return Value;
	}
	const int64 Mirrored = Enum->GetValueByNameString(Name);
	return Mirrored == INDEX_NONE ? Value : TEnum(Mirrored);
}

EBallRegion UBallContactResolver::MirrorBallRegion(EBallRegion Region) {
	return MirrorByName(Region);
}

EBodyRegion UBallContactResolver::MirrorBodyRegion(EBodyRegion Region) {
	return MirrorByName(Region);
}

bool UBallContactResolver::IsFootRegion(EBodyRegion Region) {
	const FString Name = StaticEnum<EBodyRegion>()->GetNameStringByValue(int64(Region));
	return Name.Contains(TEXT("Foot")) || Name.Contains(TEXT("Toe")) || Name.Contains(TEXT("Heel"))
		|| Name.Contains(TEXT("Instep")) || Name.Contains(TEXT("Sole"));
}

static bool IsLaunchingContact(EContactType Type) {
	switch (Type) {
	case EContactType::Redirect:
	case EContactType::Strike:
	case EContactType::Pass:
	case EContactType::Shot:
	case EContactType::Header:
	case EContactType::Deflection:
	case EContactType::Release:
		return true;
	default:
		return false;
	}
}

FContactResult UBallContactResolver::ResolveContact(const FContactQuery& Query) {
	const FBallContactDescriptor& D = Query.Descriptor;
	FContactResult Result;
	Result.BodyPoint = Query.BodyPoint;
	const FVector RegionDir = RegionDirection(D.BallContactRegion, Query.Basis);
	Result.SurfacePoint = Query.BallCenter + RegionDir * Query.BallRadius;
	// The ball centre that puts the touched side of the ball on the body point, plus the tuning nudge.
	Result.TargetBallCenter = Query.BodyPoint - RegionDir * Query.BallRadius + Query.Basis.TransformVector(D.LocalBallOffset);
	Result.Distance = float(FVector::Dist(Query.BallCenter, Result.TargetBallCenter));
	const FVector BodyToBall = Query.BallCenter - Query.BodyPoint;
	Result.ContactNormal = BodyToBall.IsNearlyZero(1.0) ? Query.BodyNormal.GetSafeNormal() : BodyToBall.GetSafeNormal();
	Result.RelativeVelocity = Query.BodyVelocity - Query.BallVelocity;
	Result.NormalSpeed = float(FVector::DotProduct(Result.RelativeVelocity, Result.ContactNormal));
	Result.TangentVelocity = Result.RelativeVelocity - Result.ContactNormal * Result.NormalSpeed;

	// T3: the animation / gameplay must allow this contact now.
	if (!Query.bWindowOpen) {
		Result.Validation = EContactValidation::OutsideWindow;
		return Result;
	}
	// T1: close enough (exact equality is never required, doc §32).
	if (Result.Distance > D.ContactTolerance) {
		Result.Validation = EContactValidation::TooFar;
		return Result;
	}
	// T4: the ball sits on the side the named body surface faces (within 60 degrees).
	if (!BodyToBall.IsNearlyZero(1.0) && FVector::DotProduct(Query.BodyNormal.GetSafeNormal(), BodyToBall.GetSafeNormal()) < 0.5) {
		Result.Validation = EContactValidation::WrongRegion;
		return Result;
	}
	// T2: body and ball are closing on each other. A resting contact (no relative motion) passes; T5 can
	// reject it when a contact needs a moving body.
	const FVector Closing = Query.BodyVelocity - Query.BallVelocity;
	if (Closing.Size() > 1.0 && !BodyToBall.IsNearlyZero(1.0)
		&& FVector::DotProduct(Closing.GetSafeNormal(), BodyToBall.GetSafeNormal()) <= D.MinApproachDot) {
		Result.Validation = EContactValidation::WrongDirection;
		return Result;
	}
	// T5: optional minimum body speed (no accidental contact from an almost still body).
	if (D.MinBodySpeed > 0.f && Query.BodyVelocity.Size() < D.MinBodySpeed) {
		Result.Validation = EContactValidation::TooSlow;
		return Result;
	}
	Result.Validation = EContactValidation::Valid;
	if (IsLaunchingContact(D.ContactType)) {
		Result.ResponseVelocity = ComputeExitVelocity(D, Query.Basis, Query.PlayerVelocity, Query.BallVelocity);
	}
	return Result;
}

void UBallContactResolver::SweepRegionToBall(FVector BodyPoint, FVector BodyVelocity, float BodyRadius, FVector BallCenter, FVector BallVelocity, float BallRadius,
	float DeltaTime, float& OutClosestApproach, float& OutSweepAlpha, FVector& OutContactPoint) {
	// Relative to the ball the region moves along S(a) = Now - Step * (1 - a), a in [0, 1] (a tick ago -> now).
	const double Reach = double(BallRadius) + double(BodyRadius);
	const FVector Now = BodyPoint - BallCenter;
	const FVector Step = (BodyVelocity - BallVelocity) * double(DeltaTime);
	const FVector Start = Now - Step;
	const double StepSq = Step.SizeSquared();
	const double Closest = StepSq > UE_SMALL_NUMBER ? FMath::Clamp(-FVector::DotProduct(Start, Step) / StepSq, 0.0, 1.0) : 1.0;
	OutClosestApproach = float((Start + Step * Closest).Size() - Reach);
	double Alpha = Closest;
	if (OutClosestApproach <= 0.f && Start.SizeSquared() > Reach * Reach) {
		// Touching: where the path first enters the sphere, the smaller root of |Start + Step a| = Reach (it lies in
		// [0, Closest] because Start is outside and the closest point inside).
		const double B = FVector::DotProduct(Start, Step);
		const double C = Start.SizeSquared() - Reach * Reach;
		Alpha = FMath::Clamp((-B - FMath::Sqrt(FMath::Max(B * B - StepSq * C, 0.0))) / StepSq, 0.0, Closest);
	} else if (OutClosestApproach <= 0.f) {
		Alpha = 0.0; // already on the ball a tick ago
	}
	OutSweepAlpha = float(Alpha);
	const FVector Offset = Start + Step * Alpha;
	const FVector BallThen = BallCenter - BallVelocity * (double(DeltaTime) * (1.0 - Alpha));
	OutContactPoint = BallThen + (Offset.IsNearlyZero() ? FVector::UpVector : Offset.GetSafeNormal()) * double(BallRadius);
}

void UBallContactResolver::TrackRegion(FContactTracker& Tracker, EBodyRegion Region, FVector Point, FVector Normal, float DeltaTime) {
	const bool bSameRegion = Tracker.bTracking && Tracker.Region == Region;
	Tracker.Velocity = bSameRegion && DeltaTime > 0.f ? (Point - Tracker.Point) / DeltaTime : FVector::ZeroVector;
	Tracker.PrevPoint = bSameRegion ? Tracker.Point : Point;
	Tracker.Region = Region;
	Tracker.Point = Point;
	Tracker.Normal = Normal;
	Tracker.bTracking = true;
}

void UBallContactResolver::StopTracking(FContactTracker& Tracker) {
	Tracker.bTracking = false;
	Tracker.Velocity = FVector::ZeroVector;
}

void UBallContactResolver::OpenContactWindow(FContactTracker& Tracker) {
	Tracker.bWaitSeparation = Tracker.bProcessed && Tracker.bTouching;
	Tracker.bProcessed = Tracker.bWaitSeparation;
	Tracker.bTouchedInWindow = false;
	Tracker.Recontacts = 0;
	Tracker.ImpulseUsed = 0.f;
}

void UBallContactResolver::UpdateContactState(FContactTracker& Tracker, float ClosestApproach, float DeltaTime, const FBallContactDescriptor& Descriptor,
	float MinSeparationTime) {
	const bool bTouchingNow = ClosestApproach <= 0.f;
	if (bTouchingNow) {
		if (!Tracker.bTouching && Tracker.bTouchedInWindow && Tracker.SeparatedFor >= MinSeparationTime) {
			++Tracker.Recontacts;
			if (Descriptor.bAllowRecontact) {
				Tracker.bProcessed = false;
			}
		}
		Tracker.bTouchedInWindow = true;
		Tracker.SeparatedFor = 0.f;
	} else {
		Tracker.SeparatedFor += DeltaTime;
		if (Tracker.bWaitSeparation && Tracker.SeparatedFor >= MinSeparationTime) {
			Tracker.bWaitSeparation = false;
			Tracker.bProcessed = false;
		}
	}
	Tracker.bTouching = bTouchingNow;
}

bool UBallContactResolver::CanProcessContact(const FContactTracker& Tracker, const FBallContactDescriptor& Descriptor) {
	return Descriptor.bProcessOnce ? !Tracker.bProcessed : Tracker.bTouching;
}

void UBallContactResolver::MarkContactProcessed(FContactTracker& Tracker) {
	Tracker.bProcessed = true;
}

FVector UBallContactResolver::ComputeExitVelocity(const FBallContactDescriptor& Descriptor, const FSkillBasis& Basis, FVector PlayerVelocity, FVector BallVelocity) {
	FVector Direction = Basis.TransformVector(Descriptor.PreferredDirection).GetSafeNormal();
	if (Direction.IsNearlyZero()) {
		Direction = Basis.Forward;
	}
	return Direction * Descriptor.PreferredSpeed
		+ Basis.Up * (Descriptor.PreferredSpeed * Descriptor.VerticalBias)
		+ PlayerVelocity * Descriptor.InheritPlayerVelocity
		+ BallVelocity * Descriptor.InheritBallVelocity;
}

FVector UBallContactResolver::ComputeDeflection(FVector BallVelocity, FVector BodyVelocity, FVector Normal, float Restitution, float TangentDamping,
	FVector BallSpinDeg, float SpinTransfer, float BallRadius, FVector& OutSpinDeg) {
	OutSpinDeg = BallSpinDeg;
	const FVector N = Normal.GetSafeNormal();
	const FVector Relative = BallVelocity - BodyVelocity;
	const double Approach = FVector::DotProduct(Relative, N);
	if (N.IsNearlyZero() || Approach >= 0.0) {
		return BallVelocity; // moving away from (or along) the surface already
	}
	const FVector NormalPart = N * Approach;
	const FVector Slide = Relative - NormalPart;
	const FVector NewRelative = Slide * (1.0 - TangentDamping) - NormalPart * Restitution;
	if (SpinTransfer > 0.f && BallRadius > 0.f) {
		// Rolling-style spin from the sliding speed: omega = (n x slide) / r, in degrees per second.
		OutSpinDeg = BallSpinDeg * (1.0 - TangentDamping)
			+ FVector::CrossProduct(N, Slide) / BallRadius * (180.0 / PI) * SpinTransfer;
	}
	return BodyVelocity + NewRelative;
}
