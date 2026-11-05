#include "BallAssist.h"
#include "Curves/CurveFloat.h"
#include "UObject/UnrealType.h"

FBallAssistResult UBallAssist::BoundedBlend(const FBallAssistInput& In) {
	FBallAssistResult Out;
	Out.StrengthUsed = FMath::Clamp(In.Strength, 0.f, In.bArcade ? 1.f : MaxStrengthWithoutArcade);
	Out.bStrengthCapped = Out.StrengthUsed < In.Strength;
	const double Strength = Out.StrengthUsed;
	const double RawSpeed = In.RawVelocity.Size();
	const FVector Want = In.IntendedDirection.GetSafeNormal();
	FVector Dir = RawSpeed > UE_KINDA_SMALL_NUMBER ? In.RawVelocity / RawSpeed : Want;
	if (RawSpeed > UE_KINDA_SMALL_NUMBER && !Want.IsZero()) {
		const double Error = FMath::Acos(FMath::Clamp(FVector::DotProduct(Dir, Want), -1.0, 1.0));
		const double Turn = FMath::Min(Strength * Error, double(FMath::DegreesToRadians(FMath::Max(In.MaxAngleDegrees, 0.f))));
		if (Turn > 0.0) {
			// About the axis that carries the raw direction onto the intended one (any perpendicular when they are opposite).
			FVector Axis = FVector::CrossProduct(Dir, Want).GetSafeNormal();
			if (Axis.IsZero()) {
				FVector Unused;
				Dir.FindBestAxisVectors(Axis, Unused);
			}
			Dir = Dir.RotateAngleAxisRad(Turn, Axis);
			Out.AngleCorrectionDegrees = float(FMath::RadiansToDegrees(Turn));
		}
	}
	const double SpeedGap = In.IntendedSpeed > 0.f ? In.IntendedSpeed - RawSpeed : 0.0;
	const double MaxChange = FMath::Max(In.MaxSpeedChange, 0.f);
	Out.SpeedCorrection = float(FMath::Clamp(Strength * SpeedGap, -MaxChange, MaxChange));
	Out.Velocity = Dir * FMath::Max(RawSpeed + Out.SpeedCorrection, 0.0);
	Out.VelocityChange = Out.Velocity - In.RawVelocity;
	return Out;
}

FBallAssistInput UBallAssist::ScaleForSkill(FBallAssistInput Input, float Skill, const UCurveFloat* SkillToScale) {
	const float Scale = SkillToScale ? FMath::Max(SkillToScale->GetFloatValue(Skill), 0.f) : 1.f;
	Input.Strength *= Scale;
	Input.MaxAngleDegrees *= Scale;
	Input.MaxSpeedChange *= Scale;
	return Input;
}

float UBallAssist::GetAttribute(const FFootballAttributes& Attributes, FName Name) {
	const FFloatProperty* Property = CastField<FFloatProperty>(FFootballAttributes::StaticStruct()->FindPropertyByName(Name));
	return Property ? Property->GetPropertyValue_InContainer(&Attributes) : -1.f;
}
